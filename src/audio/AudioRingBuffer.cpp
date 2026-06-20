#include "AudioRingBuffer.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace EchoRadar {

AudioRingBuffer::AudioRingBuffer(size_t capacityFrames)
    // Minimum two slots so the mask is never zero.
    : m_capacityFrames(std::bit_ceil(std::max(capacityFrames, size_t{2})))
    , m_mask(m_capacityFrames - 1)
    , m_data(m_capacityFrames * kChannels, 0.f)
{
}

size_t AudioRingBuffer::PushInterleaved(const float* src, size_t frameCount) {
    // Relaxed load of write index — producer owns it.
    const size_t w     = m_writeIdx.load(std::memory_order_relaxed);
    // Acquire load of read index — synchronise with consumer's release store.
    const size_t r     = m_readIdx.load(std::memory_order_acquire);
    const size_t avail = m_capacityFrames - (w - r); // free slots
    const size_t n     = std::min(frameCount, avail);
    if (n == 0) return 0;

    const size_t slot  = w & m_mask;             // storage index of first frame
    const size_t contig = m_capacityFrames - slot; // contiguous slots before wrap

    const size_t first = std::min(n, contig);
    std::memcpy(m_data.data() + slot * kChannels,
                src,
                first * kChannels * sizeof(float));

    if (first < n) {
        std::memcpy(m_data.data(),
                    src + first * kChannels,
                    (n - first) * kChannels * sizeof(float));
    }

    // Release store — makes written data visible to consumer.
    m_writeIdx.store(w + n, std::memory_order_release);
    return n;
}

size_t AudioRingBuffer::PopInterleaved(float* dst, size_t frameCount) {
    // Relaxed load of read index — consumer owns it.
    const size_t r     = m_readIdx.load(std::memory_order_relaxed);
    // Acquire load of write index — synchronise with producer's release store.
    const size_t w     = m_writeIdx.load(std::memory_order_acquire);
    const size_t avail = w - r; // filled slots
    const size_t n     = std::min(frameCount, avail);
    if (n == 0) return 0;

    const size_t slot  = r & m_mask;
    const size_t contig = m_capacityFrames - slot;

    const size_t first = std::min(n, contig);
    std::memcpy(dst,
                m_data.data() + slot * kChannels,
                first * kChannels * sizeof(float));

    if (first < n) {
        std::memcpy(dst + first * kChannels,
                    m_data.data(),
                    (n - first) * kChannels * sizeof(float));
    }

    // Release store — makes consumed slots visible to producer.
    m_readIdx.store(r + n, std::memory_order_release);
    return n;
}

size_t AudioRingBuffer::GetAvailableFrames() const {
    const size_t r = m_readIdx.load(std::memory_order_relaxed);
    const size_t w = m_writeIdx.load(std::memory_order_acquire);
    return w - r;
}

void AudioRingBuffer::Clear() {
    // Advance read index to write index — discards all buffered frames.
    const size_t w = m_writeIdx.load(std::memory_order_acquire);
    m_readIdx.store(w, std::memory_order_release);
}

} // namespace EchoRadar
