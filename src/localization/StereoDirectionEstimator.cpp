#include "StereoDirectionEstimator.h"

#include <dsp/STFTProcessor.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace EchoRadar {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr std::array<float, StereoDirectionFeatures::kBandCount + 1> kBandEdges{
    120.0f, 300.0f, 600.0f, 1200.0f, 2400.0f,
    4800.0f, 8000.0f, 12000.0f, 20000.0f,
};

size_t BandForFrequency(float frequency) {
    for (size_t index = 0; index < StereoDirectionFeatures::kBandCount; ++index) {
        if (frequency >= kBandEdges[index] && frequency < kBandEdges[index + 1]) {
            return index;
        }
    }
    return StereoDirectionFeatures::kBandCount;
}

float SafeDbRatio(double numerator, double denominator) {
    constexpr double epsilon = 1.0e-12;
    return static_cast<float>(10.0 * std::log10((numerator + epsilon) / (denominator + epsilon)));
}

bool FiniteFeatures(const StereoDirectionFeatures& features) {
    if (!std::isfinite(features.broadbandIldDb) || !std::isfinite(features.itdSamples) ||
        !std::isfinite(features.correlationPeak) || !std::isfinite(features.stereoQuality) ||
        !std::isfinite(features.rms)) return false;
    return std::all_of(features.bandIldDb.begin(), features.bandIldDb.end(),
                       [](float value) { return std::isfinite(value); }) &&
           std::all_of(features.bandCoherence.begin(), features.bandCoherence.end(),
                       [](float value) { return std::isfinite(value); });
}

std::vector<std::string> Split(const std::string& line, char separator) {
    std::vector<std::string> values;
    std::istringstream input(line);
    std::string value;
    while (std::getline(input, value, separator)) values.push_back(value);
    return values;
}

const char* ClassName(SoundClass soundClass) {
    return soundClass == SoundClass::Gunshot ? "gunshot" : "footstep";
}

SoundClass ParseClass(const std::string& text) {
    return text == "gunshot" ? SoundClass::Gunshot : SoundClass::Footstep;
}

} // namespace

void DirectionCalibrationProfile::Clear() {
    m_audioProfileKey.clear();
    m_samples.clear();
}

void DirectionCalibrationProfile::SetAudioProfileKey(std::string key) {
    m_audioProfileKey = std::move(key);
}

bool DirectionCalibrationProfile::Matches(const AudioProfile& profile) const {
    return !m_samples.empty() && m_audioProfileKey == profile.StableKey();
}

void DirectionCalibrationProfile::AddSample(const DirectionCalibrationSample& sample) {
    if (!FiniteFeatures(sample.features) || !std::isfinite(sample.angleDegrees)) return;
    DirectionCalibrationSample safe = sample;
    safe.angleDegrees = WrapDirectionDegrees(safe.angleDegrees);
    m_samples.push_back(std::move(safe));
}

size_t DirectionCalibrationProfile::SampleCount(SoundClass soundClass) const {
    return static_cast<size_t>(std::count_if(
        m_samples.begin(), m_samples.end(),
        [soundClass](const auto& sample) { return sample.soundClass == soundClass; }));
}

bool DirectionCalibrationProfile::Save(const std::filesystem::path& path,
                                       std::string* error) const {
    std::error_code filesystemError;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), filesystemError);
        if (filesystemError) {
            if (error) *error = "Could not create calibration directory";
            return false;
        }
    }
    const std::filesystem::path temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        if (error) *error = "Could not open calibration file for writing";
        return false;
    }
    output << "echoradar-direction-calibration-v1\n";
    output << "profile\t" << std::quoted(m_audioProfileKey) << '\n';
    output << std::setprecision(9);
    for (const auto& sample : m_samples) {
        output << "sample\t" << ClassName(sample.soundClass) << '\t' << sample.angleDegrees
               << '\t' << sample.features.broadbandIldDb
               << '\t' << sample.features.itdSamples
               << '\t' << sample.features.correlationPeak
               << '\t' << sample.features.stereoQuality
               << '\t' << sample.features.rms;
        for (float value : sample.features.bandIldDb) output << '\t' << value;
        for (float value : sample.features.bandCoherence) output << '\t' << value;
        output << '\n';
    }
    output.flush();
    if (!output) {
        if (error) *error = "Could not finish writing calibration file";
        return false;
    }
    output.close();

#ifdef _WIN32
    if (!MoveFileExW(temporary.wstring().c_str(), path.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (error) *error = "Could not atomically publish calibration file";
        return false;
    }
#else
    std::filesystem::rename(temporary, path, filesystemError);
    if (filesystemError) {
        if (error) *error = "Could not atomically publish calibration file";
        return false;
    }
#endif
    if (error) error->clear();
    return true;
}

bool DirectionCalibrationProfile::Load(const std::filesystem::path& path,
                                       std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error) *error = "Calibration profile is unavailable";
        return false;
    }
    std::string line;
    if (!std::getline(input, line) || line != "echoradar-direction-calibration-v1") {
        if (error) *error = "Calibration profile version is incompatible";
        return false;
    }

    DirectionCalibrationProfile loaded;
    while (std::getline(input, line)) {
        if (line.rfind("profile\t", 0) == 0) {
            std::istringstream profileLine(line.substr(8));
            profileLine >> std::quoted(loaded.m_audioProfileKey);
            continue;
        }
        if (line.rfind("sample\t", 0) != 0) continue;
        const auto fields = Split(line, '\t');
        constexpr size_t expected = 8 + StereoDirectionFeatures::kBandCount * 2;
        if (fields.size() != expected) continue;
        try {
            DirectionCalibrationSample sample;
            sample.soundClass = ParseClass(fields[1]);
            sample.angleDegrees = std::stof(fields[2]);
            sample.features.broadbandIldDb = std::stof(fields[3]);
            sample.features.itdSamples = std::stof(fields[4]);
            sample.features.correlationPeak = std::stof(fields[5]);
            sample.features.stereoQuality = std::stof(fields[6]);
            sample.features.rms = std::stof(fields[7]);
            size_t cursor = 8;
            for (float& value : sample.features.bandIldDb) value = std::stof(fields[cursor++]);
            for (float& value : sample.features.bandCoherence) value = std::stof(fields[cursor++]);
            loaded.AddSample(sample);
        } catch (...) {
        }
    }
    if (loaded.m_audioProfileKey.empty()) {
        if (error) *error = "Calibration profile has no audio profile key";
        return false;
    }
    *this = std::move(loaded);
    if (error) error->clear();
    return true;
}

StereoDirectionEstimator::StereoDirectionEstimator() : StereoDirectionEstimator(Config{}) {}

StereoDirectionEstimator::StereoDirectionEstimator(Config config) : m_config(config) {
    if (m_config.sampleRate == 0 || m_config.fftSize == 0 || m_config.hopSize == 0 ||
        m_config.maximumLagSamples == 0) {
        throw std::invalid_argument("Invalid stereo direction estimator configuration");
    }
}

bool StereoDirectionEstimator::ExtractFeatures(
    std::span<const float> interleaved,
    StereoDirectionFeatures& output,
    std::string* error) const {
    output = {};
    if (interleaved.size() < static_cast<size_t>(m_config.fftSize) * 2 ||
        interleaved.size() % 2 != 0) {
        if (error) *error = "Direction input must contain at least one stereo FFT window";
        return false;
    }
    const size_t frameCount = interleaved.size() / 2;
    float peakEnvelope = 0.0f;
    for (size_t frame = 0; frame < frameCount; ++frame) {
        peakEnvelope = std::max(peakEnvelope, std::max(
            std::abs(interleaved[frame * 2]),
            std::abs(interleaved[frame * 2 + 1])));
    }
    double leftEnergy = 0.0;
    double rightEnergy = 0.0;
    for (size_t frame = 0; frame < frameCount; ++frame) {
        const double left = interleaved[frame * 2];
        const double right = interleaved[frame * 2 + 1];
        const float envelope = std::max(std::abs(static_cast<float>(left)),
                                        std::abs(static_cast<float>(right)));
        const float activity = peakEnvelope > 1.0e-9f
            ? std::clamp(envelope / (peakEnvelope * 0.5f), 0.0f, 1.0f) : 0.0f;
        // Direction cues should be dominated by the recognized transient, not
        // by quiet ambience in the rest of the localization window.
        const double weight = 0.08 + 0.92 * activity * activity;
        leftEnergy += weight * left * left;
        rightEnergy += weight * right * right;
    }
    output.rms = static_cast<float>(std::sqrt((leftEnergy + rightEnergy) /
                                              std::max<size_t>(1, frameCount * 2)));
    if (output.rms < 1.0e-6f) {
        if (error) *error = "Direction input is silent";
        return false;
    }
    output.broadbandIldDb = SafeDbRatio(leftEnergy, rightEnergy);

    float bestCorrelation = -std::numeric_limits<float>::infinity();
    int bestLag = 0;
    const int maximumLag = static_cast<int>(
        std::min<size_t>(m_config.maximumLagSamples, frameCount / 4));
    std::vector<float> lagCorrelations(static_cast<size_t>(maximumLag * 2 + 1), -1.0f);
    for (int lag = -maximumLag; lag <= maximumLag; ++lag) {
        double numerator = 0.0;
        double laggedLeftEnergy = 0.0;
        double laggedRightEnergy = 0.0;
        const size_t leftStart = lag < 0 ? static_cast<size_t>(-lag) : 0;
        const size_t rightStart = lag > 0 ? static_cast<size_t>(lag) : 0;
        const size_t count = frameCount - static_cast<size_t>(std::abs(lag));
        for (size_t frame = 0; frame < count; ++frame) {
            const double left = interleaved[(leftStart + frame) * 2];
            const double right = interleaved[(rightStart + frame) * 2 + 1];
            const float envelope = std::max(std::abs(static_cast<float>(left)),
                                            std::abs(static_cast<float>(right)));
            const float activity = peakEnvelope > 1.0e-9f
                ? std::clamp(envelope / (peakEnvelope * 0.5f), 0.0f, 1.0f) : 0.0f;
            const double weight = 0.08 + 0.92 * activity * activity;
            numerator += weight * left * right;
            laggedLeftEnergy += weight * left * left;
            laggedRightEnergy += weight * right * right;
        }
        const float correlation = static_cast<float>(
            numerator / std::sqrt(std::max(1.0e-18, laggedLeftEnergy * laggedRightEnergy)));
        lagCorrelations[static_cast<size_t>(lag + maximumLag)] = correlation;
        if (correlation > bestCorrelation) {
            bestCorrelation = correlation;
            bestLag = lag;
        }
    }
    float refinedLag = static_cast<float>(bestLag);
    if (bestLag > -maximumLag && bestLag < maximumLag) {
        const float previous = lagCorrelations[static_cast<size_t>(bestLag - 1 + maximumLag)];
        const float center = lagCorrelations[static_cast<size_t>(bestLag + maximumLag)];
        const float next = lagCorrelations[static_cast<size_t>(bestLag + 1 + maximumLag)];
        const float curvature = previous - 2.0f * center + next;
        if (std::abs(curvature) > 1.0e-6f) {
            refinedLag += std::clamp(0.5f * (previous - next) / curvature, -0.5f, 0.5f);
        }
    }
    output.itdSamples = -refinedLag;
    output.correlationPeak = std::clamp(bestCorrelation, -1.0f, 1.0f);

    STFTProcessor processor({m_config.fftSize, m_config.hopSize, m_config.sampleRate});
    processor.PushInterleaved(interleaved.data(), frameCount);
    std::array<double, StereoDirectionFeatures::kBandCount> bandLeft{};
    std::array<double, StereoDirectionFeatures::kBandCount> bandRight{};
    std::array<std::complex<double>, StereoDirectionFeatures::kBandCount> bandCross{};
    STFTFrame stft;
    size_t spectralFrames = 0;
    while (processor.PopFrame(stft)) {
        ++spectralFrames;
        for (size_t bin = 1; bin < stft.left.spectrum.size(); ++bin) {
            const size_t band = BandForFrequency(processor.BinToHz(static_cast<uint32_t>(bin)));
            if (band >= StereoDirectionFeatures::kBandCount) continue;
            bandLeft[band] += stft.left.power[bin];
            bandRight[band] += stft.right.power[bin];
            bandCross[band] += static_cast<std::complex<double>>(
                stft.left.spectrum[bin] * std::conj(stft.right.spectrum[bin]));
        }
    }
    if (spectralFrames == 0) {
        if (error) *error = "Direction input produced no spectral frames";
        return false;
    }

    float ildMean = 0.0f;
    for (size_t band = 0; band < StereoDirectionFeatures::kBandCount; ++band) {
        output.bandIldDb[band] = SafeDbRatio(bandLeft[band], bandRight[band]);
        output.bandCoherence[band] = static_cast<float>(
            std::abs(bandCross[band]) /
            std::sqrt(std::max(1.0e-18, bandLeft[band] * bandRight[band])));
        output.bandCoherence[band] = std::clamp(output.bandCoherence[band], 0.0f, 1.0f);
        ildMean += output.bandIldDb[band];
    }
    ildMean /= static_cast<float>(StereoDirectionFeatures::kBandCount);
    float ildSpread = 0.0f;
    float coherenceMean = 0.0f;
    for (size_t band = 0; band < StereoDirectionFeatures::kBandCount; ++band) {
        const float difference = output.bandIldDb[band] - ildMean;
        ildSpread += difference * difference;
        coherenceMean += output.bandCoherence[band];
    }
    ildSpread = std::sqrt(ildSpread / StereoDirectionFeatures::kBandCount);
    coherenceMean /= StereoDirectionFeatures::kBandCount;
    const float asymmetry = std::max({
        std::abs(output.broadbandIldDb) / 9.0f,
        std::abs(output.itdSamples) / 12.0f,
        ildSpread / 8.0f,
    });
    output.stereoQuality = std::clamp(
        (0.25f + 0.75f * asymmetry) * (0.35f + 0.65f * coherenceMean), 0.0f, 1.0f);
    if (error) error->clear();
    return true;
}

float StereoDirectionEstimator::FeatureDistance(const StereoDirectionFeatures& left,
                                                const StereoDirectionFeatures& right) {
    float distance = std::pow((left.broadbandIldDb - right.broadbandIldDb) / 12.0f, 2.0f);
    distance += std::pow((left.itdSamples - right.itdSamples) / 16.0f, 2.0f);
    for (size_t band = 0; band < StereoDirectionFeatures::kBandCount; ++band) {
        distance += 0.45f * std::pow((left.bandIldDb[band] - right.bandIldDb[band]) / 12.0f, 2.0f);
        distance += 0.15f * std::pow(left.bandCoherence[band] - right.bandCoherence[band], 2.0f);
    }
    return std::sqrt(distance / 5.8f);
}

void StereoDirectionEstimator::AddCircularKernel(std::array<float, 24>& probabilities,
                                                 float angleDegrees,
                                                 float sigmaDegrees,
                                                 float weight) {
    const float safeSigma = std::max(5.0f, sigmaDegrees);
    for (size_t index = 0; index < probabilities.size(); ++index) {
        const float center = static_cast<float>(index) * 15.0f;
        const float distance = CircularDistanceDegrees(center, angleDegrees);
        probabilities[index] += weight * std::exp(-0.5f * distance * distance /
                                                   (safeSigma * safeSigma));
    }
}

void StereoDirectionEstimator::Normalize(std::array<float, 24>& probabilities) {
    const float sum = std::accumulate(probabilities.begin(), probabilities.end(), 0.0f);
    if (sum <= 1.0e-12f) {
        probabilities.fill(1.0f / probabilities.size());
        return;
    }
    for (float& probability : probabilities) probability /= sum;
}

DirectionResult StereoDirectionEstimator::Estimate(
    uint64_t eventId,
    SoundClass soundClass,
    std::span<const float> interleaved,
    const AudioProfile& profile,
    const LocalizationTuning& tuning,
    const DirectionCalibrationProfile* calibration) const {
    const auto start = std::chrono::steady_clock::now();
    StereoDirectionFeatures features;
    std::string error;
    if (!ExtractFeatures(interleaved, features, &error)) {
        DirectionResult unavailable;
        unavailable.eventId = eventId;
        unavailable.status = DirectionStatus::AudioUnavailable;
        return unavailable;
    }
    DirectionResult result = EstimateFeatures(
        eventId, soundClass, features, profile, tuning, calibration);
    result.inferenceMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    return result;
}

DirectionResult StereoDirectionEstimator::EstimateFeatures(
    uint64_t eventId,
    SoundClass soundClass,
    const StereoDirectionFeatures& features,
    const AudioProfile& profile,
    const LocalizationTuning& tuning,
    const DirectionCalibrationProfile* calibration) const {
    DirectionResult result;
    result.eventId = eventId;

    const float isolation = std::clamp(profile.leftRightIsolationPercent / 100.0f, 0.0f, 1.0f);
    const float isolationCompensation = 1.0f + 0.65f * isolation;

    // Pool the useful mid/high-band ILD using coherence as reliability.  This
    // is substantially less sensitive to bass and unrelated broadband energy
    // than applying one fixed gain formula to the whole event.
    float bandIld = 0.0f;
    float bandWeight = 0.0f;
    for (size_t band = 1; band + 1 < StereoDirectionFeatures::kBandCount; ++band) {
        const float weight = 0.1f + features.bandCoherence[band] *
                                      features.bandCoherence[band];
        bandIld += weight * std::clamp(features.bandIldDb[band], -24.0f, 24.0f);
        bandWeight += weight;
    }
    if (bandWeight > 0.0f) bandIld /= bandWeight;
    const float effectiveIld = 0.42f * features.broadbandIldDb + 0.58f * bandIld;
    const float levelEvidence = std::tanh(
        -effectiveIld / (8.5f * isolationCompensation));
    const float timeEvidence = std::tanh(
        features.itdSamples /
        (0.72f * static_cast<float>(m_config.maximumLagSamples)));
    const float levelReliability = 0.25f + 0.75f * std::clamp(
        std::abs(effectiveIld) / 8.0f, 0.0f, 1.0f);
    const float timeReliability = std::clamp(
        (features.correlationPeak - 0.08f) / 0.82f, 0.0f, 1.0f);
    float levelWeight = 0.56f * levelReliability;
    float timeWeight = 0.44f * timeReliability;
    // Conflicting cues are common with reflections. Prefer the cue with the
    // clearer evidence instead of averaging them into a confidently wrong
    // center bearing.
    if (levelEvidence * timeEvidence < 0.0f) {
        if (levelWeight > timeWeight) timeWeight *= 0.35f;
        else levelWeight *= 0.35f;
    }
    const float side = std::clamp(
        (levelWeight * levelEvidence + timeWeight * timeEvidence) /
            std::max(0.001f, levelWeight + timeWeight),
        -1.0f, 1.0f);
    float frontAngle = std::asin(side) * 180.0f / kPi;
    frontAngle = WrapDirectionDegrees(frontAngle);
    const float signedFront = frontAngle > 180.0f ? frontAngle - 360.0f : frontAngle;
    const float rearAngle = WrapDirectionDegrees(180.0f - signedFront);
    const float syntheticFrontWeight = profile.perspectiveCorrection ? 0.62f : 0.55f;
    const float syntheticSigma = 20.0f + (1.0f - features.stereoQuality) * 42.0f;
    AddCircularKernel(result.probabilities, frontAngle, syntheticSigma, syntheticFrontWeight);
    AddCircularKernel(result.probabilities, rearAngle, syntheticSigma,
                      1.0f - syntheticFrontWeight);

    float calibrationWeight = 0.0f;
    if (calibration && calibration->Matches(profile)) {
        std::vector<std::pair<float, const DirectionCalibrationSample*>> nearest;
        for (const auto& sample : calibration->Samples()) {
            if (sample.soundClass != soundClass) continue;
            nearest.emplace_back(FeatureDistance(features, sample.features), &sample);
        }
        const size_t keep = std::min<size_t>(8, nearest.size());
        std::partial_sort(nearest.begin(), nearest.begin() + static_cast<std::ptrdiff_t>(keep),
                          nearest.end(), [](const auto& left, const auto& right) {
                              return left.first < right.first;
                          });
        for (size_t index = 0; index < keep; ++index) {
            const float weight = std::exp(-0.5f * nearest[index].first * nearest[index].first /
                                          (0.58f * 0.58f));
            AddCircularKernel(result.probabilities, nearest[index].second->angleDegrees,
                              12.0f + nearest[index].first * 15.0f, weight * 1.8f);
            calibrationWeight += weight;
        }
        if (calibrationWeight > 0.2f) result.profileSource = DirectionProfileSource::Calibrated;
    }
    Normalize(result.probabilities);

    const auto primaryIterator = std::max_element(
        result.probabilities.begin(), result.probabilities.end());
    const size_t primaryIndex = static_cast<size_t>(
        std::distance(result.probabilities.begin(), primaryIterator));
    const size_t previousIndex = (primaryIndex + result.probabilities.size() - 1) %
        result.probabilities.size();
    const size_t nextIndex = (primaryIndex + 1) % result.probabilities.size();
    const float previousProbability = result.probabilities[previousIndex];
    const float centerProbability = result.probabilities[primaryIndex];
    const float nextProbability = result.probabilities[nextIndex];
    const float peakCurvature = previousProbability - 2.0f * centerProbability +
                                nextProbability;
    float binOffset = 0.0f;
    if (std::abs(peakCurvature) > 1.0e-7f) {
        binOffset = std::clamp(
            0.5f * (previousProbability - nextProbability) / peakCurvature,
            -0.5f, 0.5f);
    }
    result.primaryAngleDegrees = WrapDirectionDegrees(
        (static_cast<float>(primaryIndex) + binOffset) * 15.0f);

    size_t secondaryIndex = primaryIndex;
    float secondaryProbability = 0.0f;
    for (size_t index = 0; index < result.probabilities.size(); ++index) {
        const float separation = CircularDistanceDegrees(
            static_cast<float>(index) * 15.0f, result.primaryAngleDegrees);
        if (separation < tuning.secondaryMinimumSeparationDegrees) continue;
        if (result.probabilities[index] > secondaryProbability) {
            secondaryProbability = result.probabilities[index];
            secondaryIndex = index;
        }
    }
    const float primaryProbability = *primaryIterator;
    if (primaryProbability > 0.0f &&
        secondaryProbability / primaryProbability >= tuning.secondaryRatio) {
        result.secondaryAngleDegrees = static_cast<float>(secondaryIndex) * 15.0f;
        result.secondaryConfidence = std::clamp(
            secondaryProbability / primaryProbability * features.stereoQuality, 0.0f, 1.0f);
    }

    double x = 0.0;
    double y = 0.0;
    for (size_t index = 0; index < result.probabilities.size(); ++index) {
        const double radians = static_cast<double>(index) * 15.0 * kPi / 180.0;
        x += result.probabilities[index] * std::cos(radians);
        y += result.probabilities[index] * std::sin(radians);
    }
    const float resultant = static_cast<float>(std::clamp(std::sqrt(x * x + y * y), 0.0, 1.0));
    result.uncertaintyDegrees = std::clamp(
        10.0f + (1.0f - resultant) * 80.0f, 10.0f, 90.0f);
    const float modelCertainty = std::clamp(primaryProbability * 6.0f, 0.0f, 1.0f);
    const float calibrationBoost = result.profileSource == DirectionProfileSource::Calibrated
        ? std::clamp(calibrationWeight / 3.0f, 0.0f, 1.0f) : 0.0f;
    result.confidence = std::clamp(
        features.stereoQuality * (0.55f * modelCertainty + 0.45f * calibrationBoost),
        0.0f, 1.0f);
    result.status = result.confidence >= tuning.minimumConfidence
        ? DirectionStatus::Estimated : DirectionStatus::LowConfidence;
    return result;
}

} // namespace EchoRadar
