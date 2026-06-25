#pragma once
#include "../common/Types.h"
#include <kiss_fftr.h>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace EchoRadar {

/// Positive-frequency channel spectrum (N/2 + 1 bins for real FFT input).
struct STFTChannelFrame {
    std::vector<std::complex<float>> spectrum;   ///< Complex FFT bins [0, Nyquist]
    std::vector<float>               magnitudes; ///< |X[k]|
    std::vector<float>               power;      ///< |X[k]|^2
};

/// One stereo STFT output frame.
struct STFTFrame {
    uint64_t         frame_index{0};   ///< Monotonic STFT frame index
    uint64_t         start_sample{0};  ///< Absolute PCM sample index of window start
    uint32_t         fft_size{1024};
    uint32_t         hop_size{512};
    uint32_t         sample_rate{48000};
    STFTChannelFrame left;
    STFTChannelFrame right;
};

/// Streaming stereo STFT processor using Hann window + KissFFT.
class STFTProcessor {
public:
    struct Config {
        uint32_t fft_size{1024};
        uint32_t hop_size{512};
        uint32_t sample_rate{48000};
    };

    explicit STFTProcessor(const Config& cfg = {});
    ~STFTProcessor();

    STFTProcessor(const STFTProcessor&)            = delete;
    STFTProcessor& operator=(const STFTProcessor&) = delete;

    void Reset();

    /// Push streaming stereo PCM frames ([L,R,L,R,...]).
    void PushInterleaved(const float* samples, size_t frameCount);

    size_t GetAvailableSTFTFrames() const;
    bool   PopFrame(STFTFrame& outFrame);

    const Config& GetConfig() const { return m_cfg; }
    uint32_t      GetBinCount() const { return m_cfg.fft_size / 2 + 1; }
    float         BinToHz(uint32_t bin) const;

    // Legacy compatibility helpers used by existing app/tests.
    Spectrogram Process(const std::vector<float>& samples);
    std::pair<Spectrogram, Spectrogram> ProcessStereo(const AudioFrame& frame);

private:
    Config m_cfg;

    std::vector<float> m_window;
    std::vector<float> m_left_samples;
    std::vector<float> m_right_samples;
    size_t             m_next_window_start{0};
    uint64_t           m_stream_start_sample{0};
    uint64_t           m_next_frame_index{0};

    kiss_fftr_cfg             m_fft_cfg{nullptr};
    std::vector<float>        m_fft_input;
    std::vector<kiss_fft_cpx> m_fft_output;

    std::deque<STFTFrame> m_ready_frames;

    void ValidateConfig() const;
    void BuildFFTPlan();
    void DestroyFFTPlan();
    void ProduceFrames();
    void MaybeCompactBuffers();

    void ComputeChannelFrame(const std::vector<float>& source, size_t start, STFTChannelFrame& out);
};

} // namespace EchoRadar
