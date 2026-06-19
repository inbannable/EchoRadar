#pragma once
#include "../common/Types.h"

namespace EchoRadar {

/// Kalman-filter-based direction smoother.
/// Receives noisy DirectionEstimate samples and outputs a smoothed angle trajectory.
class DirectionTracker {
public:
    struct Config {
        float process_noise{1.0f};      // Q  – expected angle drift per step (deg²)
        float measurement_noise{10.0f}; // R  – sensor noise variance (deg²)
        float initial_variance{100.0f}; // P₀ – initial estimation uncertainty
    };

    explicit DirectionTracker(Config cfg = {});
    ~DirectionTracker() = default;

    /// Feed a new (possibly noisy) estimate; returns smoothed result.
    DirectionEstimate Update(const DirectionEstimate& raw);

    /// Reset the filter to an uninitialised state.
    void Reset();

    bool IsInitialised() const { return m_initialised; }

private:
    Config m_cfg;
    bool   m_initialised{false};

    // Kalman state
    float m_angle{0.0f};    // x̂  – angle estimate (deg)
    float m_variance{0.0f}; // P  – error covariance

    /// Wrap angle difference into [-180, +180].
    static float AngleDiff(float a, float b);
    /// Wrap angle into [0, 360).
    static float WrapAngle(float a);
};

} // namespace EchoRadar
