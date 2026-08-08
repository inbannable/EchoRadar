#pragma once

#include "EventPostProcessor.h"
#include "LogMelExtractor.h"
#include "ProbabilityModel.h"

#include <chrono>
#include <array>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace EchoRadar {

class SoundRecognizer {
public:
    struct Config {
        uint32_t contextFrames{96};
        uint32_t inferenceStrideFrames{5};
        LogMelExtractor::Config featureConfig;
        EventPostProcessor::Config postProcessing;
    };

    struct RuntimeStats {
        uint64_t processedPcmFrames{0};
        uint64_t producedLogMelFrames{0};
        uint64_t inferenceCount{0};
        double lastInferenceMs{0.0};
        double p95InferenceMs{0.0};
        double maxInferenceMs{0.0};
    };

    explicit SoundRecognizer(std::shared_ptr<ProbabilityModel> model);
    SoundRecognizer(std::shared_ptr<ProbabilityModel> model, Config config);

    std::vector<SoundEvent> PushInterleaved(const float* stereoSamples, size_t frameCount);
    std::vector<SoundEvent> Flush();
    void Reset();

    const SoundProbabilities& LastProbabilities() const { return m_lastProbabilities; }
    SoundRecognitionState CurrentState() const;
    const RuntimeStats& Stats() const { return m_stats; }
    const std::string& LastError() const { return m_lastError; }

private:
    std::shared_ptr<ProbabilityModel> m_model;
    Config m_config;
    LogMelExtractor m_preprocessor;
    EventPostProcessor m_postProcessor;
    std::deque<LogMelFrame> m_context;
    std::vector<float> m_input;
    uint32_t m_framesSinceInference{0};
    std::array<double, 256> m_inferenceTimes{};
    size_t m_inferenceTimeCount{0};
    size_t m_inferenceTimeIndex{0};
    uint64_t m_lastSample{0};
    SoundProbabilities m_lastProbabilities;
    RuntimeStats m_stats;
    std::string m_lastError;
};

} // namespace EchoRadar
