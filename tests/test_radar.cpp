#include <radar/RadarProcessor.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace EchoRadar {
namespace {

constexpr float kPi = 3.14159265358979323846f;

AudioChannelLayout Layout51() {
    return MakeAudioChannelLayout(6, WindowsSpeaker::Surround51Back).value();
}

AudioChannelLayout Layout71() {
    return MakeAudioChannelLayout(8, WindowsSpeaker::Surround71).value();
}

size_t ChannelForRole(const AudioChannelLayout& layout, AudioChannelRole role) {
    for (size_t channel = 0; channel < layout.channelCount; ++channel) {
        if (layout.roles[channel] == role) return channel;
    }
    return layout.channelCount;
}

struct Tone {
    AudioChannelRole role;
    float frequencyHz;
    float amplitude;
    float phase{0.0f};
};

std::vector<float> MakeAudio(const AudioChannelLayout& layout,
                             size_t frameCount,
                             std::span<const Tone> tones) {
    std::vector<float> audio(frameCount * layout.channelCount, 0.0f);
    for (const Tone& tone : tones) {
        const size_t channel = ChannelForRole(layout, tone.role);
        if (channel >= layout.channelCount) continue;
        for (size_t frame = 0; frame < frameCount; ++frame) {
            const float angle = 2.0f * kPi * tone.frequencyHz *
                                static_cast<float>(frame) /
                                static_cast<float>(RadarProcessor::kSampleRate) +
                                tone.phase;
            audio[frame * layout.channelCount + channel] +=
                tone.amplitude * std::sin(angle);
        }
    }
    return audio;
}

SurroundAudioBlockView View(const std::vector<float>& audio,
                            const AudioChannelLayout& layout,
                            uint64_t firstSample = 0,
                            uint64_t generation = 1,
                            bool discontinuity = false) {
    return {
        audio,
        audio.size() / layout.channelCount,
        RadarProcessor::kSampleRate,
        layout,
        firstSample,
        generation,
        discontinuity,
    };
}

RadarProcessorConfig ImmediateConfig() {
    RadarProcessorConfig config;
    config.sensitivityDbfs = -96.0f;
    config.attackMilliseconds = 0;
    config.releaseMilliseconds = 0;
    config.holdMilliseconds = 0;
    return config;
}

RadarFrame LastFrame(RadarProcessor& processor) {
    RadarFrame frame;
    RadarFrame latest;
    bool found = false;
    while (processor.PopFrame(frame)) {
        latest = frame;
        found = true;
    }
    EXPECT_TRUE(found);
    return latest;
}

float CircularDistance(float left, float right) {
    const float difference = std::fabs(left - right);
    return std::min(difference, 360.0f - difference);
}

TEST(RadarProcessorTest, ResolvesCardinalAndIntermediateBearings) {
    const AudioChannelLayout layout = Layout71();
    RadarProcessor processor(ImmediateConfig());

    auto front = MakeAudio(layout, RadarProcessor::kFftSize,
                           std::array{Tone{AudioChannelRole::FrontCenter, 1000.0f, 0.25f}});
    processor.PushAudio(View(front, layout));
    RadarFrame frame = LastFrame(processor);
    EXPECT_EQ(frame.status, RadarRuntimeStatus::Active);
    EXPECT_EQ(frame.strongestSector, 0u);
    EXPECT_GT(frame.confidence, 0.99f);

    auto right = MakeAudio(layout, RadarProcessor::kFftSize,
                           std::array{Tone{AudioChannelRole::SideRight, 1000.0f, 0.25f}});
    processor.PushAudio(View(right, layout, RadarProcessor::kFftSize, 2, true));
    frame = LastFrame(processor);
    EXPECT_EQ(frame.strongestSector, 6u);
    EXPECT_NEAR(frame.strongestAzimuthDegrees, 90.0f, 0.01f);

    auto left = MakeAudio(layout, RadarProcessor::kFftSize,
                          std::array{Tone{AudioChannelRole::SideLeft, 1000.0f, 0.25f}});
    processor.PushAudio(View(left, layout, 2 * RadarProcessor::kFftSize, 3, true));
    frame = LastFrame(processor);
    EXPECT_EQ(frame.strongestSector, 18u);
    EXPECT_NEAR(frame.strongestAzimuthDegrees, 270.0f, 0.01f);

    auto rear = MakeAudio(layout, RadarProcessor::kFftSize,
        std::array{
            Tone{AudioChannelRole::BackLeft, 1000.0f, 0.25f},
            Tone{AudioChannelRole::BackRight, 1000.0f, 0.25f},
        });
    processor.PushAudio(View(rear, layout, 3 * RadarProcessor::kFftSize, 4, true));
    frame = LastFrame(processor);
    EXPECT_EQ(frame.strongestSector, 12u);
    EXPECT_NEAR(frame.strongestAzimuthDegrees, 180.0f, 0.01f);

    auto intermediate = MakeAudio(layout, RadarProcessor::kFftSize,
        std::array{
            Tone{AudioChannelRole::FrontCenter, 1000.0f, 0.25f},
            Tone{AudioChannelRole::FrontRight, 1000.0f, 0.25f},
        });
    processor.PushAudio(View(intermediate, layout, 4 * RadarProcessor::kFftSize, 5, true));
    frame = LastFrame(processor);
    EXPECT_EQ(frame.strongestSector, 1u);
    EXPECT_NEAR(frame.strongestAzimuthDegrees, 15.0f, 0.01f);
    EXPECT_GT(frame.confidence, 0.95f);
}

TEST(RadarProcessorTest, FollowsMirroredRoleOrderAndExcludesLfe) {
    AudioChannelLayout layout = Layout51();
    const size_t frontLeft = ChannelForRole(layout, AudioChannelRole::FrontLeft);
    const size_t frontRight = ChannelForRole(layout, AudioChannelRole::FrontRight);
    std::swap(layout.roles[frontLeft], layout.roles[frontRight]);

    std::vector<float> audio(RadarProcessor::kFftSize * layout.channelCount, 0.0f);
    const size_t lfe = ChannelForRole(layout, AudioChannelRole::Lfe);
    for (size_t sample = 0; sample < RadarProcessor::kFftSize; ++sample) {
        const float value = 0.2f * std::sin(2.0f * kPi * 1000.0f *
            static_cast<float>(sample) / RadarProcessor::kSampleRate);
        audio[sample * layout.channelCount + frontLeft] = value;
        audio[sample * layout.channelCount + lfe] = value * 4.0f;
    }

    RadarProcessor processor(ImmediateConfig());
    processor.PushAudio(View(audio, layout));
    const RadarFrame frame = LastFrame(processor);
    EXPECT_EQ(frame.strongestSector, 2u);
    EXPECT_NEAR(frame.strongestAzimuthDegrees, 30.0f, 0.01f);
}

TEST(RadarProcessorTest, ReportsSilenceUnsupportedAndMalformedInput) {
    const AudioChannelLayout surround = Layout51();
    std::vector<float> silence(RadarProcessor::kFftSize * surround.channelCount, 0.0f);
    RadarProcessor processor(ImmediateConfig());
    processor.PushAudio(View(silence, surround));
    RadarFrame frame = LastFrame(processor);
    EXPECT_EQ(frame.status, RadarRuntimeStatus::Silent);
    EXPECT_TRUE(std::all_of(frame.sectorActivitiesDbfs.begin(),
                            frame.sectorActivitiesDbfs.end(),
                            [](float level) { return level == kRadarSilenceDbfs; }));

    const AudioChannelLayout stereo =
        MakeAudioChannelLayout(2, WindowsSpeaker::Stereo).value();
    std::vector<float> stereoAudio(64 * 2, 0.0f);
    processor.PushAudio(View(stereoAudio, stereo, 100, 2, true));
    frame = LastFrame(processor);
    EXPECT_EQ(frame.status, RadarRuntimeStatus::UnsupportedLayout);

    AudioChannelLayout malformed = surround;
    malformed.roles[1] = malformed.roles[0];
    std::vector<float> malformedAudio(64 * malformed.channelCount, 0.0f);
    processor.PushAudio(View(malformedAudio, malformed, 200, 3, true));
    frame = LastFrame(processor);
    EXPECT_EQ(frame.status, RadarRuntimeStatus::MalformedInput);

    // Structural malformation takes precedence over an unsupported rate so
    // callers do not mistake an unsafe/short block for a format-only issue.
    SurroundAudioBlockView shortBlock = View(malformedAudio, surround, 300, 4, true);
    shortBlock.frameCount += 1u;
    shortBlock.sampleRate = 44'100;
    processor.PushAudio(shortBlock);
    frame = LastFrame(processor);
    EXPECT_EQ(frame.status, RadarRuntimeStatus::MalformedInput);
}

TEST(RadarProcessorTest, InterpolatesBetweenAdjacentFifteenDegreeSectors) {
    const AudioChannelLayout layout = Layout71();
    const float theta = 7.5f * kPi / 180.0f;
    const float powerRatio = std::tan(theta) /
        (std::sin(30.0f * kPi / 180.0f) -
         std::tan(theta) * std::cos(30.0f * kPi / 180.0f));
    auto audio = MakeAudio(layout, RadarProcessor::kFftSize,
        std::array{
            Tone{AudioChannelRole::FrontCenter, 1125.0f, 0.25f},
            Tone{AudioChannelRole::FrontRight, 1125.0f,
                 0.25f * std::sqrt(powerRatio)},
        });

    RadarProcessor processor(ImmediateConfig());
    processor.PushAudio(View(audio, layout));
    const RadarFrame frame = LastFrame(processor);
    EXPECT_NEAR(frame.sectorActivitiesDbfs[0],
                frame.sectorActivitiesDbfs[1], 0.15f);
    EXPECT_GT(frame.sectorActivitiesDbfs[0], frame.sectorActivitiesDbfs[23] + 20.0f);
    EXPECT_GT(frame.sectorActivitiesDbfs[1], frame.sectorActivitiesDbfs[2] + 20.0f);
}

TEST(RadarProcessorTest, PresetsAndCustomCurveAreSpectralEmphasis) {
    EXPECT_GT(RadarProcessor::FrequencyWeightDb(RadarPreset::Footsteps, 250.0f),
              RadarProcessor::FrequencyWeightDb(RadarPreset::Footsteps, 6000.0f));
    EXPECT_GT(RadarProcessor::FrequencyWeightDb(RadarPreset::Gunshots, 3000.0f),
              RadarProcessor::FrequencyWeightDb(RadarPreset::Gunshots, 250.0f));
    EXPECT_FLOAT_EQ(RadarProcessor::FrequencyWeightDb(RadarPreset::All, 400.0f), 0.0f);

    std::array<float, kRadarCustomCurvePointCount> custom{};
    custom[4] = -10.0f;
    custom[5] = 10.0f;
    EXPECT_FLOAT_EQ(RadarProcessor::FrequencyWeightDb(
        RadarPreset::Custom, 250.0f, custom), -10.0f);
    EXPECT_GT(RadarProcessor::FrequencyWeightDb(
        RadarPreset::Custom, 350.0f, custom), -10.0f);
    EXPECT_LT(RadarProcessor::FrequencyWeightDb(
        RadarPreset::Custom, 350.0f, custom), 10.0f);
}

TEST(RadarProcessorTest, AppliesAttackReleaseAndBoundedHold) {
    const AudioChannelLayout layout = Layout51();
    RadarProcessorConfig config = ImmediateConfig();
    config.attackMilliseconds = 100;
    config.releaseMilliseconds = 250;
    config.holdMilliseconds = 0;
    RadarProcessor processor(config);
    const size_t frameCount = RadarProcessor::kFftSize + 4 * RadarProcessor::kHopSize;
    auto signal = MakeAudio(layout, frameCount,
                            std::array{Tone{AudioChannelRole::FrontCenter, 1000.0f, 0.2f}});
    processor.PushAudio(View(signal, layout));
    std::vector<RadarFrame> frames;
    RadarFrame frame;
    while (processor.PopFrame(frame)) frames.push_back(frame);
    ASSERT_GE(frames.size(), 4u);
    EXPECT_LT(frames.front().sectorActivitiesDbfs[0],
              frames.back().sectorActivitiesDbfs[0]);

    RadarProcessorConfig noHoldConfig = ImmediateConfig();
    RadarProcessorConfig heldConfig = ImmediateConfig();
    heldConfig.holdMilliseconds = 20;
    RadarProcessor noHold(noHoldConfig);
    RadarProcessor held(heldConfig);
    noHold.PushAudio(View(signal, layout));
    held.PushAudio(View(signal, layout));
    while (noHold.PopFrame(frame)) {}
    while (held.PopFrame(frame)) {}

    std::vector<float> silence(8 * RadarProcessor::kHopSize * layout.channelCount, 0.0f);
    noHold.PushAudio(View(silence, layout, frameCount));
    held.PushAudio(View(silence, layout, frameCount));
    std::vector<RadarFrame> noHoldFrames;
    std::vector<RadarFrame> heldFrames;
    while (noHold.PopFrame(frame)) noHoldFrames.push_back(frame);
    while (held.PopFrame(frame)) heldFrames.push_back(frame);
    ASSERT_EQ(noHoldFrames.size(), heldFrames.size());
    const auto firstSilent = [](const std::vector<RadarFrame>& radarFrames) {
        return std::find_if(radarFrames.begin(), radarFrames.end(),
            [](const RadarFrame& candidate) {
                return candidate.sectorActivitiesDbfs[0] == kRadarSilenceDbfs;
            });
    };
    EXPECT_NE(firstSilent(noHoldFrames), noHoldFrames.end());
    EXPECT_NE(firstSilent(heldFrames), heldFrames.end());
    bool holdIncreasedPersistence = false;
    for (size_t index = 0; index < noHoldFrames.size(); ++index) {
        if (heldFrames[index].sectorActivitiesDbfs[0] >
            noHoldFrames[index].sectorActivitiesDbfs[0] + 0.1f) {
            holdIncreasedPersistence = true;
        }
    }
    EXPECT_TRUE(holdIncreasedPersistence);

    config.holdMilliseconds = 3000;
    processor.SetConfig(config);
    EXPECT_EQ(processor.GetConfig().holdMilliseconds, 2000u);
}

TEST(RadarProcessorTest, ResetsOnGenerationLayoutAndTimelineDiscontinuity) {
    const AudioChannelLayout layout = Layout51();
    RadarProcessor processor(ImmediateConfig());
    auto partial = MakeAudio(layout, RadarProcessor::kFftSize - 1,
                             std::array{Tone{AudioChannelRole::FrontCenter, 800.0f, 0.2f}});
    processor.PushAudio(View(partial, layout));
    EXPECT_EQ(processor.GetAvailableFrames(), 0u);

    std::vector<float> oneFrame(layout.channelCount, 0.0f);
    processor.PushAudio(View(oneFrame, layout,
                             RadarProcessor::kFftSize - 1, 2, true));
    EXPECT_EQ(processor.GetAvailableFrames(), 0u);

    auto full = MakeAudio(layout, RadarProcessor::kFftSize,
                          std::array{Tone{AudioChannelRole::FrontCenter, 800.0f, 0.2f}});
    processor.PushAudio(View(full, layout, 20'000, 2, false));
    EXPECT_EQ(processor.GetAvailableFrames(), 1u);
    EXPECT_EQ(LastFrame(processor).timestampSample, 20'000u + RadarProcessor::kFftSize / 2u);
}

TEST(RadarProcessorTest, SelectsAtMostThreeSeparatedLocalPeaksWithinEighteenDb) {
    std::array<float, kRadarSectorCount> levels{};
    levels.fill(kRadarSilenceDbfs);
    levels[0] = -10.0f;
    levels[1] = -11.0f;  // Not a local maximum and only 15 degrees away.
    levels[4] = -15.0f;
    levels[10] = -20.0f;
    levels[16] = -27.9f;
    levels[20] = -28.1f; // More than 18 dB down.
    std::array<RadarPeak, kRadarMaximumPeaks> peaks{};
    const uint32_t count = RadarProcessor::SelectPeaks(levels, 0.8f, peaks, -48.0f);
    ASSERT_EQ(count, 3u);
    EXPECT_LT(CircularDistance(peaks[0].azimuthDegrees, 0.0f), 8.0f);
    EXPECT_LT(CircularDistance(peaks[1].azimuthDegrees, 60.0f), 8.0f);
    EXPECT_LT(CircularDistance(peaks[2].azimuthDegrees, 150.0f), 8.0f);
    EXPECT_GE(CircularDistance(peaks[0].azimuthDegrees, peaks[1].azimuthDegrees), 30.0f);
}

TEST(RadarProcessorTest, SeparatesRefinedPeakAzimuthsRatherThanSectorCenters) {
    std::array<float, kRadarSectorCount> levels{};
    levels.fill(kRadarSilenceDbfs);
    levels[0] = -10.0f;
    levels[1] = -10.5f;  // Shallow valley pulls both neighboring peaks inward.
    levels[2] = -10.1f;
    levels[8] = -12.0f;  // A well-separated peak must still be retained.

    std::array<RadarPeak, kRadarMaximumPeaks> peaks{};
    const uint32_t count = RadarProcessor::SelectPeaks(levels, 0.8f, peaks, -48.0f);

    ASSERT_EQ(count, 2u);
    for (uint32_t left = 0; left < count; ++left) {
        for (uint32_t right = left + 1u; right < count; ++right) {
            EXPECT_GE(CircularDistance(peaks[left].azimuthDegrees,
                                       peaks[right].azimuthDegrees),
                      30.0f);
        }
    }
    EXPECT_LT(CircularDistance(peaks[0].azimuthDegrees, 0.0f), 15.0f);
    EXPECT_LT(CircularDistance(peaks[1].azimuthDegrees, 120.0f), 8.0f);
}

TEST(RadarProcessorTest, EventWindowSeparatesDifferentFrequencies) {
    const AudioChannelLayout layout = Layout71();
    const size_t frameCount = RadarProcessor::kFftSize + 3 * RadarProcessor::kHopSize;
    auto audio = MakeAudio(layout, frameCount,
        std::array{
            Tone{AudioChannelRole::FrontCenter, 937.5f, 0.2f},
            Tone{AudioChannelRole::SideRight, 3000.0f, 0.2f},
        });
    SoundEvent event;
    event.streamGeneration = 1;
    RadarProcessor processor(ImmediateConfig());
    const RadarEventResult result = processor.AnalyzeEventWindow(event, View(audio, layout));
    ASSERT_EQ(result.status, RadarRuntimeStatus::Active);
    ASSERT_EQ(result.peakCount, 2u);
    std::array<float, 2> azimuths{
        result.peaks[0].azimuthDegrees,
        result.peaks[1].azimuthDegrees,
    };
    std::sort(azimuths.begin(), azimuths.end());
    EXPECT_NEAR(azimuths[0], 0.0f, 8.0f);
    EXPECT_NEAR(azimuths[1], 90.0f, 8.0f);
}

TEST(RadarProcessorTest, OpposingSameBandEnergyCancels) {
    const AudioChannelLayout layout = Layout71();
    auto audio = MakeAudio(layout, RadarProcessor::kFftSize,
        std::array{
            Tone{AudioChannelRole::SideLeft, 1500.0f, 0.2f},
            Tone{AudioChannelRole::SideRight, 1500.0f, 0.2f},
        });
    SoundEvent event;
    RadarProcessor processor(ImmediateConfig());
    const RadarEventResult result = processor.AnalyzeEventWindow(event, View(audio, layout));
    EXPECT_EQ(result.status, RadarRuntimeStatus::Silent);
    EXPECT_EQ(result.peakCount, 0u);
}

TEST(RadarProcessorTest, SanitizesNonfiniteSamplesAndConfiguration) {
    RadarProcessorConfig config;
    config.sensitivityDbfs = std::numeric_limits<float>::quiet_NaN();
    config.customCurveDb[3] = std::numeric_limits<float>::infinity();
    config.attackMilliseconds = 10'000;
    const RadarProcessorConfig clean = RadarProcessor::SanitizeConfig(config);
    EXPECT_FLOAT_EQ(clean.sensitivityDbfs, -48.0f);
    EXPECT_FLOAT_EQ(clean.customCurveDb[3], 0.0f);
    EXPECT_EQ(clean.attackMilliseconds, 2000u);

    const AudioChannelLayout layout = Layout51();
    std::vector<float> samples(RadarProcessor::kFftSize * layout.channelCount,
                               std::numeric_limits<float>::quiet_NaN());
    RadarProcessor processor(ImmediateConfig());
    processor.PushAudio(View(samples, layout));
    const RadarFrame frame = LastFrame(processor);
    EXPECT_EQ(frame.status, RadarRuntimeStatus::Silent);
    EXPECT_TRUE(std::all_of(frame.sectorActivitiesDbfs.begin(),
                            frame.sectorActivitiesDbfs.end(),
                            [](float value) { return std::isfinite(value); }));
}

} // namespace
} // namespace EchoRadar
