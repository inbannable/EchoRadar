#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace EchoRadar {

/// Borrowed 48 kHz stereo float32 PCM passed on the processing thread.
/// The span remains valid only for the duration of OnAudio().
struct AudioBlockView {
    std::span<const float> interleaved;
    size_t frameCount{0};
    uint32_t sampleRate{48000};
    uint32_t channels{2};
    uint64_t firstSample{0};
    uint64_t streamGeneration{0};
};

class IRealtimeAudioConsumer {
public:
    virtual ~IRealtimeAudioConsumer() = default;
    virtual void OnAudio(const AudioBlockView& block) = 0;
    virtual void OnStreamReset(uint64_t streamGeneration) = 0;
};

} // namespace EchoRadar
