#pragma once

#include "RadarTypes.h"

#include <audio/AudioStreamConsumer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace EchoRadar {

/// Streaming 48 kHz multichannel energy-vector radar.
///
/// The processing path uses a 2,048-sample Hann STFT and a 480-sample hop.
/// Once constructed, PushAudio performs no dynamic allocation and takes no
/// locks. The event-window API is intended for the non-realtime event worker.
class RadarProcessor : public IRealtimeSurroundAudioConsumer {
public:
    static constexpr uint32_t kSampleRate = 48000;
    static constexpr uint32_t kFftSize = 2048;
    static constexpr uint32_t kHopSize = 480;
    static constexpr uint32_t kBinCount = kFftSize / 2 + 1;

    RadarProcessor();
    explicit RadarProcessor(const RadarProcessorConfig& config);
    ~RadarProcessor();

    RadarProcessor(const RadarProcessor&) = delete;
    RadarProcessor& operator=(const RadarProcessor&) = delete;
    RadarProcessor(RadarProcessor&&) noexcept;
    RadarProcessor& operator=(RadarProcessor&&) noexcept;

    void SetConfig(const RadarProcessorConfig& config);
    const RadarProcessorConfig& GetConfig() const;

    /// Resets the STFT, smoothing, timeline, and queued output frames.
    void Reset(uint64_t streamGeneration = 0);
    void OnStreamReset(uint64_t streamGeneration) { Reset(streamGeneration); }
    void OnStreamReset(uint64_t streamGeneration,
                       const AudioChannelLayout& layout) override;

    /// Accepts borrowed interleaved surround audio. Unsupported or malformed
    /// blocks produce a status frame and are never directionally approximated.
    void PushAudio(const SurroundAudioBlockView& block);
    void Process(const SurroundAudioBlockView& block) { PushAudio(block); }
    void OnAudio(const SurroundAudioBlockView& block) { PushAudio(block); }
    void OnSurroundAudio(const SurroundAudioBlockView& block) override {
        PushAudio(block);
    }

    size_t GetAvailableFrames() const;
    bool PopFrame(RadarFrame& output);
    RadarRuntimeStatus GetStatus() const;

    /// Analyzes a matching recognition window without modifying streaming
    /// state. The result reports no more than three azimuth-only peaks.
    RadarEventResult AnalyzeEventWindow(
        const SoundEvent& event,
        const SurroundAudioBlockView& window) const;

    /// Selects local maxima at least 30 degrees apart and no more than 18 dB
    /// below the strongest peak. Exposed to make event policy deterministic.
    static uint32_t SelectPeaks(
        const std::array<float, kRadarSectorCount>& activitiesDbfs,
        float frameConfidence,
        std::array<RadarPeak, kRadarMaximumPeaks>& output,
        float sensitivityDbfs = -48.0f);

    static float FrequencyWeightDb(
        RadarPreset preset,
        float frequencyHz,
        const std::array<float, kRadarCustomCurvePointCount>& customCurveDb = {});

    static RadarProcessorConfig SanitizeConfig(RadarProcessorConfig config);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace EchoRadar
