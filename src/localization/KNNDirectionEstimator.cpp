#include "KNNDirectionEstimator.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>
#include <stdexcept>

namespace EchoRadar {

KNNDirectionEstimator::KNNDirectionEstimator(Config cfg) : m_cfg(cfg) {
    if (cfg.k <= 0) throw std::invalid_argument("k must be > 0");
}

void KNNDirectionEstimator::AddSample(const FeatureVector& fv, float angle_deg) {
    m_samples.push_back({fv, angle_deg});
}

bool KNNDirectionEstimator::Save(const std::string& path) const {
    // TODO (Milestone 8): serialise m_samples to binary file.
    (void)path;
    return false;
}

bool KNNDirectionEstimator::Load(const std::string& path) {
    // TODO (Milestone 8): deserialise m_samples from binary file.
    (void)path;
    return false;
}

DirectionEstimate KNNDirectionEstimator::Estimate(const FeatureVector& fv) const {
    DirectionEstimate result;
    result.timestamp = fv.timestamp;

    if (m_samples.empty()) return result;

    // Compute distance to every training sample
    std::vector<std::pair<float, float>> dist_angle; // (distance, angle)
    dist_angle.reserve(m_samples.size());
    for (const auto& s : m_samples) {
        dist_angle.emplace_back(Distance(fv, s.fv), s.angle_deg);
    }

    // Partial sort to find k nearest
    int k = std::min(static_cast<std::size_t>(m_cfg.k), dist_angle.size());
    std::partial_sort(dist_angle.begin(), dist_angle.begin() + k, dist_angle.end(),
                      [](const auto& a, const auto& b){ return a.first < b.first; });

    if (dist_angle[0].first > m_cfg.max_distance) return result; // too far

    // TODO (Milestone 8): circular-mean of k nearest angles, weighted by 1/d.
    // Stub: return nearest neighbour angle.
    result.angle      = dist_angle[0].second;
    result.confidence = 0.5f;
    return result;
}

float KNNDirectionEstimator::Distance(const FeatureVector& a,
                                       const FeatureVector& b) const {
    // Euclidean in feature space (ILD, ITD, centroid, rolloff, bands)
    float d = 0.0f;
    d += (a.ild - b.ild) * (a.ild - b.ild);
    d += (a.itd - b.itd) * (a.itd - b.itd);
    d += (a.spectral_centroid - b.spectral_centroid) *
         (a.spectral_centroid - b.spectral_centroid);
    d += (a.spectral_rolloff - b.spectral_rolloff) *
         (a.spectral_rolloff - b.spectral_rolloff);
    std::size_t nb = std::min(a.energy_bands.size(), b.energy_bands.size());
    for (std::size_t i = 0; i < nb; ++i) {
        float diff = a.energy_bands[i] - b.energy_bands[i];
        d += diff * diff;
    }
    return std::sqrt(d);
}

} // namespace EchoRadar
