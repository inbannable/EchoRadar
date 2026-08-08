#pragma once

#include <dsp/STFTProcessor.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace EchoRadar {

struct StereoOnsetFeatureFrame {
    static constexpr size_t kChannels = 5;
    static constexpr size_t kMelBins = 64;
    uint64_t startSample{0};
    uint64_t endSample{0};
    std::array<float, kChannels * kMelBins> values{};
    float sceneActivity{0.0f};
};

/// Streaming implementation of Python's stereo_onset_features() contract.
class StereoOnsetFeatureExtractor {
public:
    struct Config {
        uint32_t sampleRate{48000};
        uint32_t fftSize{1024};
        uint32_t hopSize{240};
        uint32_t melBins{64};
        float minHz{50.0f};
        float maxHz{18000.0f};
        float pcenSmoothing{0.025f};
        float pcenAlpha{0.98f};
        float pcenDelta{2.0f};
        float pcenRoot{0.5f};
        float pcenEpsilon{1e-6f};
        uint32_t activitySmoothingFrames{100};
    };

    StereoOnsetFeatureExtractor();
    explicit StereoOnsetFeatureExtractor(const Config& config);

    void Reset();
    void PushInterleaved(const float* stereoSamples, size_t frameCount);
    bool PopFrame(StereoOnsetFeatureFrame& frame);
    size_t GetAvailableFrames() const { return m_ready.size(); }
    const Config& GetConfig() const { return m_config; }

private:
    Config m_config;
    STFTProcessor m_stft;
    std::vector<float> m_filters;
    std::array<float, StereoOnsetFeatureFrame::kMelBins> m_pcenSmooth{};
    bool m_pcenInitialized{false};
    bool m_activityInitialized{false};
    float m_activityState{0.0f};
    std::deque<StereoOnsetFeatureFrame> m_ready;

    void BuildFilters();
};

} // namespace EchoRadar
