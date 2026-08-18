#pragma once

#include <recognition/SoundRecognitionTypes.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace EchoRadar {

enum class HeadphoneEqProfile : uint8_t {
    Natural,
    Crisp,
    Smooth,
};

enum class SpatialEnhancementState : uint8_t {
    Off,
    On,
    Unknown,
};

enum class DirectionProfileSource : uint8_t {
    Synthetic,
    Calibrated,
};

enum class DirectionStatus : uint8_t {
    Estimated,
    LowConfidence,
    Disabled,
    AudioUnavailable,
    ModelUnavailable,
    InferenceFailed,
};

inline const char* ToString(HeadphoneEqProfile profile) {
    switch (profile) {
    case HeadphoneEqProfile::Natural: return "natural";
    case HeadphoneEqProfile::Crisp: return "crisp";
    case HeadphoneEqProfile::Smooth: return "smooth";
    }
    return "natural";
}

inline const char* ToString(SpatialEnhancementState state) {
    switch (state) {
    case SpatialEnhancementState::Off: return "off";
    case SpatialEnhancementState::On: return "on";
    case SpatialEnhancementState::Unknown: return "unknown";
    }
    return "unknown";
}

inline const char* ToString(DirectionProfileSource source) {
    return source == DirectionProfileSource::Calibrated ? "calibrated" : "synthetic";
}

inline const char* ToString(DirectionStatus status) {
    switch (status) {
    case DirectionStatus::Estimated: return "estimated";
    case DirectionStatus::LowConfidence: return "low-confidence";
    case DirectionStatus::Disabled: return "disabled";
    case DirectionStatus::AudioUnavailable: return "audio-unavailable";
    case DirectionStatus::ModelUnavailable: return "model-unavailable";
    case DirectionStatus::InferenceFailed: return "inference-failed";
    }
    return "audio-unavailable";
}

struct AudioProfile {
    std::string name{"Default"};
    HeadphoneEqProfile eqProfile{HeadphoneEqProfile::Natural};
    float leftRightIsolationPercent{0.0f};
    bool perspectiveCorrection{true};
    float displayAspectRatio{16.0f / 9.0f};
    SpatialEnhancementState spatialEnhancement{SpatialEnhancementState::Unknown};
    std::string outputEndpointId;

    std::string StableKey() const;
};

struct LocalizationTuning {
    struct PeakWindowTuning {
        uint32_t beforePeakMs{18};
        uint32_t afterPeakMs{150};
        uint32_t envelopeSmoothingMs{4};
        float minimumPeakToNoiseDb{6.0f};
        float minimumActiveFrameFraction{0.02f};
    };

    bool localizeFootsteps{true};
    bool localizeGunshots{true};
    // Legacy broad-window settings are retained for event search and settings
    // migration. The class-specific peak windows below are authoritative for
    // feature extraction.
    uint32_t sampleWindowMs{240};
    uint32_t preOnsetMs{40};
    PeakWindowTuning footstepPeak{};
    PeakWindowTuning gunshotPeak{8, 75, 4, 7.0f, 0.015f};
    float minimumConfidence{0.35f};
    bool showSecondaryDirection{false};
    float secondaryRatio{0.75f};
    float secondaryMinimumSeparationDegrees{60.0f};

    const PeakWindowTuning& PeakWindowFor(SoundClass soundClass) const;
};

struct OverlaySettings {
    enum class Visibility : uint8_t {
        Off,
        Cs2Only,
        Always,
    };

    Visibility visibility{Visibility::Cs2Only};
    float radiusPixels{110.0f};
    float thicknessPixels{8.0f};
    float opacity{0.90f};
    float offsetX{0.0f};
    float offsetY{0.0f};
    float footstepLifetimeSeconds{1.2f};
    float gunshotLifetimeSeconds{0.8f};
    bool showCenterDot{false};
};

struct DirectionResult {
    uint64_t eventId{0};
    uint64_t sceneId{0};
    float primaryAngleDegrees{0.0f};
    float primaryElevationDegrees{0.0f};
    float confidence{0.0f};
    float uncertaintyDegrees{180.0f};
    std::optional<float> secondaryAngleDegrees;
    std::optional<float> secondaryElevationDegrees;
    float secondaryConfidence{0.0f};
    DirectionProfileSource profileSource{DirectionProfileSource::Synthetic};
    DirectionStatus status{DirectionStatus::AudioUnavailable};
    double inferenceMilliseconds{0.0};
    uint32_t featureSchemaVersion{0};
    uint32_t mapperVersion{0};
    uint64_t peakSample{0};
    uint64_t clipStartSample{0};
    uint64_t clipEndSample{0};
    float peakToNoiseDb{0.0f};
    float activeFrameFraction{0.0f};
    float gccQuality{0.0f};
    std::array<float, 24> probabilities{};
};

/// One public source returned by the scene-level 3D direction model.  Class is
/// intentionally omitted; classes are only used internally for filtering.
struct DirectionSourceEstimate {
    float azimuthDegrees{0.0f};
    float elevationDegrees{0.0f};
    float confidence{0.0f};
    float uncertaintyDegrees{180.0f};
};

/// Shared localization result attached to every recognizer event in a scene.
struct DirectionSceneResult {
    static constexpr size_t kMaximumSources = 3;
    static constexpr uint8_t kGunshotClassBit = 1u << 0u;
    static constexpr uint8_t kFootstepClassBit = 1u << 1u;

    uint64_t sceneId{0};
    uint64_t streamGeneration{0};
    uint64_t anchorEventSample{0};
    uint64_t sceneStartSample{0};
    uint64_t sceneEndSample{0};
    uint64_t resultDeliverySample{0};
    double deliveryMilliseconds{0.0};
    uint32_t sourceCount{0};
    std::array<DirectionSourceEstimate, kMaximumSources> sources{};
    DirectionStatus status{DirectionStatus::AudioUnavailable};
    uint8_t enabledClassMask{kGunshotClassBit | kFootstepClassBit};
    double inferenceMilliseconds{0.0};
    uint32_t featureFrames{0};
    uint32_t inputChannels{0};
    uint32_t melBins{0};
    uint32_t sampleRate{48000};
    std::string modelVersion;
    std::string preprocessingVersion;

    DirectionResult LegacyResult(uint64_t eventId) const;
};

float WrapDirectionDegrees(float angle);
float CircularDistanceDegrees(float left, float right);

} // namespace EchoRadar
