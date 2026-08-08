#include "SoundRecognizer.h"

#include <algorithm>
#include <stdexcept>

namespace EchoRadar {

SoundRecognizer::SoundRecognizer(std::shared_ptr<ProbabilityModel> model, Config config)
    : m_model(std::move(model)),
      m_config(std::move(config)),
      m_preprocessor(m_config.featureConfig),
      m_postProcessor(m_config.postProcessing) {
    if (!m_model) throw std::invalid_argument("SoundRecognizer requires a probability model");
    if (m_config.contextFrames == 0 || m_config.inferenceStrideFrames == 0 ||
        m_model->InputFrames() != m_config.contextFrames ||
        m_model->InputBins() != m_config.featureConfig.melBins ||
        m_model->InputChannels() != m_preprocessor.GetOutputChannels()) {
        throw std::invalid_argument("Probability model shape is incompatible with SoundRecognizer");
    }
    m_input.resize(m_model->InputChannels() * static_cast<size_t>(m_config.contextFrames) *
                   m_model->InputBins());
    m_framesSinceInference = m_config.inferenceStrideFrames - 1;
}

std::vector<SoundEvent> SoundRecognizer::PushInterleaved(const float* stereoSamples, size_t frameCount) {
    std::vector<SoundEvent> events;
    m_preprocessor.PushInterleaved(stereoSamples, frameCount);
    m_stats.processedPcmFrames += frameCount;

    LogMelFrame frame;
    while (m_preprocessor.PopFrame(frame)) {
        ++m_stats.producedLogMelFrames;
        m_lastSample = frame.startSample + m_preprocessor.GetConfig().fftSize;
        m_context.push_back(std::move(frame));
        while (m_context.size() > m_config.contextFrames) m_context.pop_front();
        if (m_context.size() < m_config.contextFrames) continue;
        if (++m_framesSinceInference < m_config.inferenceStrideFrames) continue;
        m_framesSinceInference = 0;

        size_t offset = 0;
        for (size_t channel = 0; channel < m_model->InputChannels(); ++channel) {
            for (const LogMelFrame& contextFrame : m_context) {
                const auto begin = contextFrame.values.begin() +
                    static_cast<std::ptrdiff_t>(channel * m_model->InputBins());
                std::copy(begin, begin + static_cast<std::ptrdiff_t>(m_model->InputBins()),
                          m_input.begin() + static_cast<std::ptrdiff_t>(offset));
                offset += m_model->InputBins();
            }
        }

        std::array<float, kSoundClassCount> values{};
        const auto start = std::chrono::steady_clock::now();
        const bool ok = m_model->Predict(m_input, values, &m_lastError);
        const auto finish = std::chrono::steady_clock::now();
        const double elapsedMs = std::chrono::duration<double, std::milli>(finish - start).count();
        ++m_stats.inferenceCount;
        m_stats.lastInferenceMs = elapsedMs;
        m_stats.maxInferenceMs = std::max(m_stats.maxInferenceMs, elapsedMs);
        m_inferenceTimes[m_inferenceTimeIndex] = elapsedMs;
        m_inferenceTimeIndex = (m_inferenceTimeIndex + 1) % m_inferenceTimes.size();
        m_inferenceTimeCount = std::min(m_inferenceTimeCount + 1, m_inferenceTimes.size());
        std::array<double, 256> ordered = m_inferenceTimes;
        std::sort(ordered.begin(), ordered.begin() + static_cast<std::ptrdiff_t>(m_inferenceTimeCount));
        const size_t percentileIndex = m_inferenceTimeCount == 0 ? 0 :
            std::min(m_inferenceTimeCount - 1, static_cast<size_t>(m_inferenceTimeCount * 0.95));
        m_stats.p95InferenceMs = ordered[percentileIndex];
        if (!ok) continue;

        m_lastProbabilities.sample = m_lastSample;
        m_lastProbabilities.values = values;
        auto emitted = m_postProcessor.Process(m_lastProbabilities);
        events.insert(events.end(), std::make_move_iterator(emitted.begin()),
                      std::make_move_iterator(emitted.end()));
    }
    return events;
}

std::vector<SoundEvent> SoundRecognizer::Flush() {
    return m_postProcessor.Flush(m_lastSample);
}

void SoundRecognizer::Reset() {
    m_preprocessor.Reset();
    m_postProcessor.Reset();
    m_context.clear();
    m_framesSinceInference = m_config.inferenceStrideFrames - 1;
    m_inferenceTimes = {};
    m_inferenceTimeCount = 0;
    m_inferenceTimeIndex = 0;
    m_lastSample = 0;
    m_lastProbabilities = {};
    m_stats = {};
    m_lastError.clear();
}

SoundRecognitionState SoundRecognizer::CurrentState() const {
    SoundRecognitionState state;
    state.sample = m_lastProbabilities.sample;
    state.activeClasses = m_postProcessor.ActiveClasses();
    state.mode = m_postProcessor.IsAmbient()
        ? RecognitionMode::Ambient
        : RecognitionMode::TargetActive;
    return state;
}

} // namespace EchoRadar
