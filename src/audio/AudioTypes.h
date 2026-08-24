#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace EchoRadar {

constexpr size_t kMaxAudioChannels = 8;

// Values from the Windows WAVEFORMATEXTENSIBLE speaker-position mask.  They are
// kept here instead of relying on ksmedia.h so layout validation is testable on
// every platform.
namespace WindowsSpeaker {
constexpr uint32_t FrontLeft   = 0x00000001u;
constexpr uint32_t FrontRight  = 0x00000002u;
constexpr uint32_t FrontCenter = 0x00000004u;
constexpr uint32_t Lfe         = 0x00000008u;
constexpr uint32_t BackLeft    = 0x00000010u;
constexpr uint32_t BackRight   = 0x00000020u;
constexpr uint32_t SideLeft    = 0x00000200u;
constexpr uint32_t SideRight   = 0x00000400u;

constexpr uint32_t Stereo          = FrontLeft | FrontRight;
constexpr uint32_t Surround51Back  = Stereo | FrontCenter | Lfe | BackLeft | BackRight;
constexpr uint32_t Surround51Side  = Stereo | FrontCenter | Lfe | SideLeft | SideRight;
constexpr uint32_t Surround71      = Surround51Back | SideLeft | SideRight;
} // namespace WindowsSpeaker

enum class AudioChannelRole : uint8_t {
    Unknown,
    FrontLeft,
    FrontRight,
    FrontCenter,
    Lfe,
    LowFrequency = Lfe,
    BackLeft,
    BackRight,
    SideLeft,
    SideRight,
};

enum class AudioChannelLayoutKind : uint8_t {
    Unsupported,
    Stereo,
    Surround51Back,
    Surround51Side,
    Surround71,
    FivePointOneBack = Surround51Back,
    FivePointOneSide = Surround51Side,
    SevenPointOne = Surround71,
};

/// A WAVEFORMATEXTENSIBLE layout in Windows speaker-mask channel order.
/// Only the first channelCount entries in roles are populated.
struct AudioChannelLayout {
    uint32_t channelCount{0};
    uint32_t channelMask{0};
    std::array<AudioChannelRole, kMaxAudioChannels> roles{};
    AudioChannelLayoutKind kind{AudioChannelLayoutKind::Unsupported};

    bool IsValid() const noexcept;
    bool IsDirectional() const noexcept;

    bool operator==(const AudioChannelLayout&) const = default;
};

/// Validate exactly the layouts EchoRadar can consume without guessing channel
/// roles.  Five-point-one accepts either the Windows back or side convention.
bool IsValidWindowsSpeakerMask(uint32_t channelCount, uint32_t channelMask) noexcept;
std::optional<AudioChannelLayout>
MakeAudioChannelLayout(uint32_t channelCount, uint32_t channelMask) noexcept;

// More explicit aliases for callers that discover layouts from WASAPI/WAV.
inline std::optional<AudioChannelLayout>
AudioChannelLayoutFromWindowsSpeakerMask(uint32_t channelCount,
                                         uint32_t channelMask) noexcept {
    return MakeAudioChannelLayout(channelCount, channelMask);
}

bool IsDirectionalAudioChannelRole(AudioChannelRole role) noexcept;
std::optional<float>
AudioChannelAzimuthDegrees(const AudioChannelLayout& layout, size_t channelIndex) noexcept;
const char* ToString(AudioChannelRole role) noexcept;
const char* ToString(AudioChannelLayoutKind kind) noexcept;

constexpr float kRecognitionCenterGain = 0.7071067811865475f;
constexpr float kRecognitionSurroundGain = 0.5f;

/// Downmix discrete surround frames to the existing recognition stereo format.
/// Front center is sent equally to both sides, surround channels stay on their
/// corresponding side at a lower gain, and LFE is deliberately omitted.
bool DownmixForRecognition(std::span<const float> surroundInterleaved,
                           size_t frameCount,
                           const AudioChannelLayout& layout,
                           std::span<float> stereoInterleaved) noexcept;
std::vector<float> DownmixForRecognition(std::span<const float> surroundInterleaved,
                                         size_t frameCount,
                                         const AudioChannelLayout& layout);

struct AudioLevels {
    float leftRms{0.0f};
    float rightRms{0.0f};
    float leftPeak{0.0f};
    float rightPeak{0.0f};
    uint32_t channelCount{2};
    std::array<float, kMaxAudioChannels> rms{};
    std::array<float, kMaxAudioChannels> peak{};
};

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

struct AudioProfile {
    std::string name{"Default"};
    HeadphoneEqProfile eqProfile{HeadphoneEqProfile::Natural};
    float leftRightIsolationPercent{0.0f};
    bool perspectiveCorrection{true};
    float displayAspectRatio{16.0f / 9.0f};
    SpatialEnhancementState spatialEnhancement{SpatialEnhancementState::Unknown};
    std::string outputEndpointId;
};

} // namespace EchoRadar
