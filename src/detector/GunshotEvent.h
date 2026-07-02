#pragma once
#include <cstdint>

namespace EchoRadar {

/// Event-level gunshot detection result.
/// Legacy `confidence` / `timestamp` fields remain for compatibility with older
/// monitors and stubs that still consume the original event shape.
struct GunshotEvent {
    double onsetTimeSec{0.0};
    double peakTimeSec{0.0};

    int startFrame{0};
    int peakFrame{0};
    int endFrame{0};

    float candidateScore{0.0f};
    float gunshotProb{0.0f};

    float leftRightBalanceAtPeak{0.0f};
    float spectralCentroidAtPeak{0.0f};
    float hfEnergyRatioAtPeak{0.0f};

    // Legacy compatibility.
    float confidence{0.0f};
    uint64_t timestamp{0};
};

enum class CandidateDecisionType {
    Peak,
    RejectedTemporal,
    RejectedFalsePositive,
    RejectedConfidence,
    Accepted
};

struct CandidateDecision {
    CandidateDecisionType type{CandidateDecisionType::Peak};
    int frameIndex{0};
    double timeSec{0.0};
    float candidateScore{0.0f};
    float confidence{0.0f};
};

struct EventDetectorConfig {
    float triggerThreshold{0.56f};
    float releaseThreshold{0.32f};

    int minEventFrames{1};
    int maxEventFrames{12};
    int maxRiseFrames{4};
    int maxDecayFrames{8};
    int maxPeakWidthFrames{4};
    int peakLookaheadFrames{3};
    int minPeakIntervalFrames{2};
    int minEventSeparationFrames{2};
    int refractoryFrames{2};

    int scoreNormalizationWindow{16};
    float scoreEmaAlpha{0.18f};

    float wEnergyRise{0.36f};
    float wSpectralFlux{0.28f};
    float wHfEnergyRatio{0.18f};
    float wTransientScore{0.18f};

    float minPeakProminence{0.08f};
    float minRiseSlope{0.03f};
    float minDecaySlope{0.02f};
    float peakBoundaryRatio{0.55f};
    float peakWidthRatio{0.65f};
    float minConfidence{0.50f};
};

enum class DetectorState {
    Idle,
    InCandidate,
    Cooldown
};

} // namespace EchoRadar
