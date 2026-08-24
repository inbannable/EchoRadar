#pragma once

#include <audio/AudioTypes.h>
#include <recognition/RecognitionTypes.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace EchoRadar {

inline constexpr size_t kRadarSectorCount = 24;
inline constexpr float kRadarSectorWidthDegrees = 15.0f;
inline constexpr size_t kRadarCustomCurvePointCount = 16;
inline constexpr size_t kRadarMaximumPeaks = 3;
inline constexpr float kRadarSilenceDbfs = -120.0f;

/// Frequencies used by the editable custom spectral-emphasis curve. Values
/// between control points are interpolated on a logarithmic frequency axis.
inline constexpr std::array<float, kRadarCustomCurvePointCount>
    kRadarCurveFrequenciesHz{{
        20.0f, 40.0f, 80.0f, 125.0f,
        250.0f, 500.0f, 750.0f, 1000.0f,
        1500.0f, 2000.0f, 3000.0f, 4000.0f,
        6000.0f, 8000.0f, 12000.0f, 20000.0f,
    }};

enum class RadarMode : uint8_t {
    Continuous,
    Events,
    Combined,
};

enum class RadarPreset : uint8_t {
    All,
    Footsteps,
    Gunshots,
    Custom,
};

enum class RadarRuntimeStatus : uint8_t {
    WaitingForAudio,
    Active,
    Silent,
    UnsupportedLayout,
    UnsupportedSampleRate,
    MalformedInput,

    // Descriptive aliases retained for UI and integration call sites.
    Ready = Active,
    NoSignal = Silent,
    UnsupportedAudioLayout = UnsupportedLayout,
    InvalidInput = MalformedInput,
};

inline const char* ToString(RadarMode mode) {
    switch (mode) {
    case RadarMode::Continuous: return "continuous";
    case RadarMode::Events: return "events";
    case RadarMode::Combined: return "combined";
    }
    return "continuous";
}

inline const char* ToString(RadarPreset preset) {
    switch (preset) {
    case RadarPreset::All: return "all";
    case RadarPreset::Footsteps: return "footsteps";
    case RadarPreset::Gunshots: return "gunshots";
    case RadarPreset::Custom: return "custom";
    }
    return "all";
}

inline const char* ToString(RadarRuntimeStatus status) {
    switch (status) {
    case RadarRuntimeStatus::WaitingForAudio: return "waiting-for-audio";
    case RadarRuntimeStatus::Active: return "active";
    case RadarRuntimeStatus::Silent: return "silent";
    case RadarRuntimeStatus::UnsupportedLayout: return "unsupported-layout";
    case RadarRuntimeStatus::UnsupportedSampleRate: return "unsupported-sample-rate";
    case RadarRuntimeStatus::MalformedInput: return "malformed-input";
    }
    return "malformed-input";
}

/// User-adjustable radar tuning. Curve values and the built-in preset curves
/// are power gains in dB; presets are spectral emphasis, not classifiers.
struct RadarProcessorConfig {
    RadarMode mode{RadarMode::Continuous};
    RadarPreset preset{RadarPreset::All};
    float sensitivityDbfs{-48.0f};
    uint32_t attackMilliseconds{30};
    uint32_t releaseMilliseconds{250};
    uint32_t holdMilliseconds{300};
    std::array<float, kRadarCustomCurvePointCount> customCurveDb{};
};

using RadarConfig = RadarProcessorConfig;

struct RadarFrame {
    uint64_t timestampSample{0};
    double timestampSeconds{0.0};
    uint64_t streamGeneration{0};
    uint32_t sampleRate{48000};
    AudioChannelLayout layout{};
    RadarPreset preset{RadarPreset::All};
    std::array<float, kRadarSectorCount> sectorActivitiesDbfs = [] {
        std::array<float, kRadarSectorCount> values{};
        values.fill(kRadarSilenceDbfs);
        return values;
    }();
    float confidence{0.0f};
    uint32_t strongestSector{0};
    float strongestAzimuthDegrees{0.0f};
    RadarRuntimeStatus status{RadarRuntimeStatus::WaitingForAudio};
};

struct RadarPeak {
    float azimuthDegrees{0.0f};
    float energyDbfs{kRadarSilenceDbfs};
    float confidence{0.0f};
    float angularUncertaintyDegrees{180.0f};
};

struct RadarEventResult {
    SoundEvent recognitionEvent{};
    uint32_t peakCount{0};
    std::array<RadarPeak, kRadarMaximumPeaks> peaks{};
    RadarPreset preset{RadarPreset::All};
    RadarRuntimeStatus status{RadarRuntimeStatus::WaitingForAudio};
};

} // namespace EchoRadar
