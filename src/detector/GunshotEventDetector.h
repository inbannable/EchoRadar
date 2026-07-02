#pragma once
#include "../common/Types.h"
#include "GunshotEvent.h"
#include <deque>

namespace EchoRadar {

/// Streaming, event-level gunshot detector over per-frame AudioFeatures.
class GunshotEventDetector {
public:
    explicit GunshotEventDetector(const EventDetectorConfig& cfg = {});

    void Reset();
    void PushFrame(const AudioFeatures& features, double frameTimeSec, int frameIndex);

    size_t GetAvailableEvents() const;
    bool   PopEvent(GunshotEvent& outEvent);
    size_t GetAvailableDecisions() const;
    bool   PopDecision(CandidateDecision& outDecision);

    float GetLastScore() const { return m_lastScore; }
    float GetLastConfidence() const { return m_lastConfidence; }
    DetectorState GetState() const { return m_state; }
    const EventDetectorConfig& GetConfig() const { return m_cfg; }
    float GetTriggerThreshold() const { return m_cfg.triggerThreshold; }
    float GetReleaseThreshold() const { return m_cfg.releaseThreshold; }
    void SetTriggerThreshold(float triggerThreshold);

private:
    struct ScoredFrame {
        AudioFeatures features{};
        float score{0.0f};
        double timeSec{0.0};
        int frameIndex{0};
    };

    struct PendingPeak {
        int frameIndex{0};
        double timeSec{0.0};
        float score{0.0f};
    };

    struct CandidateMetrics {
        int startPos{0};
        int peakPos{0};
        int endPos{0};
        int durationFrames{0};
        int riseFrames{0};
        int decayFrames{0};
        int peakWidthFrames{0};
        float peakScore{0.0f};
        float prominence{0.0f};
        float riseSlope{0.0f};
        float decaySlope{0.0f};
    };

    EventDetectorConfig m_cfg;
    DetectorState       m_state{DetectorState::Idle};
    float               m_lastScore{0.0f};
    float               m_lastConfidence{0.0f};
    float               m_scoreBaseline{0.0f};
    float               m_logEnergyBaseline{0.0f};
    bool                m_hasBaseline{false};
    int                 m_lastDecisionFrame{-1000000};
    int                 m_lastCandidateFrame{-1000000};
    int                 m_lastAcceptedPeakFrame{-1000000};
    std::deque<ScoredFrame> m_frameHistory;
    std::deque<PendingPeak> m_pendingPeaks;
    std::deque<GunshotEvent> m_readyEvents;
    std::deque<CandidateDecision> m_readyDecisions;

    float ComputeFrameScore(const AudioFeatures& features);
    float Clamp01(float value) const;
    bool  DetectLocalPeak(PendingPeak& outPeak) const;
    void  QueuePeakCandidate(const PendingPeak& peak);
    void  EvaluateReadyPeaks(int currentFrameIndex);
    bool  BuildCandidateMetrics(const PendingPeak& peak, CandidateMetrics& outMetrics) const;
    bool  PassTemporalValidation(const CandidateMetrics& metrics) const;
    bool  PassFalsePositiveFilter(const ScoredFrame& peakFrame, const CandidateMetrics& metrics) const;
    float ComputeConfidence(const ScoredFrame& peakFrame, const CandidateMetrics& metrics) const;
    void  EmitDecision(CandidateDecisionType type, const PendingPeak& peak, float confidence);
    void  AcceptEvent(const PendingPeak& peak,
                      const ScoredFrame& peakFrame,
                      const CandidateMetrics& metrics,
                      float confidence);
    const ScoredFrame* FindFrameByIndex(int frameIndex) const;
    void  UpdateState(int currentFrameIndex);
};

} // namespace EchoRadar
