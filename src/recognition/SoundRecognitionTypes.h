#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace EchoRadar {

enum class SoundClass : uint8_t {
    Gunshot = 0,
    Footstep = 1,
    Mechanical = 2,
};

enum class RecognitionMode : uint8_t {
    Ambient = 0,
    TargetActive = 1,
};

inline constexpr size_t kSoundClassCount = 3;
inline constexpr std::array<SoundClass, kSoundClassCount> kSoundClasses = {
    SoundClass::Gunshot,
    SoundClass::Footstep,
    SoundClass::Mechanical,
};

inline const char* ToString(SoundClass soundClass) {
    switch (soundClass) {
    case SoundClass::Gunshot: return "gunshot";
    case SoundClass::Footstep: return "footstep";
    case SoundClass::Mechanical: return "mechanical";
    }
    return "unknown";
}

inline std::optional<SoundClass> SoundClassFromString(const std::string& value) {
    if (value == "gunshot") return SoundClass::Gunshot;
    if (value == "footstep") return SoundClass::Footstep;
    if (value == "mechanical") return SoundClass::Mechanical;
    return std::nullopt;
}

inline size_t SoundClassIndex(SoundClass soundClass) {
    return static_cast<size_t>(soundClass);
}

struct SoundEvent {
    SoundClass soundClass{SoundClass::Gunshot};
    uint64_t onsetSample{0};
    uint64_t endSample{0};
    float confidence{0.0f};
    std::string modelVersion;
    uint64_t detectedSample{0};
};

struct SoundProbabilities {
    uint64_t sample{0};
    std::array<float, kSoundClassCount> values{};
};

struct SoundRecognitionState {
    uint64_t sample{0};
    RecognitionMode mode{RecognitionMode::Ambient};
    std::array<bool, kSoundClassCount> activeClasses{};

    bool IsAmbient() const { return mode == RecognitionMode::Ambient; }
};

} // namespace EchoRadar
