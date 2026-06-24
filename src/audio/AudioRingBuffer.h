#pragma once
#include <atomic>
#include <cstddef>
#include <vector>

namespace EchoRadar {

/// Lock-free single-producer / single-consumer ring buffer for stereo float32 PCM.
///
/// Memory layout per slot: [ L, R ]  (interleaved)
/// One "frame" = one stereo sample pair = 2 floats = 8 bytes.
/// Capacity is rounded up to the next power-of-two for fast wrap-around via bitmask.
///
/// Thread-safety contract:
///   PushInterleaved          — ONE producer thread (audio callback).
///   PopInterleaved, Clear,
///   GetAvailableFrames       — ONE consumer thread.
///
/// Overflow policy: new frames are silently dropped when full (callback-safe; never blocks).
class AudioRingBuffer {
public:
    static constexpr size_t kChannels = 2; ///< Stereo (L, R)

    /// @param capacityFrames  Minimum number of frames; rounded up to next power-of-two.
    explicit AudioRingBuffer(size_t capacityFrames);

    AudioRingBuffer(const AudioRingBuffer&)            = delete;
    AudioRingBuffer& operator=(const AudioRingBuffer&) = delete;

    /// Push up to @p frameCount interleaved stereo frames from @p src.
    /// Silently drops new frames when full (never blocks — safe from audio callback).
    /// @return Frames actually written (≤ frameCount).
    size_t PushInterleaved(const float* src, size_t frameCount);

    /// Pop up to @p frameCount interleaved stereo frames into @p dst.
    /// @return Frames actually read (≤ frameCount; 0 when empty).
    size_t PopInterleaved(float* dst, size_t frameCount);

    /// Frames available to read right now.
    size_t GetAvailableFrames() const;

    /// Discard all buffered frames.  Call from consumer thread only.
    void Clear();

    size_t CapacityFrames() const { return m_capacityFrames; }

private:
    size_t                          m_capacityFrames; ///< always a power-of-two
    size_t                          m_mask;           ///< m_capacityFrames - 1
    std::vector<float>              m_data;           ///< m_capacityFrames * kChannels floats

    // Cache-line separated to avoid false sharing between producer and consumer.
    alignas(64) std::atomic<size_t> m_writeIdx{0};   ///< owned by producer
    alignas(64) std::atomic<size_t> m_readIdx{0};    ///< owned by consumer
};

} // namespace EchoRadar
