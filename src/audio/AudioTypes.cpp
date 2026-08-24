#include "AudioTypes.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace EchoRadar {
namespace {

constexpr std::array<std::pair<uint32_t, AudioChannelRole>, 8> kWindowsRoles{{
    {WindowsSpeaker::FrontLeft, AudioChannelRole::FrontLeft},
    {WindowsSpeaker::FrontRight, AudioChannelRole::FrontRight},
    {WindowsSpeaker::FrontCenter, AudioChannelRole::FrontCenter},
    {WindowsSpeaker::Lfe, AudioChannelRole::Lfe},
    {WindowsSpeaker::BackLeft, AudioChannelRole::BackLeft},
    {WindowsSpeaker::BackRight, AudioChannelRole::BackRight},
    {WindowsSpeaker::SideLeft, AudioChannelRole::SideLeft},
    {WindowsSpeaker::SideRight, AudioChannelRole::SideRight},
}};

bool IsFivePointOne(const AudioChannelLayout& layout) noexcept {
    return layout.kind == AudioChannelLayoutKind::Surround51Back ||
           layout.kind == AudioChannelLayoutKind::Surround51Side;
}

} // namespace

bool AudioChannelLayout::IsValid() const noexcept {
    const auto canonical = MakeAudioChannelLayout(channelCount, channelMask);
    return canonical.has_value() && canonical->kind == kind &&
           canonical->roles == roles;
}

bool AudioChannelLayout::IsDirectional() const noexcept {
    return IsValid() &&
        (kind == AudioChannelLayoutKind::Surround51Back ||
         kind == AudioChannelLayoutKind::Surround51Side ||
         kind == AudioChannelLayoutKind::Surround71);
}

bool IsValidWindowsSpeakerMask(uint32_t channelCount, uint32_t channelMask) noexcept {
    if (std::popcount(channelMask) != channelCount) return false;
    switch (channelCount) {
    case 2:
        return channelMask == WindowsSpeaker::Stereo;
    case 6:
        return channelMask == WindowsSpeaker::Surround51Back ||
               channelMask == WindowsSpeaker::Surround51Side;
    case 8:
        return channelMask == WindowsSpeaker::Surround71;
    default:
        return false;
    }
}

std::optional<AudioChannelLayout>
MakeAudioChannelLayout(uint32_t channelCount, uint32_t channelMask) noexcept {
    if (!IsValidWindowsSpeakerMask(channelCount, channelMask)) return std::nullopt;

    AudioChannelLayout layout;
    layout.channelCount = channelCount;
    layout.channelMask = channelMask;
    if (channelMask == WindowsSpeaker::Stereo) {
        layout.kind = AudioChannelLayoutKind::Stereo;
    } else if (channelMask == WindowsSpeaker::Surround51Back) {
        layout.kind = AudioChannelLayoutKind::Surround51Back;
    } else if (channelMask == WindowsSpeaker::Surround51Side) {
        layout.kind = AudioChannelLayoutKind::Surround51Side;
    } else {
        layout.kind = AudioChannelLayoutKind::Surround71;
    }

    size_t roleIndex = 0;
    for (const auto& [bit, role] : kWindowsRoles) {
        if ((channelMask & bit) != 0u) layout.roles[roleIndex++] = role;
    }
    return layout;
}

bool IsDirectionalAudioChannelRole(AudioChannelRole role) noexcept {
    return role != AudioChannelRole::Unknown && role != AudioChannelRole::Lfe;
}

std::optional<float>
AudioChannelAzimuthDegrees(const AudioChannelLayout& layout, size_t channelIndex) noexcept {
    if (!layout.IsDirectional() || channelIndex >= layout.channelCount ||
        channelIndex >= layout.roles.size()) {
        return std::nullopt;
    }

    switch (layout.roles[channelIndex]) {
    case AudioChannelRole::FrontLeft:   return 330.0f;
    case AudioChannelRole::FrontRight:  return 30.0f;
    case AudioChannelRole::FrontCenter: return 0.0f;
    case AudioChannelRole::Lfe:
    case AudioChannelRole::Unknown:     return std::nullopt;
    case AudioChannelRole::SideLeft:
        return IsFivePointOne(layout) ? 250.0f : 270.0f;
    case AudioChannelRole::SideRight:
        return IsFivePointOne(layout) ? 110.0f : 90.0f;
    case AudioChannelRole::BackLeft:
        return IsFivePointOne(layout) ? 250.0f : 210.0f;
    case AudioChannelRole::BackRight:
        return IsFivePointOne(layout) ? 110.0f : 150.0f;
    }
    return std::nullopt;
}

const char* ToString(AudioChannelRole role) noexcept {
    switch (role) {
    case AudioChannelRole::FrontLeft: return "front-left";
    case AudioChannelRole::FrontRight: return "front-right";
    case AudioChannelRole::FrontCenter: return "front-center";
    case AudioChannelRole::Lfe: return "lfe";
    case AudioChannelRole::BackLeft: return "back-left";
    case AudioChannelRole::BackRight: return "back-right";
    case AudioChannelRole::SideLeft: return "side-left";
    case AudioChannelRole::SideRight: return "side-right";
    case AudioChannelRole::Unknown: return "unknown";
    }
    return "unknown";
}

const char* ToString(AudioChannelLayoutKind kind) noexcept {
    switch (kind) {
    case AudioChannelLayoutKind::Stereo: return "stereo";
    case AudioChannelLayoutKind::Surround51Back: return "5.1-back";
    case AudioChannelLayoutKind::Surround51Side: return "5.1-side";
    case AudioChannelLayoutKind::Surround71: return "7.1";
    case AudioChannelLayoutKind::Unsupported: return "unsupported";
    }
    return "unsupported";
}

bool DownmixForRecognition(std::span<const float> surroundInterleaved,
                           size_t frameCount,
                           const AudioChannelLayout& layout,
                           std::span<float> stereoInterleaved) noexcept {
    if (!layout.IsValid() || layout.channelCount == 0 ||
        frameCount > surroundInterleaved.size() / layout.channelCount ||
        frameCount > stereoInterleaved.size() / 2) {
        return false;
    }

    for (size_t frame = 0; frame < frameCount; ++frame) {
        float left = 0.0f;
        float right = 0.0f;
        const size_t sourceOffset = frame * layout.channelCount;
        for (size_t channel = 0; channel < layout.channelCount; ++channel) {
            const float sample = surroundInterleaved[sourceOffset + channel];
            switch (layout.roles[channel]) {
            case AudioChannelRole::FrontLeft:
                left += sample;
                break;
            case AudioChannelRole::FrontRight:
                right += sample;
                break;
            case AudioChannelRole::FrontCenter:
                left += sample * kRecognitionCenterGain;
                right += sample * kRecognitionCenterGain;
                break;
            case AudioChannelRole::BackLeft:
            case AudioChannelRole::SideLeft:
                left += sample * kRecognitionSurroundGain;
                break;
            case AudioChannelRole::BackRight:
            case AudioChannelRole::SideRight:
                right += sample * kRecognitionSurroundGain;
                break;
            case AudioChannelRole::Lfe:
            case AudioChannelRole::Unknown:
                break;
            }
        }
        // Preserve the existing stereo recognizer's normalized float contract
        // when coherent front/center/surround content sums above full scale.
        stereoInterleaved[frame * 2] = std::clamp(left, -1.0f, 1.0f);
        stereoInterleaved[frame * 2 + 1] = std::clamp(right, -1.0f, 1.0f);
    }
    return true;
}

std::vector<float> DownmixForRecognition(std::span<const float> surroundInterleaved,
                                         size_t frameCount,
                                         const AudioChannelLayout& layout) {
    std::vector<float> stereo(frameCount * 2, 0.0f);
    if (!DownmixForRecognition(surroundInterleaved, frameCount, layout, stereo)) {
        throw std::invalid_argument("Invalid surround audio block for recognition downmix");
    }
    return stereo;
}

} // namespace EchoRadar
