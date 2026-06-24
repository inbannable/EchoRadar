#pragma once
#include <cstdint>
#include <string>

namespace EchoRadar {

/// Device type classification for audio routing decisions.
enum class AudioDeviceType {
    Unknown,           ///< Type cannot be determined
    Microphone,        ///< Physical microphone (ambient sound)
    Loopback,          ///< Stereo Mix / loopback recording (system audio output)
    VirtualCable,      ///< Virtual audio cable (e.g., VB-Audio Cable, VB-Meeter)
    LineIn,            ///< Line input / auxiliary input
};

/// Describes one audio capture device returned by AudioDeviceManager.
struct AudioDeviceInfo {
    std::string    id;                ///< Platform device ID string (WASAPI GUID on Windows)
    std::string    name;              ///< Human-readable device name
    AudioDeviceType type{AudioDeviceType::Unknown}; ///< Detected device type
    bool           isDefault{false};  ///< True if this is the system default input device
    uint32_t       nativeChannels{0};
    uint32_t       nativeSampleRate{0};
};

/// Convert device type to human-readable string.
inline const char* DeviceTypeString(AudioDeviceType type) {
    switch (type) {
        case AudioDeviceType::Microphone:    return "Microphone (ambient sound)";
        case AudioDeviceType::Loopback:      return "Stereo Mix (system audio)";
        case AudioDeviceType::VirtualCable:  return "Virtual Cable (e.g., VB-Cable, OBS)";
        case AudioDeviceType::LineIn:        return "Line Input";
        case AudioDeviceType::Unknown:
        default:                             return "Unknown device type";
    }
}

} // namespace EchoRadar
