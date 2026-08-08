#pragma once

#include "SoundRecognitionTypes.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace EchoRadar {

struct LabeledSoundEvent {
    SoundClass soundClass{SoundClass::Gunshot};
    uint64_t onsetSample{0};
    uint64_t endSample{0};
    std::string stratum;
    float snrDb{0.0f};
    bool overlap{false};
    bool seenSource{false};
};

struct ClassMetrics {
    uint64_t truePositives{0};
    uint64_t falsePositives{0};
    uint64_t falseNegatives{0};
    double precision{0.0};
    double recall{0.0};
    double f1{0.0};
    double falseAlertsPerMinute{0.0};
    double meanLatencyMs{0.0};
    double meanDetectionLatencyMs{0.0};
    double medianAbsoluteOnsetErrorMs{0.0};
    double p95AbsoluteOnsetErrorMs{0.0};
    uint64_t support{0};
    double recallCi95Low{0.0};
    double recallCi95High{1.0};
    bool conclusive{false};
};

struct EvaluationReport {
    std::array<ClassMetrics, kSoundClassCount> classes{};
    double durationSeconds{0.0};
};

bool LoadTimelineJsonl(const std::filesystem::path& path,
                       std::vector<LabeledSoundEvent>& events,
                       std::string* error = nullptr);
EvaluationReport EvaluateEvents(const std::vector<LabeledSoundEvent>& truth,
                                const std::vector<SoundEvent>& predictions,
                                double durationSeconds,
                                uint64_t onsetToleranceSamples = 7200);
bool WriteEvaluationJson(const std::filesystem::path& path,
                         const EvaluationReport& report,
                         std::string* error = nullptr);

} // namespace EchoRadar
