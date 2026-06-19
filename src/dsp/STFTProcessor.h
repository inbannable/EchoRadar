#pragma once
#include "../common/Types.h"
#include <cstdint>
#include <vector>

namespace EchoRadar {

/// Short-Time Fourier Transform processor.
/// Converts interleaved PCM samples into a power spectrogram.
///
/// Default parameters:
///   FFT size  = 1024
///   Hop size  = 512
///   Window    = Hann
class STFTProcessor {
public:
    struct Config {
        uint32_t fft_size{1024};
        uint32_t hop_size{512};
        uint32_t sample_rate{48000};
        bool     apply_hann_window{true};
    };

    explicit STFTProcessor(Config cfg = {});
    ~STFTProcessor();

    STFTProcessor(const STFTProcessor&)            = delete;
    STFTProcessor& operator=(const STFTProcessor&) = delete;

    /// Process a mono channel; returns accumulated spectrogram frames.
    Spectrogram Process(const std::vector<float>& samples);

    /// Process left/right channels separately; returns two spectrograms.
    std::pair<Spectrogram, Spectrogram> ProcessStereo(const AudioFrame& frame);

    const Config& GetConfig() const { return m_cfg; }

    /// Bin index → frequency in Hz.
    float BinToHz(uint32_t bin) const;

private:
    Config               m_cfg;
    std::vector<float>   m_window;      // pre-computed Hann window
    std::vector<float>   m_overlap_buf; // cross-frame overlap samples
    // KissFFT plan stored as void* to avoid pulling kiss_fft headers here
    void*                m_fft_cfg{nullptr};

    void BuildWindow();
    void BuildFFTPlan();
    void DestroyFFTPlan();
};

} // namespace EchoRadar
