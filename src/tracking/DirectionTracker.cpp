#include "DirectionTracker.h"
#include <cmath>

namespace EchoRadar {

DirectionTracker::DirectionTracker(Config cfg) : m_cfg(cfg) {}

DirectionEstimate DirectionTracker::Update(const DirectionEstimate& raw) {
    if (!m_initialised) {
        m_angle       = raw.angle;
        m_variance    = m_cfg.initial_variance;
        m_initialised = true;
    } else {
        // ── Predict ───────────────────────────────────────────────────────────
        // Angle model: constant; variance grows by process noise each step.
        m_variance += m_cfg.process_noise;

        // ── Update ────────────────────────────────────────────────────────────
        float K         = m_variance / (m_variance + m_cfg.measurement_noise);
        float diff      = AngleDiff(raw.angle, m_angle);
        m_angle         = WrapAngle(m_angle + K * diff);
        m_variance      = (1.0f - K) * m_variance;
    }

    DirectionEstimate out;
    out.angle      = m_angle;
    out.confidence = raw.confidence;
    out.timestamp  = raw.timestamp;
    return out;
}

void DirectionTracker::Reset() {
    m_initialised = false;
    m_angle       = 0.0f;
    m_variance    = 0.0f;
}

float DirectionTracker::AngleDiff(float a, float b) {
    float d = std::fmod(a - b + 540.0f, 360.0f) - 180.0f;
    return d;
}

float DirectionTracker::WrapAngle(float a) {
    return std::fmod(a + 360.0f, 360.0f);
}

} // namespace EchoRadar
