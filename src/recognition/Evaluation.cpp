#include "Evaluation.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <regex>
#include <sstream>

namespace EchoRadar {
namespace {

std::string Extract(const std::string& line, const std::string& key) {
    const std::regex pattern("\\\"" + key + "\\\"\\s*:\\s*(\\\"([^\\\"]*)\\\"|[-+0-9.eE]+|true|false)");
    std::smatch match;
    if (!std::regex_search(line, match, pattern)) return {};
    return match[2].matched ? match[2].str() : match[1].str();
}

uint64_t U64(const std::string& value) {
    try { return std::stoull(value); } catch (...) { return 0; }
}

float Float(const std::string& value) {
    try { return std::stof(value); } catch (...) { return 0.0f; }
}

bool Bool(const std::string& value) {
    return value == "true" || value == "1";
}

std::pair<double, double> WilsonInterval(uint64_t successes, uint64_t total) {
    if (total == 0) return {0.0, 1.0};
    constexpr double z = 1.959963984540054;
    const double count = static_cast<double>(total);
    const double proportion = static_cast<double>(successes) / count;
    const double denominator = 1.0 + z * z / count;
    const double center = (proportion + z * z / (2.0 * count)) / denominator;
    const double margin = z * std::sqrt(proportion * (1.0 - proportion) / count +
                                        z * z / (4.0 * count * count)) / denominator;
    return {std::max(0.0, center - margin), std::min(1.0, center + margin)};
}

} // namespace

bool LoadTimelineJsonl(const std::filesystem::path& path,
                       std::vector<LabeledSoundEvent>& events,
                       std::string* error) {
    events.clear();
    std::ifstream in(path);
    if (!in) {
        if (error) *error = "Could not open timeline: " + path.string();
        return false;
    }
    std::string line;
    size_t lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        if (line.empty()) continue;
        const auto soundClass = SoundClassFromString(Extract(line, "class"));
        if (!soundClass) {
            if (error) *error = "Unknown sound class at timeline line " + std::to_string(lineNumber);
            return false;
        }
        LabeledSoundEvent event;
        event.soundClass = *soundClass;
        event.onsetSample = U64(Extract(line, "onset_sample"));
        event.endSample = U64(Extract(line, "end_sample"));
        event.stratum = Extract(line, "stratum");
        event.snrDb = Float(Extract(line, "snr_db"));
        event.overlap = Bool(Extract(line, "overlap"));
        event.seenSource = Bool(Extract(line, "seen_source"));
        if (event.endSample < event.onsetSample) {
            if (error) *error = "Invalid event range at timeline line " + std::to_string(lineNumber);
            return false;
        }
        events.push_back(std::move(event));
    }
    if (error) error->clear();
    return true;
}

EvaluationReport EvaluateEvents(const std::vector<LabeledSoundEvent>& truth,
                                const std::vector<SoundEvent>& predictions,
                                double durationSeconds,
                                uint64_t onsetToleranceSamples) {
    EvaluationReport report;
    report.durationSeconds = durationSeconds;
    std::vector<bool> matchedTruth(truth.size(), false);
    std::vector<bool> matchedPredictions(predictions.size(), false);
    std::array<double, kSoundClassCount> latencySumMs{};
    std::array<double, kSoundClassCount> detectionLatencySumMs{};
    std::array<std::vector<double>, kSoundClassCount> absoluteOnsetErrorsMs;

    for (size_t predictionIndex = 0; predictionIndex < predictions.size(); ++predictionIndex) {
        const SoundEvent& prediction = predictions[predictionIndex];
        size_t best = truth.size();
        uint64_t bestDistance = onsetToleranceSamples + 1;
        for (size_t truthIndex = 0; truthIndex < truth.size(); ++truthIndex) {
            if (matchedTruth[truthIndex] || truth[truthIndex].soundClass != prediction.soundClass) continue;
            const uint64_t distance = prediction.onsetSample > truth[truthIndex].onsetSample
                ? prediction.onsetSample - truth[truthIndex].onsetSample
                : truth[truthIndex].onsetSample - prediction.onsetSample;
            if (distance <= onsetToleranceSamples && distance < bestDistance) {
                best = truthIndex;
                bestDistance = distance;
            }
        }
        const size_t classIndex = SoundClassIndex(prediction.soundClass);
        if (best == truth.size()) {
            ++report.classes[classIndex].falsePositives;
        } else {
            matchedTruth[best] = true;
            matchedPredictions[predictionIndex] = true;
            ++report.classes[classIndex].truePositives;
            const int64_t latencySamples = static_cast<int64_t>(prediction.onsetSample) -
                static_cast<int64_t>(truth[best].onsetSample);
            latencySumMs[classIndex] += static_cast<double>(latencySamples) * 1000.0 / 48000.0;
            absoluteOnsetErrorsMs[classIndex].push_back(
                std::abs(static_cast<double>(latencySamples) * 1000.0 / 48000.0));
            const uint64_t detected = prediction.detectedSample == 0
                ? prediction.onsetSample : prediction.detectedSample;
            const int64_t detectionLatencySamples = static_cast<int64_t>(detected) -
                static_cast<int64_t>(truth[best].onsetSample);
            detectionLatencySumMs[classIndex] +=
                static_cast<double>(detectionLatencySamples) * 1000.0 / 48000.0;
        }
    }
    for (size_t truthIndex = 0; truthIndex < truth.size(); ++truthIndex) {
        if (!matchedTruth[truthIndex]) ++report.classes[SoundClassIndex(truth[truthIndex].soundClass)].falseNegatives;
    }
    for (size_t i = 0; i < kSoundClassCount; ++i) {
        ClassMetrics& metrics = report.classes[i];
        const double precisionDenominator = static_cast<double>(metrics.truePositives + metrics.falsePositives);
        const double recallDenominator = static_cast<double>(metrics.truePositives + metrics.falseNegatives);
        metrics.precision = precisionDenominator == 0.0 ? 0.0 : metrics.truePositives / precisionDenominator;
        metrics.recall = recallDenominator == 0.0 ? 0.0 : metrics.truePositives / recallDenominator;
        metrics.f1 = metrics.precision + metrics.recall == 0.0 ? 0.0 :
            2.0 * metrics.precision * metrics.recall / (metrics.precision + metrics.recall);
        metrics.falseAlertsPerMinute = durationSeconds <= 0.0 ? 0.0 :
            metrics.falsePositives * 60.0 / durationSeconds;
        metrics.meanLatencyMs = metrics.truePositives == 0 ? 0.0 :
            latencySumMs[i] / static_cast<double>(metrics.truePositives);
        metrics.meanDetectionLatencyMs = metrics.truePositives == 0 ? 0.0 :
            detectionLatencySumMs[i] / static_cast<double>(metrics.truePositives);
        auto& errors = absoluteOnsetErrorsMs[i];
        std::sort(errors.begin(), errors.end());
        if (!errors.empty()) {
            metrics.medianAbsoluteOnsetErrorMs = errors[errors.size() / 2];
            const size_t p95 = std::min(errors.size() - 1,
                static_cast<size_t>(std::ceil(errors.size() * 0.95)) - 1);
            metrics.p95AbsoluteOnsetErrorMs = errors[p95];
        }
        metrics.support = metrics.truePositives + metrics.falseNegatives;
        const auto interval = WilsonInterval(metrics.truePositives, metrics.support);
        metrics.recallCi95Low = interval.first;
        metrics.recallCi95High = interval.second;
        metrics.conclusive = metrics.support >= 30;
    }
    return report;
}

bool WriteEvaluationJson(const std::filesystem::path& path,
                         const EvaluationReport& report,
                         std::string* error) {
    std::ofstream out(path);
    if (!out) {
        if (error) *error = "Could not create report: " + path.string();
        return false;
    }
    out << std::fixed << std::setprecision(6);
    out << "{\n  \"duration_seconds\": " << report.durationSeconds << ",\n  \"classes\": {\n";
    for (size_t i = 0; i < kSoundClassCount; ++i) {
        const ClassMetrics& metrics = report.classes[i];
        out << "    \"" << ToString(kSoundClasses[i]) << "\": {"
            << "\"tp\": " << metrics.truePositives
            << ", \"fp\": " << metrics.falsePositives
            << ", \"fn\": " << metrics.falseNegatives
            << ", \"precision\": " << metrics.precision
            << ", \"recall\": " << metrics.recall
            << ", \"f1\": " << metrics.f1
            << ", \"false_alerts_per_minute\": " << metrics.falseAlertsPerMinute
            << ", \"mean_latency_ms\": " << metrics.meanLatencyMs
            << ", \"mean_detection_latency_ms\": " << metrics.meanDetectionLatencyMs
            << ", \"median_absolute_onset_error_ms\": " << metrics.medianAbsoluteOnsetErrorMs
            << ", \"p95_absolute_onset_error_ms\": " << metrics.p95AbsoluteOnsetErrorMs
            << ", \"support\": " << metrics.support
            << ", \"recall_ci95_low\": " << metrics.recallCi95Low
            << ", \"recall_ci95_high\": " << metrics.recallCi95High
            << ", \"conclusive\": " << (metrics.conclusive ? "true" : "false") << "}"
            << (i + 1 == kSoundClassCount ? "\n" : ",\n");
    }
    out << "  }\n}\n";
    if (!out.good()) {
        if (error) *error = "Failed while writing evaluation report";
        return false;
    }
    if (error) error->clear();
    return true;
}

} // namespace EchoRadar
