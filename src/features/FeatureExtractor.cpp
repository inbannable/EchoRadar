#include "FeatureExtractor.h"
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace EchoRadar {

FeatureExtractor::FeatureExtractor(Config cfg) : m_cfg(cfg) {
    if (cfg.n_energy_bands == 0)
        throw std::invalid_argument("n_energy_bands must be > 0");
}

FeatureVector FeatureExtractor::Extract(const Spectrogram& left,
                                        const Spectrogram& right,
                                        uint64_t           ts_ms) const {
    FeatureVector fv;
    fv.timestamp = ts_ms;

    if (left.magnitude.empty() || right.magnitude.empty()) return fv;
    std::size_t fi = left.magnitude.size() - 1; // use latest frame

    fv.ild              = ComputeILD(left, right, fi);
    fv.itd              = ComputeITD(left, right, fi);
    fv.spectral_centroid = ComputeSpectralCentroid(left, fi);
    fv.spectral_rolloff  = ComputeSpectralRolloff(left, fi);
    fv.energy_bands      = ComputeEnergyBands(left, fi);

    return fv;
}

float FeatureExtractor::ComputeILD(const Spectrogram& left,
                                    const Spectrogram& right,
                                    std::size_t fi) const {
    // TODO (Milestone 7): sum power across bins, return 20*log10(L/R).
    (void)left; (void)right; (void)fi;
    return 0.0f;
}

float FeatureExtractor::ComputeITD(const Spectrogram& left,
                                    const Spectrogram& right,
                                    std::size_t fi) const {
    // TODO (Milestone 7): cross-correlate L/R in time domain.
    (void)left; (void)right; (void)fi;
    return 0.0f;
}

float FeatureExtractor::ComputeSpectralCentroid(const Spectrogram& spec,
                                                  std::size_t fi) const {
    // TODO (Milestone 7): Σ(f * |X[f]|) / Σ|X[f]|
    (void)spec; (void)fi;
    return 0.0f;
}

float FeatureExtractor::ComputeSpectralRolloff(const Spectrogram& spec,
                                                std::size_t fi,
                                                float rolloff_pct) const {
    // TODO (Milestone 7): frequency below which rolloff_pct % of energy lies.
    (void)spec; (void)fi; (void)rolloff_pct;
    return 0.0f;
}

std::vector<float> FeatureExtractor::ComputeEnergyBands(const Spectrogram& spec,
                                                          std::size_t fi) const {
    // TODO (Milestone 7): split spectrum into n_energy_bands equal log-spaced bands.
    (void)spec; (void)fi;
    return std::vector<float>(m_cfg.n_energy_bands, 0.0f);
}

} // namespace EchoRadar
