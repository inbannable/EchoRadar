#include "RingBuffer.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace EchoRadar {

RingBuffer::RingBuffer(std::size_t capacity)
    : m_capacity(capacity)
{
    if (capacity == 0) throw std::invalid_argument("RingBuffer capacity must be > 0");
    m_buffer.resize(capacity);
}

void RingBuffer::Push(AudioFrame frame) {
    std::lock_guard lock(m_mutex);
    std::size_t write_idx = (m_head + m_size) % m_capacity;
    m_buffer[write_idx]   = std::move(frame);
    if (m_size < m_capacity) {
        ++m_size;
    } else {
        // Overwrite oldest – advance head
        m_head = (m_head + 1) % m_capacity;
    }
}

bool RingBuffer::Pop(AudioFrame& out) {
    std::lock_guard lock(m_mutex);
    if (m_size == 0) return false;
    out    = std::move(m_buffer[m_head]);
    m_head = (m_head + 1) % m_capacity;
    --m_size;
    return true;
}

std::vector<AudioFrame> RingBuffer::GetLast(uint32_t ms) const {
    std::lock_guard lock(m_mutex);
    if (m_size == 0) return {};

    // Estimate frame duration from the first available frame
    const AudioFrame& first = m_buffer[m_head];
    uint32_t sr = first.sample_rate > 0 ? first.sample_rate : 48000;
    uint32_t samples_per_frame = static_cast<uint32_t>(first.left.size());
    if (samples_per_frame == 0) samples_per_frame = 480;
    double ms_per_frame = static_cast<double>(samples_per_frame) / sr * 1000.0;
    if (ms_per_frame <= 0) ms_per_frame = 10.0;

    auto n_frames = static_cast<std::size_t>(std::ceil(ms / ms_per_frame));
    n_frames = std::min(n_frames, m_size);

    std::vector<AudioFrame> result;
    result.reserve(n_frames);
    std::size_t start = (m_head + m_size - n_frames) % m_capacity;
    for (std::size_t i = 0; i < n_frames; ++i) {
        result.push_back(m_buffer[(start + i) % m_capacity]);
    }
    return result;
}

std::size_t RingBuffer::Size() const {
    std::lock_guard lock(m_mutex);
    return m_size;
}

bool RingBuffer::IsEmpty() const {
    std::lock_guard lock(m_mutex);
    return m_size == 0;
}

void RingBuffer::Clear() {
    std::lock_guard lock(m_mutex);
    m_head = 0;
    m_size = 0;
}

} // namespace EchoRadar
