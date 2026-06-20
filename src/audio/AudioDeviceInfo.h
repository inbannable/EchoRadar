#pragma once
#include <cstdint>
#include <string>

namespace EchoRadar {

/// Describes one audio capture device returned by AudioDeviceManager.
struct AudioDeviceInfo {
    std::string id;                ///< Platform device ID string (WASAPI GUID on Windows)
    std::string name;              ///< Human-readable device name
    bool        isDefault{false};  ///< True if this is the system default input device
    uint32_t    nativeChannels{0};
    uint32_t    nativeSampleRate{0};
};

} // namespace EchoRadar
