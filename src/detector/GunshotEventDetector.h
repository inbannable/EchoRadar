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

    float GetLastScore() const { return m_lastScore; }
    DetectorState GetState() const { return m_state; }
    const EventDetectorConfig& GetConfig() const { return m_cfg; }

private:
    struct CandidateState {
        bool  active{false};
        int   startFrame{0};
        int   peakFrame{0};
        int   endFrame{0};
        double startTimeSec{0.0};
        double peakTimeSec{0.0};
        double endTimeSec{0.0};
        int   lastStrongFrame{0};
        double lastStrongTimeSec{0.0};
        float peakScore{0.0f};
        float leftRightBalanceAtPeak{0.0f};
        float spectralCentroidAtPeak{0.0f};
        float hfEnergyRatioAtPeak{0.0f};
        int   belowReleaseFrames{0};
    };

    EventDetectorConfig m_cfg;
    DetectorState       m_state{DetectorState::Idle};
    float               m_lastScore{0.0f};
    float               m_scoreBaseline{0.0f};
    float               m_logEnergyBaseline{0.0f};
    bool                m_hasBaseline{false};
    int                 m_cooldownRemaining{0};
    CandidateState      m_candidate{};
    std::deque<GunshotEvent> m_readyEvents;

    float ComputeFrameScore(const AudioFeatures& features);
    float Clamp01(float value) const;
    void  StartCandidate(const AudioFeatures& features, double frameTimeSec, int frameIndex, float score);
    void  UpdateCandidate(const AudioFeatures& features, double frameTimeSec, int frameIndex, float score);
    void  FinalizeCandidate(bool discard);
};

} // namespace EchoRadar
