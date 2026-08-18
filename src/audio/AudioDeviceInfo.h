#pragma once

#include <cstdint>
#include <string>

namespace EchoRadar {

/// One Windows render endpoint available for WASAPI loopback capture.
struct AudioDeviceInfo {
    std::string id;
    std::string name;
    bool isDefault{false};
    uint32_t nativeChannels{0};
    uint32_t nativeSampleRate{0};
};

} // namespace EchoRadar
