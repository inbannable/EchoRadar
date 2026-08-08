#pragma once

#include "SoundRecognitionTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace EchoRadar {

inline constexpr size_t kV4SoundClassCount = 2;
inline constexpr size_t kV4SourceCount = 3;

enum class SoundSourceHint : uint8_t {
    Self = 0,
    Remote = 1,
    Unknown = 2,
};

enum class SceneState : uint8_t {
    Quiet,
    Busy,
};

inline const char* ToString(SoundSourceHint source) {
    switch (source) {
    case SoundSourceHint::Self: return "self";
    case SoundSourceHint::Remote: return "remote";
    case SoundSourceHint::Unknown: return "unknown";
    }
    return "unknown";
}

inline const char* ToString(SceneState scene) {
    return scene == SceneState::Quiet ? "quiet" : "busy";
}

struct V4ModelOutput {
    std::array<float, kV4SoundClassCount> onsetProbabilities{};
    std::array<std::array<float, kV4SourceCount>, kV4SoundClassCount>
        sourceProbabilities{};
};

struct V4SoundEvent {
    SoundClass soundClass{SoundClass::Gunshot};
    uint64_t onsetSample{0};
    uint64_t endSample{0};
    uint64_t detectedSample{0};
    uint64_t deliveredSample{0};
    float confidence{0.0f};
    SoundSourceHint sourceHint{SoundSourceHint::Unknown};
    float sourceConfidence{0.0f};
    SceneState sceneState{SceneState::Quiet};
    bool suppressed{false};
    uint64_t streamGeneration{0};
    std::string modelVersion;
};

} // namespace EchoRadar
