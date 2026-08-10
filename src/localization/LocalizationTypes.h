#pragma once

#include <array>
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
    bool localizeFootsteps{true};
    bool localizeGunshots{true};
    uint32_t sampleWindowMs{240};
    uint32_t preOnsetMs{40};
    float minimumConfidence{0.35f};
    bool showSecondaryDirection{false};
    float secondaryRatio{0.75f};
    float secondaryMinimumSeparationDegrees{60.0f};
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
    float primaryAngleDegrees{0.0f};
    float confidence{0.0f};
    float uncertaintyDegrees{180.0f};
    std::optional<float> secondaryAngleDegrees;
    float secondaryConfidence{0.0f};
    DirectionProfileSource profileSource{DirectionProfileSource::Synthetic};
    DirectionStatus status{DirectionStatus::AudioUnavailable};
    double inferenceMilliseconds{0.0};
    std::array<float, 24> probabilities{};
};

float WrapDirectionDegrees(float angle);
float CircularDistanceDegrees(float left, float right);

} // namespace EchoRadar
