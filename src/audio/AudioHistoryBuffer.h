#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace EchoRadar {

/// Circular PCM cache for recent stereo interleaved float frames.
/// Designed for single-thread push/extract in the DSP pipeline.
class AudioHistoryBuffer {
public:
    static constexpr size_t kChannels = 2;

    explicit AudioHistoryBuffer(size_t capacityFrames = 48000 * 3, uint32_t sampleRate = 48000);

    void Reset();

    /// Push sequential interleaved stereo frames.
    /// @param startSample Absolute sample index of src[0] (left/right frame index).
    void PushInterleaved(const float* src, size_t frameCount, uint64_t startSample);

    bool ExtractWindow(uint64_t startSample, size_t frameCount, std::vector<float>& outInterleaved) const;
    bool ExtractWindowByTime(double centerTimeSec,
                             double preSeconds,
                             double postSeconds,
                             std::vector<float>& outInterleaved,
                             uint64_t& outStartSample) const;

    uint64_t GetOldestSample() const;
    uint64_t GetNewestSampleExclusive() const;
    size_t   GetStoredFrames() const { return m_sizeFrames; }
    size_t   CapacityFrames() const { return m_capacityFrames; }
    uint32_t GetSampleRate() const { return m_sampleRate; }

private:
    size_t m_capacityFrames{0};
    uint32_t m_sampleRate{48000};
    std::vector<float> m_data;
    size_t m_writePos{0};
    size_t m_sizeFrames{0};
    uint64_t m_startSample{0};
    bool m_hasTimeline{false};

    size_t OldestSlot() const;
};

} // namespace EchoRadar
