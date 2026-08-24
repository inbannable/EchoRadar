#pragma once

#include "AudioTypes.h"

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
    uint32_t nativeChannelMask{0};
    AudioChannelLayout layout{};

    bool SupportsDirectionalRadar() const noexcept {
        return nativeSampleRate == 48000 && layout.IsDirectional();
    }
};

} // namespace EchoRadar
