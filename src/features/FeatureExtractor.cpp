#include "FeatureExtractor.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace EchoRadar {

FeatureExtractor::FeatureExtractor(const Config& cfg) : m_cfg(cfg) {
    if (cfg.low_band_hz < 0.0f || cfg.mid_band_hz <= cfg.low_band_hz) {
        throw std::invalid_argument("Band boundaries must satisfy 0 <= low < mid");
    }
    if (cfg.epsilon <= 0.0f) {
        throw std::invalid_argument("epsilon must be > 0");
    }
    if (cfg.ema_alpha <= 0.0f || cfg.ema_alpha > 1.0f) {
        throw std::invalid_argument("ema_alpha must be in (0, 1]");
    }
    if (cfg.n_energy_bands == 0) {
        throw std::invalid_argument("n_energy_bands must be > 0");
    }

    const float weight_sum =
        cfg.transient_energy_weight + cfg.transient_spike_weight + cfg.transient_high_weight;
    if (weight_sum <= 0.0f) {
        throw std::invalid_argument("Transient weights must sum to > 0");
    }
}

void FeatureExtractor::Reset() {
    m_cached_fft_size = 0;
    m_cached_sample_rate = 0;
    m_low_band_end_bin = 0;
    m_mid_band_end_bin = 0;

    m_prev_total_energy = 0.0f;
    m_prev_log_energy = 0.0f;
    m_prev_high_band_energy = 0.0f;
    m_energy_ema = 0.0f;
    m_high_band_ema = 0.0f;
    m_log_energy_ema = 0.0f;
    m_has_temporal_state = false;
    m_prev_magnitudes.clear();
}

void FeatureExtractor::UpdateBandCache(const STFTFrame& frame) {
    if (frame.fft_size == m_cached_fft_size && frame.sample_rate == m_cached_sample_rate) {
        return;
    }

    m_cached_fft_size = frame.fft_size;
    m_cached_sample_rate = frame.sample_rate;

    if (frame.fft_size == 0 || frame.sample_rate == 0) {
        m_low_band_end_bin = 0;
        m_mid_band_end_bin = 0;
        return;
    }

    const float hz_per_bin = static_cast<float>(frame.sample_rate) /
                             static_cast<float>(frame.fft_size);
    const uint32_t max_bin = frame.fft_size / 2;

    m_low_band_end_bin = static_cast<uint32_t>(std::floor(m_cfg.low_band_hz / hz_per_bin));
    m_mid_band_end_bin = static_cast<uint32_t>(std::floor(m_cfg.mid_band_hz / hz_per_bin));
    m_low_band_end_bin = std::min(m_low_band_end_bin, max_bin);
    m_mid_band_end_bin = std::max(m_low_band_end_bin, std::min(m_mid_band_end_bin, max_bin));
}

float FeatureExtractor::Clamp01(float value) const {
    return std::clamp(value, 0.0f, 1.0f);
}

AudioFeatures FeatureExtractor::Extract(const STFTFrame& frame) {
    AudioFeatures out{};

    const size_t bin_count = std::min(
        std::min(frame.left.power.size(), frame.right.power.size()),
        std::min(frame.left.magnitudes.size(), frame.right.magnitudes.size()));
    if (bin_count == 0 || frame.fft_size == 0 || frame.sample_rate == 0) {
        return out;
    }

    UpdateBandCache(frame);
    if (m_prev_magnitudes.size() != bin_count) {
        m_prev_magnitudes.assign(bin_count, 0.0f);
        m_has_temporal_state = false;
        m_prev_total_energy = 0.0f;
        m_prev_log_energy = 0.0f;
        m_prev_high_band_energy = 0.0f;
        m_energy_ema = 0.0f;
        m_high_band_ema = 0.0f;
        m_log_energy_ema = 0.0f;
    }

    const uint32_t low_end_bin = std::min<uint32_t>(m_low_band_end_bin, static_cast<uint32_t>(bin_count - 1));
    const uint32_t mid_end_bin = std::min<uint32_t>(
        std::max(m_mid_band_end_bin, low_end_bin),
        static_cast<uint32_t>(bin_count - 1));

    const float hz_per_bin = static_cast<float>(frame.sample_rate) /
                             static_cast<float>(frame.fft_size);

    float left_energy = 0.0f;
    float right_energy = 0.0f;
    float mag_sum = 0.0f;
    float weighted_freq_sum = 0.0f;
    float log_mag_sum = 0.0f;
    float flux_sum = 0.0f;

    for (uint32_t bin = 0; bin < static_cast<uint32_t>(bin_count); ++bin) {
        const float l_power = frame.left.power[bin];
        const float r_power = frame.right.power[bin];
        const float combined_power = l_power + r_power;

        out.totalEnergy += combined_power;
        left_energy += l_power;
        right_energy += r_power;

        if (bin <= low_end_bin) {
            out.lowBandEnergy += combined_power;
        } else if (bin <= mid_end_bin) {
            out.midBandEnergy += combined_power;
        } else {
            out.highBandEnergy += combined_power;
        }

        const float combined_mag = frame.left.magnitudes[bin] + frame.right.magnitudes[bin];
        const float prev_mag = m_prev_magnitudes[bin];
        const float freq_hz = static_cast<float>(bin) * hz_per_bin;

        mag_sum += combined_mag;
        weighted_freq_sum += combined_mag * freq_hz;
        log_mag_sum += std::log(std::max(combined_mag, m_cfg.epsilon));

        const float flux_num = std::fabs(combined_mag - prev_mag);
        const float flux_den = combined_mag + prev_mag + m_cfg.epsilon;
        flux_sum += flux_num / flux_den;
    }

    out.logEnergy = std::log1p(out.totalEnergy);
    out.hfEnergyRatio = out.highBandEnergy / (out.totalEnergy + m_cfg.epsilon);

    if (mag_sum > m_cfg.epsilon) {
        out.spectralCentroid = weighted_freq_sum / mag_sum;
        const float arithmetic_mean = mag_sum / static_cast<float>(bin_count);
        const float geometric_mean = std::exp(log_mag_sum / static_cast<float>(bin_count));
        out.spectralFlatness = Clamp01(geometric_mean / (arithmetic_mean + m_cfg.epsilon));
    } else {
        out.spectralCentroid = 0.0f;
        out.spectralFlatness = 0.0f;
    }

    out.leftRightBalance = (left_energy - right_energy) /
                           (left_energy + right_energy + m_cfg.epsilon);

    if (!m_has_temporal_state) {
        out.energyDelta = 0.0f;
        out.energyRise = 0.0f;
        out.spectralFlux = 0.0f;
        out.transientScore = 0.0f;

        m_prev_total_energy = out.totalEnergy;
        m_prev_log_energy = out.logEnergy;
        m_prev_high_band_energy = out.highBandEnergy;
        m_energy_ema = out.totalEnergy;
        m_high_band_ema = out.highBandEnergy;
        m_log_energy_ema = out.logEnergy;
        m_prev_magnitudes.assign(bin_count, 0.0f);
        for (size_t i = 0; i < bin_count; ++i) {
            m_prev_magnitudes[i] = frame.left.magnitudes[i] + frame.right.magnitudes[i];
        }
        m_has_temporal_state = true;
        return out;
    }

    out.energyDelta = out.totalEnergy - m_prev_total_energy;
    out.energyRise = std::max(0.0f, out.logEnergy - m_prev_log_energy);
    out.spectralFlux = flux_sum / static_cast<float>(bin_count);

    const float energy_rise_norm = Clamp01(out.energyRise * 2.5f);
    const float flux_norm = Clamp01(out.spectralFlux * 1.5f);
    const float hf_norm = Clamp01(out.hfEnergyRatio);
    const float transient_raw =
        (m_cfg.transient_energy_weight * energy_rise_norm) +
        (m_cfg.transient_spike_weight * flux_norm) +
        (m_cfg.transient_high_weight * hf_norm) +
        (0.10f * Clamp01(out.logEnergy - m_log_energy_ema));
    out.transientScore = Clamp01(transient_raw);

    const float a = m_cfg.ema_alpha;
    m_energy_ema = a * out.totalEnergy + (1.0f - a) * m_energy_ema;
    m_high_band_ema = a * out.highBandEnergy + (1.0f - a) * m_high_band_ema;
    m_log_energy_ema = a * out.logEnergy + (1.0f - a) * m_log_energy_ema;
    m_prev_total_energy = out.totalEnergy;
    m_prev_log_energy = out.logEnergy;
    m_prev_high_band_energy = out.highBandEnergy;

    for (size_t i = 0; i < bin_count; ++i) {
        m_prev_magnitudes[i] = frame.left.magnitudes[i] + frame.right.magnitudes[i];
    }

    return out;
}

FeatureVector FeatureExtractor::Extract(const Spectrogram& left,
                                        const Spectrogram& right,
                                        uint64_t           ts_ms) const {
    FeatureVector fv;
    fv.timestamp = ts_ms;
    fv.energy_bands.assign(m_cfg.n_energy_bands, 0.0f);

    if (left.magnitude.empty() || right.magnitude.empty()) {
        return fv;
    }

    const auto& left_bins = left.magnitude.back();
    const auto& right_bins = right.magnitude.back();
    const size_t n_bins = std::min(left_bins.size(), right_bins.size());
    if (n_bins == 0) {
        return fv;
    }

    float left_power_sum = 0.0f;
    float right_power_sum = 0.0f;
    for (size_t i = 0; i < n_bins; ++i) {
        left_power_sum += left_bins[i] * left_bins[i];
        right_power_sum += right_bins[i] * right_bins[i];
    }

    fv.ild = 10.0f * std::log10((left_power_sum + m_cfg.epsilon) /
                                (right_power_sum + m_cfg.epsilon));
    fv.itd = 0.0f;
    fv.spectral_centroid = ComputeLegacyCentroid(left_bins, left.sample_rate, left.fft_size);
    fv.spectral_rolloff = 0.0f;
    fv.energy_bands = ComputeLegacyEnergyBands(left_bins, m_cfg.n_energy_bands);
    return fv;
}

float FeatureExtractor::ComputeLegacyCentroid(const std::vector<float>& mags,
                                              uint32_t sample_rate,
                                              uint32_t fft_size) const {
    if (mags.empty() || fft_size == 0) {
        return 0.0f;
    }

    const float hz_per_bin = static_cast<float>(sample_rate) /
                             static_cast<float>(fft_size);
    float mag_sum = 0.0f;
    float weighted_sum = 0.0f;
    for (size_t i = 0; i < mags.size(); ++i) {
        const float freq = static_cast<float>(i) * hz_per_bin;
        mag_sum += mags[i];
        weighted_sum += mags[i] * freq;
    }
    if (mag_sum <= m_cfg.epsilon) {
        return 0.0f;
    }
    return weighted_sum / mag_sum;
}

std::vector<float> FeatureExtractor::ComputeLegacyEnergyBands(const std::vector<float>& mags,
                                                              uint32_t n_bands) const {
    std::vector<float> bands(n_bands, 0.0f);
    if (mags.empty()) {
        return bands;
    }

    for (size_t i = 0; i < mags.size(); ++i) {
        const uint32_t band = std::min<uint32_t>(
            static_cast<uint32_t>((static_cast<uint64_t>(i) * n_bands) / mags.size()),
            n_bands - 1);
        bands[band] += mags[i] * mags[i];
    }
    return bands;
}

} // namespace EchoRadar
