#pragma once

#include <recognition/RecognitionTypes.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace EchoRadar {

enum class DirectionStatus : uint8_t {
    Estimated,
    LowConfidence,
    Disabled,
    AudioUnavailable,
    ModelUnavailable,
    InferenceFailed,
};

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

struct DirectionSettings {
    bool enableGunshots{true};
    bool enableFootsteps{true};

    bool IsEnabled(SoundClass soundClass) const {
        return soundClass == SoundClass::Gunshot ? enableGunshots : enableFootsteps;
    }
};

struct DirectionSourceEstimate {
    float azimuthDegrees{0.0f};
    float elevationDegrees{0.0f};
    float confidence{0.0f};
    float uncertaintyDegrees{180.0f};
};

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
};

} // namespace EchoRadar
