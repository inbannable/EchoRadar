#pragma once
#include "../common/Types.h"
#include <atomic>
#include <cstddef>
#include <mutex>
#include <vector>

namespace EchoRadar {

/// Lock-based, thread-safe ring buffer for AudioFrames.
/// Push from the audio thread; Pop / GetLast* from the processing thread.
class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity = 256);
    ~RingBuffer() = default;

    RingBuffer(const RingBuffer&)            = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    /// Push one frame. Drops the oldest frame when full.
    void Push(AudioFrame frame);

    /// Pop the oldest frame. Returns false when empty.
    bool Pop(AudioFrame& out);

    /// Return frames spanning approximately the last `ms` milliseconds.
    std::vector<AudioFrame> GetLast(uint32_t ms) const;
    std::vector<AudioFrame> GetLast100ms() const { return GetLast(100); }
    std::vector<AudioFrame> GetLast500ms() const { return GetLast(500); }

    std::size_t Size()     const;
    bool        IsEmpty()  const;
    void        Clear();

private:
    mutable std::mutex          m_mutex;
    std::vector<AudioFrame>     m_buffer;
    std::size_t                 m_capacity;
    std::size_t                 m_head{0};
    std::size_t                 m_size{0};
};

} // namespace EchoRadar
