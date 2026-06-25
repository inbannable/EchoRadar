#pragma once
#include "../common/Types.h"
#include "../dsp/STFTProcessor.h"

namespace EchoRadar {

/// Extracts compact, real-time features from stereo STFT frames.
class FeatureExtractor {
public:
    struct Config {
        float    low_band_hz{300.0f};
        float    mid_band_hz{2000.0f};
        float    epsilon{1e-9f};
        float    ema_alpha{0.2f};
        float    transient_energy_weight{0.45f};
        float    transient_spike_weight{0.35f};
        float    transient_high_weight{0.20f};
        uint32_t n_energy_bands{8}; // legacy compatibility path
    };

    explicit FeatureExtractor(const Config& cfg = {});
    ~FeatureExtractor() = default;

    /// Stateful extraction from one STFT frame.
    /// Energy features are based on power (|X[k]|^2).
    /// Centroid/flatness use magnitudes (|X[k]|).
    AudioFeatures Extract(const STFTFrame& frame);

    void Reset();

    /// Legacy API kept for downstream compatibility.
    FeatureVector Extract(const Spectrogram& left,
                          const Spectrogram& right,
                          uint64_t           ts_ms = 0) const;

private:
    Config m_cfg;
    uint32_t m_cached_fft_size{0};
    uint32_t m_cached_sample_rate{0};
    uint32_t m_low_band_end_bin{0};
    uint32_t m_mid_band_end_bin{0};

    float m_prev_total_energy{0.0f};
    float m_prev_log_energy{0.0f};
    float m_prev_high_band_energy{0.0f};
    float m_energy_ema{0.0f};
    float m_high_band_ema{0.0f};
    float m_log_energy_ema{0.0f};
    bool  m_has_temporal_state{false};
    std::vector<float> m_prev_magnitudes;

    void  UpdateBandCache(const STFTFrame& frame);
    float Clamp01(float value) const;

    // Legacy helpers for Spectrogram-based API.
    float ComputeLegacyCentroid(const std::vector<float>& mags, uint32_t sample_rate, uint32_t fft_size) const;
    std::vector<float> ComputeLegacyEnergyBands(const std::vector<float>& mags, uint32_t n_bands) const;
};

} // namespace EchoRadar
