#pragma once
#include <cstdint>
#include <string>

namespace EchoRadar {

enum class AudioDeviceFlow {
    Input,
    Output,
};

/// Device type classification for audio routing decisions.
enum class AudioDeviceType {
    Unknown,           ///< Type cannot be determined
    Microphone,        ///< Physical microphone (ambient sound)
    Loopback,          ///< Stereo Mix / loopback recording (system audio output)
    VirtualCable,      ///< Virtual audio cable (e.g., VB-Audio Cable, VB-Meeter)
    LineIn,            ///< Line input / auxiliary input
};

/// Describes one input or render endpoint returned by AudioDeviceManager.
struct AudioDeviceInfo {
    std::string    id;                ///< Stable opaque endpoint fingerprint for this backend
    std::string    name;              ///< Human-readable device name
    AudioDeviceFlow flow{AudioDeviceFlow::Input};
    AudioDeviceType type{AudioDeviceType::Unknown}; ///< Detected device type
    bool           isDefault{false};  ///< True if this is the default endpoint for its flow
    uint32_t       nativeChannels{0};
    uint32_t       nativeSampleRate{0};
};

/// Convert device type to human-readable string.
inline const char* DeviceTypeString(AudioDeviceType type) {
    switch (type) {
        case AudioDeviceType::Microphone:    return "Microphone (ambient sound)";
        case AudioDeviceType::Loopback:      return "System audio loopback";
        case AudioDeviceType::VirtualCable:  return "Virtual Cable (e.g., VB-Cable, OBS)";
        case AudioDeviceType::LineIn:        return "Line Input";
        case AudioDeviceType::Unknown:
        default:                             return "Unknown device type";
    }
}

} // namespace EchoRadar
