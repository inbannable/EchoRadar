#pragma once
#include "../common/Types.h"
#include <string>
#include <vector>

namespace EchoRadar {

/// K-Nearest-Neighbour direction estimator.
/// Trained offline from a labelled FeatureVector dataset; infers angle at runtime.
/// No neural network required.
class KNNDirectionEstimator {
public:
    struct Config {
        int   k{5};             // number of nearest neighbours
        float max_distance{1e9f}; // reject if nearest neighbour farther than this
    };

    explicit KNNDirectionEstimator(Config cfg = {});
    ~KNNDirectionEstimator() = default;

    // ── Training ─────────────────────────────────────────────────────────────

    /// Add one training sample.
    void AddSample(const FeatureVector& fv, float angle_deg);

    /// Persist / load dataset from disk.
    bool Save(const std::string& path) const;
    bool Load(const std::string& path);

    // ── Inference ─────────────────────────────────────────────────────────────

    /// Estimate direction from a live FeatureVector.
    DirectionEstimate Estimate(const FeatureVector& fv) const;

    std::size_t SampleCount() const { return m_samples.size(); }

private:
    struct Sample {
        FeatureVector fv;
        float         angle_deg;
    };

    Config              m_cfg;
    std::vector<Sample> m_samples;

    float Distance(const FeatureVector& a, const FeatureVector& b) const;
};

} // namespace EchoRadar
