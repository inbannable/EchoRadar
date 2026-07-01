#include "GunshotEventDetector.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace EchoRadar {

namespace {
constexpr float kScoreScale = 2.4f;
}

GunshotEventDetector::GunshotEventDetector(const EventDetectorConfig& cfg) : m_cfg(cfg) {
    if (m_cfg.triggerThreshold <= m_cfg.releaseThreshold) {
        throw std::invalid_argument("triggerThreshold must be greater than releaseThreshold");
    }
    if (m_cfg.minEventFrames <= 0 || m_cfg.maxMergeGapFrames < 0 || m_cfg.refractoryFrames < 0) {
        throw std::invalid_argument("Frame timing thresholds must be non-negative and minEventFrames > 0");
    }
    if (m_cfg.scoreNormalizationWindow <= 0 && m_cfg.scoreEmaAlpha <= 0.0f) {
        throw std::invalid_argument("Either scoreNormalizationWindow or scoreEmaAlpha must be usable");
    }
}

void GunshotEventDetector::Reset() {
    m_state = DetectorState::Idle;
    m_lastScore = 0.0f;
    m_scoreBaseline = 0.0f;
    m_logEnergyBaseline = 0.0f;
    m_hasBaseline = false;
    m_cooldownRemaining = 0;
    m_candidate = {};
    m_readyEvents.clear();
}

size_t GunshotEventDetector::GetAvailableEvents() const {
    return m_readyEvents.size();
}

bool GunshotEventDetector::PopEvent(GunshotEvent& outEvent) {
    if (m_readyEvents.empty()) {
        return false;
    }

    outEvent = m_readyEvents.front();
    m_readyEvents.pop_front();
    return true;
}

void GunshotEventDetector::SetTriggerThreshold(float triggerThreshold) {
    if (triggerThreshold <= m_cfg.releaseThreshold) {
        throw std::invalid_argument("triggerThreshold must be greater than releaseThreshold");
    }
    m_cfg.triggerThreshold = triggerThreshold;
}

float GunshotEventDetector::Clamp01(float value) const {
    return std::clamp(value, 0.0f, 1.0f);
}

float GunshotEventDetector::ComputeFrameScore(const AudioFeatures& features) {
    const float energyRise = Clamp01(features.energyRise * 2.5f);
    const float spectralFlux = Clamp01(features.spectralFlux * 1.5f);
    const float hfEnergyRatio = Clamp01(features.hfEnergyRatio);
    const float transientScore = Clamp01(features.transientScore);

    const float rawScore =
        (m_cfg.wEnergyRise * energyRise) +
        (m_cfg.wSpectralFlux * spectralFlux) +
        (m_cfg.wHfEnergyRatio * hfEnergyRatio) +
        (m_cfg.wTransientScore * transientScore);

    const float alpha = m_cfg.scoreEmaAlpha > 0.0f
        ? m_cfg.scoreEmaAlpha
        : (2.0f / static_cast<float>(m_cfg.scoreNormalizationWindow + 1));

    if (!m_hasBaseline) {
        m_scoreBaseline = rawScore;
        m_logEnergyBaseline = features.logEnergy;
        m_hasBaseline = true;
    }

    const float deviation = std::max(0.0f, rawScore - m_scoreBaseline);
    const float gateLift = std::max(0.0f, features.logEnergy - m_logEnergyBaseline);
    const float gate = Clamp01(gateLift * 0.9f);

    m_scoreBaseline = alpha * rawScore + (1.0f - alpha) * m_scoreBaseline;
    m_logEnergyBaseline = alpha * features.logEnergy + (1.0f - alpha) * m_logEnergyBaseline;

    return Clamp01(deviation * kScoreScale) * (0.50f + 0.50f * gate);
}

void GunshotEventDetector::StartCandidate(const AudioFeatures& features,
                                          double frameTimeSec,
                                          int frameIndex,
                                          float score) {
    m_candidate = {};
    m_candidate.active = true;
    m_candidate.startFrame = frameIndex;
    m_candidate.peakFrame = frameIndex;
    m_candidate.endFrame = frameIndex;
    m_candidate.startTimeSec = frameTimeSec;
    m_candidate.peakTimeSec = frameTimeSec;
    m_candidate.endTimeSec = frameTimeSec;
    m_candidate.lastStrongFrame = frameIndex;
    m_candidate.lastStrongTimeSec = frameTimeSec;
    m_candidate.peakScore = score;
    m_candidate.leftRightBalanceAtPeak = features.leftRightBalance;
    m_candidate.spectralCentroidAtPeak = features.spectralCentroid;
    m_candidate.hfEnergyRatioAtPeak = features.hfEnergyRatio;
    m_candidate.belowReleaseFrames = 0;
    m_state = DetectorState::InCandidate;
}

void GunshotEventDetector::UpdateCandidate(const AudioFeatures& features,
                                          double frameTimeSec,
                                          int frameIndex,
                                          float score) {
    m_candidate.endFrame = frameIndex;
    m_candidate.endTimeSec = frameTimeSec;
    if (score >= m_cfg.releaseThreshold) {
        m_candidate.lastStrongFrame = frameIndex;
        m_candidate.lastStrongTimeSec = frameTimeSec;
    }

    if (score >= m_candidate.peakScore) {
        m_candidate.peakScore = score;
        m_candidate.peakFrame = frameIndex;
        m_candidate.peakTimeSec = frameTimeSec;
        m_candidate.leftRightBalanceAtPeak = features.leftRightBalance;
        m_candidate.spectralCentroidAtPeak = features.spectralCentroid;
        m_candidate.hfEnergyRatioAtPeak = features.hfEnergyRatio;
    }

    if (score >= m_cfg.releaseThreshold) {
        m_candidate.belowReleaseFrames = 0;
    } else {
        ++m_candidate.belowReleaseFrames;
    }
}

void GunshotEventDetector::FinalizeCandidate(bool discard) {
    if (!m_candidate.active) {
        m_state = DetectorState::Idle;
        return;
    }

    const int durationFrames = m_candidate.endFrame - m_candidate.startFrame + 1;
    if (!discard && durationFrames >= m_cfg.minEventFrames) {
        GunshotEvent ev{};
        ev.onsetTimeSec = m_candidate.startTimeSec;
        ev.peakTimeSec = m_candidate.peakTimeSec;
        ev.startFrame = m_candidate.startFrame;
        ev.peakFrame = m_candidate.peakFrame;
        ev.endFrame = m_candidate.lastStrongFrame;
        ev.candidateScore = m_candidate.peakScore;
        ev.gunshotProb = Clamp01(m_candidate.peakScore);
        ev.leftRightBalanceAtPeak = m_candidate.leftRightBalanceAtPeak;
        ev.spectralCentroidAtPeak = m_candidate.spectralCentroidAtPeak;
        ev.hfEnergyRatioAtPeak = m_candidate.hfEnergyRatioAtPeak;
        ev.confidence = ev.gunshotProb;
        ev.timestamp = static_cast<uint64_t>(std::llround(ev.peakTimeSec * 1000.0));
        m_readyEvents.push_back(ev);
    }

    m_candidate = {};
    m_state = DetectorState::Cooldown;
    m_cooldownRemaining = m_cfg.refractoryFrames;
}

void GunshotEventDetector::PushFrame(const AudioFeatures& features,
                                     double frameTimeSec,
                                     int frameIndex) {
    m_lastScore = ComputeFrameScore(features);

    if (m_state == DetectorState::Cooldown) {
        if (m_cooldownRemaining > 0) {
            --m_cooldownRemaining;
            return;
        }
        m_state = DetectorState::Idle;
    }

    if (m_state == DetectorState::Idle) {
        if (m_lastScore >= m_cfg.triggerThreshold) {
            StartCandidate(features, frameTimeSec, frameIndex, m_lastScore);
        }
        return;
    }

    if (m_state == DetectorState::InCandidate) {
        UpdateCandidate(features, frameTimeSec, frameIndex, m_lastScore);

        if (m_candidate.belowReleaseFrames > m_cfg.maxMergeGapFrames) {
            m_candidate.endFrame = m_candidate.lastStrongFrame;
            m_candidate.endTimeSec = m_candidate.lastStrongTimeSec;
            FinalizeCandidate(false);
            return;
        }
    }
}

} // namespace EchoRadar
