#pragma once

#include "AudioTypes.h"

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

/// Borrowed native-order 48 kHz surround PCM passed on the processing thread.
/// The layout records the exact WAVEFORMATEXTENSIBLE speaker mask; no channel
/// position is inferred from channel count alone.
struct SurroundAudioBlockView {
    std::span<const float> interleaved;
    size_t frameCount{0};
    uint32_t sampleRate{48000};
    AudioChannelLayout layout{};
    uint64_t firstSample{0};
    uint64_t streamGeneration{0};
    bool discontinuity{false};
};

class IRealtimeAudioConsumer {
public:
    virtual ~IRealtimeAudioConsumer() = default;
    virtual void OnAudio(const AudioBlockView& block) = 0;
    virtual void OnStreamReset(uint64_t streamGeneration) = 0;
};

class IRealtimeSurroundAudioConsumer {
public:
    virtual ~IRealtimeSurroundAudioConsumer() = default;
    virtual void OnSurroundAudio(const SurroundAudioBlockView& block) = 0;
    virtual void OnStreamReset(uint64_t streamGeneration,
                               const AudioChannelLayout& layout) = 0;
};

} // namespace EchoRadar
