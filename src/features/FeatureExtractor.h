#pragma once
#include "../common/Types.h"

namespace EchoRadar {

/// Extracts binaural and spectral features from a stereo spectrogram pair.
/// Produces a FeatureVector suitable for direction estimation.
class FeatureExtractor {
public:
    struct Config {
        uint32_t n_energy_bands{8};     // sub-band count for energy distribution
        uint32_t sample_rate{48000};
        uint32_t fft_size{1024};
    };

    explicit FeatureExtractor(Config cfg = {});
    ~FeatureExtractor() = default;

    /// Compute features from a stereo frame pair of spectrograms.
    /// @param left   Spectrogram of the left channel.
    /// @param right  Spectrogram of the right channel.
    /// @param ts_ms  Frame timestamp in milliseconds.
    FeatureVector Extract(const Spectrogram& left,
                          const Spectrogram& right,
                          uint64_t           ts_ms = 0) const;

private:
    Config m_cfg;

    float ComputeILD(const Spectrogram& left, const Spectrogram& right,
                     std::size_t frame_idx) const;
    float ComputeITD(const Spectrogram& left, const Spectrogram& right,
                     std::size_t frame_idx) const;
    float ComputeSpectralCentroid(const Spectrogram& spec,
                                  std::size_t frame_idx) const;
    float ComputeSpectralRolloff(const Spectrogram& spec,
                                 std::size_t frame_idx,
                                 float rolloff_pct = 0.85f) const;
    std::vector<float> ComputeEnergyBands(const Spectrogram& spec,
                                          std::size_t frame_idx) const;
};

} // namespace EchoRadar
