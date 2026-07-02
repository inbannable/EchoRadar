#include "GunshotEventDetector.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace EchoRadar {

namespace {
constexpr float kScoreScale = 2.2f;
constexpr int kMaxHistoryFrames = 256;
}

GunshotEventDetector::GunshotEventDetector(const EventDetectorConfig& cfg) : m_cfg(cfg) {
    if (m_cfg.triggerThreshold <= m_cfg.releaseThreshold) {
        throw std::invalid_argument("triggerThreshold must be greater than releaseThreshold");
    }
    if (m_cfg.minEventFrames <= 0 ||
        m_cfg.maxEventFrames < m_cfg.minEventFrames ||
        m_cfg.maxRiseFrames <= 0 ||
        m_cfg.maxDecayFrames <= 0 ||
        m_cfg.maxPeakWidthFrames <= 0) {
        throw std::invalid_argument("Invalid temporal validation configuration");
    }
    if (m_cfg.peakLookaheadFrames <= 0 ||
        m_cfg.minPeakIntervalFrames <= 0 ||
        m_cfg.minEventSeparationFrames <= 0 ||
        m_cfg.refractoryFrames < 0) {
        throw std::invalid_argument("Invalid peak/event separation configuration");
    }
    if (m_cfg.scoreNormalizationWindow <= 0 && m_cfg.scoreEmaAlpha <= 0.0f) {
        throw std::invalid_argument("Either scoreNormalizationWindow or scoreEmaAlpha must be usable");
    }
}

void GunshotEventDetector::Reset() {
    m_state = DetectorState::Idle;
    m_lastScore = 0.0f;
    m_lastConfidence = 0.0f;
    m_scoreBaseline = 0.0f;
    m_logEnergyBaseline = 0.0f;
    m_hasBaseline = false;
    m_lastDecisionFrame = -1000000;
    m_lastCandidateFrame = -1000000;
    m_lastAcceptedPeakFrame = -1000000;
    m_frameHistory.clear();
    m_pendingPeaks.clear();
    m_readyEvents.clear();
    m_readyDecisions.clear();
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

size_t GunshotEventDetector::GetAvailableDecisions() const {
    return m_readyDecisions.size();
}

bool GunshotEventDetector::PopDecision(CandidateDecision& outDecision) {
    if (m_readyDecisions.empty()) {
        return false;
    }

    outDecision = m_readyDecisions.front();
    m_readyDecisions.pop_front();
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
    const float spectralFlux = Clamp01(features.spectralFlux * 1.6f);
    const float hfEnergyRatio = Clamp01(features.hfEnergyRatio);
    const float transientScore = Clamp01(features.transientScore);
    const float centroidLift = Clamp01((features.spectralCentroid - 700.0f) / 2400.0f);

    const float rawScore =
        (m_cfg.wEnergyRise * energyRise) +
        (m_cfg.wSpectralFlux * spectralFlux) +
        (m_cfg.wHfEnergyRatio * hfEnergyRatio) +
        (m_cfg.wTransientScore * transientScore) +
        (0.06f * centroidLift);

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

    return Clamp01(deviation * kScoreScale) * (0.48f + 0.52f * gate);
}

bool GunshotEventDetector::DetectLocalPeak(PendingPeak& outPeak) const {
    if (m_frameHistory.size() < 3) {
        return false;
    }

    const size_t n = m_frameHistory.size();
    const ScoredFrame& left = m_frameHistory[n - 3];
    const ScoredFrame& mid = m_frameHistory[n - 2];
    const ScoredFrame& right = m_frameHistory[n - 1];

    if (mid.score < m_cfg.triggerThreshold) {
        return false;
    }

    const bool notLowerThanNeighbors = (mid.score >= left.score) && (mid.score >= right.score);
    const bool hasStrictSide = (mid.score > left.score) || (mid.score > right.score);
    if (!notLowerThanNeighbors || !hasStrictSide) {
        return false;
    }

    outPeak.frameIndex = mid.frameIndex;
    outPeak.timeSec = mid.timeSec;
    outPeak.score = mid.score;
    return true;
}

void GunshotEventDetector::QueuePeakCandidate(const PendingPeak& peak) {
    if (!m_pendingPeaks.empty()) {
        PendingPeak& prev = m_pendingPeaks.back();
        if ((peak.frameIndex - prev.frameIndex) <= 1) {
            if (peak.score >= prev.score) {
                prev = peak;
            }
            return;
        }
    }
    m_pendingPeaks.push_back(peak);
}

bool GunshotEventDetector::BuildCandidateMetrics(const PendingPeak& peak, CandidateMetrics& outMetrics) const {
    int peakPos = -1;
    for (size_t i = 0; i < m_frameHistory.size(); ++i) {
        if (m_frameHistory[i].frameIndex == peak.frameIndex) {
            peakPos = static_cast<int>(i);
            break;
        }
    }
    if (peakPos < 0) {
        return false;
    }

    const float peakScore = m_frameHistory[peakPos].score;
    const float boundaryScore = std::max(m_cfg.releaseThreshold, peakScore * m_cfg.peakBoundaryRatio);

    int startPos = peakPos;
    for (int i = peakPos - 1; i >= 0; --i) {
        if ((m_frameHistory[peakPos].frameIndex - m_frameHistory[i].frameIndex) >= m_cfg.maxEventFrames) {
            break;
        }
        if (m_frameHistory[i].score < boundaryScore) {
            break;
        }
        startPos = i;
    }

    int endPos = peakPos;
    for (int i = peakPos + 1; i < static_cast<int>(m_frameHistory.size()); ++i) {
        if ((m_frameHistory[i].frameIndex - m_frameHistory[peakPos].frameIndex) >= m_cfg.maxEventFrames) {
            break;
        }
        if (m_frameHistory[i].score < boundaryScore) {
            break;
        }
        endPos = i;
    }

    const int durationFrames = m_frameHistory[endPos].frameIndex - m_frameHistory[startPos].frameIndex + 1;
    const int riseFrames = m_frameHistory[peakPos].frameIndex - m_frameHistory[startPos].frameIndex;
    const int decayFrames = m_frameHistory[endPos].frameIndex - m_frameHistory[peakPos].frameIndex;

    const float widthThreshold = peakScore * m_cfg.peakWidthRatio;
    int widthStart = peakPos;
    while (widthStart > startPos && m_frameHistory[widthStart - 1].score >= widthThreshold) {
        --widthStart;
    }
    int widthEnd = peakPos;
    while (widthEnd < endPos && m_frameHistory[widthEnd + 1].score >= widthThreshold) {
        ++widthEnd;
    }
    const int peakWidthFrames = m_frameHistory[widthEnd].frameIndex - m_frameHistory[widthStart].frameIndex + 1;

    const float leftBase = m_frameHistory[startPos].score;
    const float rightBase = m_frameHistory[endPos].score;
    const float prominence = peakScore - std::min(leftBase, rightBase);
    const float riseSlope = riseFrames > 0 ? (peakScore - leftBase) / static_cast<float>(riseFrames) : (peakScore - leftBase);
    const float decaySlope = decayFrames > 0 ? (peakScore - rightBase) / static_cast<float>(decayFrames) : (peakScore - rightBase);

    outMetrics.startPos = startPos;
    outMetrics.peakPos = peakPos;
    outMetrics.endPos = endPos;
    outMetrics.durationFrames = durationFrames;
    outMetrics.riseFrames = riseFrames;
    outMetrics.decayFrames = decayFrames;
    outMetrics.peakWidthFrames = peakWidthFrames;
    outMetrics.peakScore = peakScore;
    outMetrics.prominence = prominence;
    outMetrics.riseSlope = riseSlope;
    outMetrics.decaySlope = decaySlope;
    return true;
}

bool GunshotEventDetector::PassTemporalValidation(const CandidateMetrics& metrics) const {
    if (metrics.durationFrames < m_cfg.minEventFrames) {
        return false;
    }
    if (metrics.durationFrames > m_cfg.maxEventFrames) {
        return false;
    }
    if (metrics.riseFrames > m_cfg.maxRiseFrames) {
        return false;
    }
    if (metrics.decayFrames > m_cfg.maxDecayFrames) {
        return false;
    }
    if (metrics.peakWidthFrames > m_cfg.maxPeakWidthFrames) {
        return false;
    }
    return true;
}

bool GunshotEventDetector::PassFalsePositiveFilter(const ScoredFrame& peakFrame, const CandidateMetrics& metrics) const {
    const AudioFeatures& f = peakFrame.features;
    int evidenceCount = 0;
    if (f.energyRise >= 0.18f) {
        ++evidenceCount;
    }
    if (f.spectralFlux >= 0.16f) {
        ++evidenceCount;
    }
    if (f.transientScore >= 0.16f) {
        ++evidenceCount;
    }
    if (f.hfEnergyRatio >= 0.16f) {
        ++evidenceCount;
    }
    if (f.spectralCentroid >= 1200.0f) {
        ++evidenceCount;
    }
    if (f.spectralFlatness >= 0.08f && f.spectralFlatness <= 0.88f) {
        ++evidenceCount;
    }

    const bool hasHighFrequencyCue = (f.hfEnergyRatio >= 0.14f) || (f.spectralCentroid >= 1150.0f);
    const bool footstepLike = (f.spectralCentroid < 950.0f) &&
                              (f.hfEnergyRatio < 0.13f) &&
                              (metrics.durationFrames >= 4);
    const bool handlingLike = (f.transientScore < 0.12f) &&
                              (f.spectralFlux < 0.12f);
    const bool broadNoiseLike = (f.spectralFlatness > 0.90f) &&
                                (f.transientScore < 0.18f);

    if (footstepLike || handlingLike || broadNoiseLike) {
        return false;
    }
    if (!hasHighFrequencyCue) {
        return false;
    }
    if (evidenceCount < 4) {
        return false;
    }
    return true;
}

float GunshotEventDetector::ComputeConfidence(const ScoredFrame& peakFrame, const CandidateMetrics& metrics) const {
    const AudioFeatures& f = peakFrame.features;
    const float energyRise = Clamp01((f.energyRise - 0.08f) / 0.60f);
    const float spectralFlux = Clamp01((f.spectralFlux - 0.08f) / 0.60f);
    const float transientScore = Clamp01((f.transientScore - 0.08f) / 0.60f);
    const float hfEnergyRatio = Clamp01((f.hfEnergyRatio - 0.08f) / 0.50f);
    const float centroid = Clamp01((f.spectralCentroid - 900.0f) / 2600.0f);
    const float flatnessBand = 1.0f - Clamp01(std::fabs(f.spectralFlatness - 0.35f) / 0.45f);
    const float prominence = Clamp01((metrics.prominence - m_cfg.minPeakProminence) / 0.35f);
    const float compactDuration = 1.0f - Clamp01(
        static_cast<float>(metrics.durationFrames - m_cfg.minEventFrames) /
        static_cast<float>(std::max(1, m_cfg.maxEventFrames - m_cfg.minEventFrames)));
    const float sharpPeak = 1.0f - Clamp01(
        static_cast<float>(metrics.peakWidthFrames - 1) /
        static_cast<float>(std::max(1, m_cfg.maxPeakWidthFrames - 1)));

    const float confidence = (0.30f * metrics.peakScore) +
                             (0.14f * energyRise) +
                             (0.13f * spectralFlux) +
                             (0.12f * transientScore) +
                             (0.10f * hfEnergyRatio) +
                             (0.08f * centroid) +
                             (0.05f * flatnessBand) +
                             (0.04f * prominence) +
                             (0.04f * compactDuration) +
                             (0.04f * sharpPeak);
    return Clamp01(confidence);
}

void GunshotEventDetector::EmitDecision(CandidateDecisionType type, const PendingPeak& peak, float confidence) {
    CandidateDecision decision{};
    decision.type = type;
    decision.frameIndex = peak.frameIndex;
    decision.timeSec = peak.timeSec;
    decision.candidateScore = peak.score;
    decision.confidence = Clamp01(confidence);
    m_readyDecisions.push_back(decision);
}

void GunshotEventDetector::AcceptEvent(const PendingPeak& peak,
                                       const ScoredFrame& peakFrame,
                                       const CandidateMetrics& metrics,
                                       float confidence) {
    GunshotEvent ev{};
    ev.onsetTimeSec = m_frameHistory[metrics.startPos].timeSec;
    ev.peakTimeSec = peak.timeSec;
    ev.startFrame = m_frameHistory[metrics.startPos].frameIndex;
    ev.peakFrame = peak.frameIndex;
    ev.endFrame = m_frameHistory[metrics.endPos].frameIndex;
    ev.candidateScore = peak.score;
    ev.gunshotProb = Clamp01(confidence);
    ev.leftRightBalanceAtPeak = peakFrame.features.leftRightBalance;
    ev.spectralCentroidAtPeak = peakFrame.features.spectralCentroid;
    ev.hfEnergyRatioAtPeak = peakFrame.features.hfEnergyRatio;
    ev.confidence = ev.gunshotProb;
    ev.timestamp = static_cast<uint64_t>(std::llround(ev.peakTimeSec * 1000.0));
    m_readyEvents.push_back(ev);

    m_lastAcceptedPeakFrame = peak.frameIndex;
}

const GunshotEventDetector::ScoredFrame* GunshotEventDetector::FindFrameByIndex(int frameIndex) const {
    for (const ScoredFrame& frame : m_frameHistory) {
        if (frame.frameIndex == frameIndex) {
            return &frame;
        }
    }
    return nullptr;
}

void GunshotEventDetector::EvaluateReadyPeaks(int currentFrameIndex) {
    while (!m_pendingPeaks.empty()) {
        const PendingPeak peak = m_pendingPeaks.front();
        if ((currentFrameIndex - peak.frameIndex) < m_cfg.peakLookaheadFrames) {
            break;
        }
        m_pendingPeaks.pop_front();

        EmitDecision(CandidateDecisionType::Peak, peak, 0.0f);

        if ((peak.frameIndex - m_lastCandidateFrame) < m_cfg.minPeakIntervalFrames) {
            EmitDecision(CandidateDecisionType::RejectedTemporal, peak, 0.0f);
            m_lastDecisionFrame = peak.frameIndex;
            continue;
        }
        m_lastCandidateFrame = peak.frameIndex;

        CandidateMetrics metrics{};
        if (!BuildCandidateMetrics(peak, metrics) || !PassTemporalValidation(metrics)) {
            EmitDecision(CandidateDecisionType::RejectedTemporal, peak, 0.0f);
            m_lastDecisionFrame = peak.frameIndex;
            continue;
        }

        const ScoredFrame* peakFrame = FindFrameByIndex(peak.frameIndex);
        if (peakFrame == nullptr) {
            EmitDecision(CandidateDecisionType::RejectedTemporal, peak, 0.0f);
            m_lastDecisionFrame = peak.frameIndex;
            continue;
        }

        if (!PassFalsePositiveFilter(*peakFrame, metrics)) {
            EmitDecision(CandidateDecisionType::RejectedFalsePositive, peak, 0.0f);
            m_lastDecisionFrame = peak.frameIndex;
            continue;
        }

        const float confidence = ComputeConfidence(*peakFrame, metrics);
        m_lastConfidence = confidence;

        if ((peak.frameIndex - m_lastAcceptedPeakFrame) < m_cfg.minEventSeparationFrames) {
            EmitDecision(CandidateDecisionType::RejectedTemporal, peak, confidence);
            m_lastDecisionFrame = peak.frameIndex;
            continue;
        }

        if (confidence < m_cfg.minConfidence) {
            EmitDecision(CandidateDecisionType::RejectedConfidence, peak, confidence);
            m_lastDecisionFrame = peak.frameIndex;
            continue;
        }

        AcceptEvent(peak, *peakFrame, metrics, confidence);
        EmitDecision(CandidateDecisionType::Accepted, peak, confidence);
        m_lastDecisionFrame = peak.frameIndex;
    }
}

void GunshotEventDetector::UpdateState(int currentFrameIndex) {
    if (m_lastScore >= m_cfg.releaseThreshold || !m_pendingPeaks.empty()) {
        m_state = DetectorState::InCandidate;
        return;
    }
    if ((currentFrameIndex - m_lastDecisionFrame) <= m_cfg.refractoryFrames) {
        m_state = DetectorState::Cooldown;
        return;
    }
    m_state = DetectorState::Idle;
}

void GunshotEventDetector::PushFrame(const AudioFeatures& features,
                                     double frameTimeSec,
                                     int frameIndex) {
    m_lastScore = ComputeFrameScore(features);
    m_frameHistory.push_back(ScoredFrame{features, m_lastScore, frameTimeSec, frameIndex});
    while (m_frameHistory.size() > kMaxHistoryFrames) {
        m_frameHistory.pop_front();
    }

    PendingPeak peak{};
    if (DetectLocalPeak(peak)) {
        QueuePeakCandidate(peak);
    }

    EvaluateReadyPeaks(frameIndex);
    UpdateState(frameIndex);
}

} // namespace EchoRadar
