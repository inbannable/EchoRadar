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

struct EventDetectorConfig {
    float triggerThreshold{0.62f};
    float releaseThreshold{0.38f};

    int minEventFrames{2};
    int maxMergeGapFrames{2};
    int refractoryFrames{5};

    int scoreNormalizationWindow{16};
    float scoreEmaAlpha{0.18f};

    float wEnergyRise{0.36f};
    float wSpectralFlux{0.28f};
    float wHfEnergyRatio{0.18f};
    float wTransientScore{0.18f};
};

enum class DetectorState {
    Idle,
    InCandidate,
    Cooldown
};

} // namespace EchoRadar
