#pragma once

#include <dsp/STFTProcessor.h>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace EchoRadar {

struct LogMelFrame {
    uint64_t startSample{0};
    std::vector<float> values;
};

class LogMelExtractor {
public:
    enum class Mode {
        LogMelV1,
        StereoPcenV2,
    };

    struct Config {
        uint32_t sampleRate{48000};
        uint32_t fftSize{1024};
        uint32_t hopSize{512};
        uint32_t melBins{64};
        float minHz{50.0f};
        float maxHz{18000.0f};
        float logScale{10000.0f};
        Mode mode{Mode::LogMelV1};
        float pcenSmoothing{0.025f};
        float pcenAlpha{0.98f};
        float pcenDelta{2.0f};
        float pcenRoot{0.5f};
        float pcenEpsilon{1e-6f};
    };

    explicit LogMelExtractor(const Config& config = {});

    void Reset();
    void PushInterleaved(const float* stereoSamples, size_t frameCount);
    bool PopFrame(LogMelFrame& frame);
    size_t GetAvailableFrames() const { return m_ready.size(); }
    const Config& GetConfig() const { return m_config; }
    size_t GetOutputChannels() const { return m_config.mode == Mode::StereoPcenV2 ? 2u : 1u; }

private:
    Config m_config;
    STFTProcessor m_stft;
    std::vector<float> m_filters;
    std::vector<float> m_monoStereoScratch;
    std::vector<float> m_pcenSmooth;
    bool m_pcenInitialized{false};
    std::deque<LogMelFrame> m_ready;

    void BuildFilters();
};

} // namespace EchoRadar
