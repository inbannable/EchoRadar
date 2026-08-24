#pragma once

#include <cstdint>

namespace EchoRadar {

struct WasapiEndpointFormat {
    uint32_t sampleRate{0};
    uint32_t channelCount{0};
    uint32_t channelMask{0};
};

// Reads the shared-mode mix format directly from the Windows endpoint. For
// multichannel formats, channelMask is nonzero only when Windows supplied an
// explicit WAVEFORMATEXTENSIBLE speaker mask; callers must not synthesize one.
bool QueryWasapiEndpointFormat(const wchar_t* endpointId,
                               WasapiEndpointFormat& format) noexcept;

} // namespace EchoRadar
