#include "AudioHistoryBuffer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace EchoRadar {

AudioHistoryBuffer::AudioHistoryBuffer(size_t capacityFrames, uint32_t sampleRate)
    : m_capacityFrames(std::max<size_t>(2, capacityFrames))
    , m_sampleRate(sampleRate)
    , m_data(m_capacityFrames * kChannels, 0.0f) {
    if (m_sampleRate == 0) {
        throw std::invalid_argument("AudioHistoryBuffer sampleRate must be > 0");
    }
}

void AudioHistoryBuffer::Reset() {
    m_writePos = 0;
    m_sizeFrames = 0;
    m_startSample = 0;
    m_hasTimeline = false;
}

size_t AudioHistoryBuffer::OldestSlot() const {
    return (m_writePos + m_capacityFrames - m_sizeFrames) % m_capacityFrames;
}

void AudioHistoryBuffer::PushInterleaved(const float* src, size_t frameCount, uint64_t startSample) {
    if (frameCount == 0) {
        return;
    }
    if (src == nullptr) {
        throw std::invalid_argument("AudioHistoryBuffer PushInterleaved src is null");
    }

    if (!m_hasTimeline || startSample != GetNewestSampleExclusive()) {
        Reset();
        m_startSample = startSample;
        m_hasTimeline = true;
    }

    if (frameCount >= m_capacityFrames) {
        const size_t keep = m_capacityFrames;
        const uint64_t keptStartSample = startSample + static_cast<uint64_t>(frameCount - keep);
        std::memcpy(m_data.data(),
                    src + ((frameCount - keep) * kChannels),
                    keep * kChannels * sizeof(float));
        m_writePos = 0;
        m_sizeFrames = keep;
        m_startSample = keptStartSample;
        return;
    }

    if (m_sizeFrames + frameCount > m_capacityFrames) {
        const size_t drop = (m_sizeFrames + frameCount) - m_capacityFrames;
        m_startSample += static_cast<uint64_t>(drop);
        m_sizeFrames -= drop;
    }

    const size_t first = std::min(frameCount, m_capacityFrames - m_writePos);
    std::memcpy(m_data.data() + (m_writePos * kChannels),
                src,
                first * kChannels * sizeof(float));

    if (first < frameCount) {
        std::memcpy(m_data.data(),
                    src + (first * kChannels),
                    (frameCount - first) * kChannels * sizeof(float));
    }

    m_writePos = (m_writePos + frameCount) % m_capacityFrames;
    m_sizeFrames = std::min(m_capacityFrames, m_sizeFrames + frameCount);
}

bool AudioHistoryBuffer::ExtractWindow(uint64_t startSample,
                                       size_t frameCount,
                                       std::vector<float>& outInterleaved) const {
    outInterleaved.clear();
    if (!m_hasTimeline || frameCount == 0) {
        return false;
    }

    const uint64_t oldest = GetOldestSample();
    const uint64_t newestExclusive = GetNewestSampleExclusive();
    const uint64_t endSampleExclusive = startSample + static_cast<uint64_t>(frameCount);
    if (startSample < oldest || endSampleExclusive > newestExclusive) {
        return false;
    }

    const uint64_t offsetFrames = startSample - oldest;
    const size_t startSlot = (OldestSlot() + static_cast<size_t>(offsetFrames)) % m_capacityFrames;
    outInterleaved.resize(frameCount * kChannels);

    const size_t first = std::min(frameCount, m_capacityFrames - startSlot);
    std::memcpy(outInterleaved.data(),
                m_data.data() + (startSlot * kChannels),
                first * kChannels * sizeof(float));
    if (first < frameCount) {
        std::memcpy(outInterleaved.data() + (first * kChannels),
                    m_data.data(),
                    (frameCount - first) * kChannels * sizeof(float));
    }
    return true;
}

bool AudioHistoryBuffer::ExtractWindowByTime(double centerTimeSec,
                                             double preSeconds,
                                             double postSeconds,
                                             std::vector<float>& outInterleaved,
                                             uint64_t& outStartSample) const {
    if (centerTimeSec < 0.0 || preSeconds < 0.0 || postSeconds < 0.0) {
        outInterleaved.clear();
        return false;
    }

    const uint64_t centerSample = static_cast<uint64_t>(std::llround(centerTimeSec * static_cast<double>(m_sampleRate)));
    const uint64_t preFrames = static_cast<uint64_t>(std::llround(preSeconds * static_cast<double>(m_sampleRate)));
    const uint64_t postFrames = static_cast<uint64_t>(std::llround(postSeconds * static_cast<double>(m_sampleRate)));
    if (centerSample < preFrames) {
        outInterleaved.clear();
        return false;
    }

    const uint64_t start = centerSample - preFrames;
    const size_t totalFrames = static_cast<size_t>(preFrames + postFrames);
    outStartSample = start;
    return ExtractWindow(start, totalFrames, outInterleaved);
}

uint64_t AudioHistoryBuffer::GetOldestSample() const {
    if (!m_hasTimeline) {
        return 0;
    }
    return m_startSample;
}

uint64_t AudioHistoryBuffer::GetNewestSampleExclusive() const {
    if (!m_hasTimeline) {
        return 0;
    }
    return m_startSample + static_cast<uint64_t>(m_sizeFrames);
}

} // namespace EchoRadar
