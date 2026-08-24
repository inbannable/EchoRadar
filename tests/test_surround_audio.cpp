#include <gtest/gtest.h>

#include <audio/AudioHistoryBuffer.h>
#include <audio/AudioCapture.h>
#include <audio/AudioRingBuffer.h>
#include <audio/AudioTypes.h>
#include <audio/Pcm16WavWriter.h>
#include <audio/PcmWav.h>

#include <filesystem>
#include <vector>

using namespace EchoRadar;

TEST(SurroundAudioLayout, ValidatesWindowsSpeakerMasksExactly) {
    EXPECT_TRUE(IsValidWindowsSpeakerMask(6, WindowsSpeaker::Surround51Back));
    EXPECT_TRUE(IsValidWindowsSpeakerMask(6, WindowsSpeaker::Surround51Side));
    EXPECT_TRUE(IsValidWindowsSpeakerMask(8, WindowsSpeaker::Surround71));

    EXPECT_FALSE(IsValidWindowsSpeakerMask(6, WindowsSpeaker::Surround71));
    EXPECT_FALSE(IsValidWindowsSpeakerMask(
        6, WindowsSpeaker::Stereo | WindowsSpeaker::FrontCenter |
               WindowsSpeaker::BackLeft | WindowsSpeaker::BackRight |
               WindowsSpeaker::SideLeft));
    EXPECT_FALSE(IsValidWindowsSpeakerMask(8, 0));
}

TEST(SurroundAudioLayout, MapsFivePointOneBackInSpeakerBitOrder) {
    const auto layout = MakeAudioChannelLayout(6, WindowsSpeaker::Surround51Back);
    ASSERT_TRUE(layout.has_value());
    EXPECT_EQ(layout->kind, AudioChannelLayoutKind::Surround51Back);
    EXPECT_EQ(layout->roles[0], AudioChannelRole::FrontLeft);
    EXPECT_EQ(layout->roles[1], AudioChannelRole::FrontRight);
    EXPECT_EQ(layout->roles[2], AudioChannelRole::FrontCenter);
    EXPECT_EQ(layout->roles[3], AudioChannelRole::Lfe);
    EXPECT_EQ(layout->roles[4], AudioChannelRole::BackLeft);
    EXPECT_EQ(layout->roles[5], AudioChannelRole::BackRight);
    EXPECT_FLOAT_EQ(*AudioChannelAzimuthDegrees(*layout, 4), 250.0f);
    EXPECT_FLOAT_EQ(*AudioChannelAzimuthDegrees(*layout, 5), 110.0f);
    EXPECT_FALSE(AudioChannelAzimuthDegrees(*layout, 3).has_value());
}

TEST(SurroundAudioLayout, MapsFivePointOneSideSurroundsToPlusMinus110) {
    const auto layout = MakeAudioChannelLayout(6, WindowsSpeaker::Surround51Side);
    ASSERT_TRUE(layout.has_value());
    EXPECT_EQ(layout->kind, AudioChannelLayoutKind::Surround51Side);
    EXPECT_EQ(layout->roles[4], AudioChannelRole::SideLeft);
    EXPECT_EQ(layout->roles[5], AudioChannelRole::SideRight);
    EXPECT_FLOAT_EQ(*AudioChannelAzimuthDegrees(*layout, 4), 250.0f);
    EXPECT_FLOAT_EQ(*AudioChannelAzimuthDegrees(*layout, 5), 110.0f);
}

TEST(SurroundAudioLayout, MapsSevenPointOneSidesAndBacks) {
    const auto layout = MakeAudioChannelLayout(8, WindowsSpeaker::Surround71);
    ASSERT_TRUE(layout.has_value());
    ASSERT_TRUE(layout->IsDirectional());
    EXPECT_EQ(layout->roles[4], AudioChannelRole::BackLeft);
    EXPECT_EQ(layout->roles[5], AudioChannelRole::BackRight);
    EXPECT_EQ(layout->roles[6], AudioChannelRole::SideLeft);
    EXPECT_EQ(layout->roles[7], AudioChannelRole::SideRight);
    EXPECT_FLOAT_EQ(*AudioChannelAzimuthDegrees(*layout, 4), 210.0f);
    EXPECT_FLOAT_EQ(*AudioChannelAzimuthDegrees(*layout, 5), 150.0f);
    EXPECT_FLOAT_EQ(*AudioChannelAzimuthDegrees(*layout, 6), 270.0f);
    EXPECT_FLOAT_EQ(*AudioChannelAzimuthDegrees(*layout, 7), 90.0f);
}

TEST(SurroundAudioLayout, NativeRadarEligibilityRejectsConversionAndUnsupportedRates) {
    const auto stereo = MakeAudioChannelLayout(2, WindowsSpeaker::Stereo);
    const auto surround = MakeAudioChannelLayout(6, WindowsSpeaker::Surround51Side);
    const auto sevenOne = MakeAudioChannelLayout(8, WindowsSpeaker::Surround71);
    ASSERT_TRUE(stereo.has_value());
    ASSERT_TRUE(surround.has_value());
    ASSERT_TRUE(sevenOne.has_value());

    AudioCaptureStatus status;
    status.nativeSampleRate = 48000;
    status.nativeChannels = surround->channelCount;
    status.nativeChannelMask = surround->channelMask;
    EXPECT_TRUE(IsNativeDirectionalRadarFormat(status, *surround));
    EXPECT_FALSE(IsNativeDirectionalRadarFormat(status, *sevenOne));

    status.nativeSampleRate = 44100;
    EXPECT_FALSE(IsNativeDirectionalRadarFormat(status, *surround));
    status.nativeSampleRate = 48000;
    status.nativeChannelMask = 0;
    EXPECT_FALSE(IsNativeDirectionalRadarFormat(status, *surround));

    status.nativeChannels = stereo->channelCount;
    status.nativeChannelMask = stereo->channelMask;
    EXPECT_FALSE(IsNativeDirectionalRadarFormat(status, *stereo));
}

TEST(SurroundRecognitionDownmix, AttenuatesCenterAndSurrounds) {
    const auto layout = MakeAudioChannelLayout(8, WindowsSpeaker::Surround71);
    ASSERT_TRUE(layout.has_value());
    const std::vector<float> source{0.10f, 0.10f, 0.20f, 1.0f,
                                    0.20f, 0.20f, 0.20f, 0.20f};
    const auto stereo = DownmixForRecognition(source, 1, *layout);
    ASSERT_EQ(stereo.size(), 2u);
    EXPECT_NEAR(stereo[0], 0.10f + 0.20f * kRecognitionCenterGain + 0.20f,
                1e-6f);
    EXPECT_NEAR(stereo[1], 0.10f + 0.20f * kRecognitionCenterGain + 0.20f,
                1e-6f);
}

TEST(SurroundRecognitionDownmix, BoundsCoherentFullScaleMix) {
    const auto layout = MakeAudioChannelLayout(8, WindowsSpeaker::Surround71);
    ASSERT_TRUE(layout.has_value());
    const std::vector<float> source(8, 1.0f);
    const auto stereo = DownmixForRecognition(source, 1, *layout);
    ASSERT_EQ(stereo.size(), 2u);
    EXPECT_FLOAT_EQ(stereo[0], 1.0f);
    EXPECT_FLOAT_EQ(stereo[1], 1.0f);
}

TEST(SurroundRecognitionDownmix, ExcludesLfeCompletely) {
    const auto layout = MakeAudioChannelLayout(6, WindowsSpeaker::Surround51Side);
    ASSERT_TRUE(layout.has_value());
    std::vector<float> source(12, 0.0f);
    source[3] = 1.0f;
    source[9] = -1.0f;
    const auto stereo = DownmixForRecognition(source, 2, *layout);
    EXPECT_EQ(stereo, std::vector<float>({0.0f, 0.0f, 0.0f, 0.0f}));
}

TEST(SurroundAudioRingBuffer, PreservesSixChannelFramesAcrossWrap) {
    AudioRingBuffer ring(4, 6);
    std::vector<float> first(18);
    for (size_t index = 0; index < first.size(); ++index) {
        first[index] = static_cast<float>(index);
    }
    ASSERT_EQ(ring.PushInterleaved(first.data(), 3), 3u);
    std::vector<float> discarded(12);
    ASSERT_EQ(ring.PopInterleaved(discarded.data(), 2), 2u);

    std::vector<float> second(12);
    for (size_t index = 0; index < second.size(); ++index) {
        second[index] = static_cast<float>(100 + index);
    }
    ASSERT_EQ(ring.PushInterleaved(second.data(), 2), 2u);
    std::vector<float> result(18);
    ASSERT_EQ(ring.PopInterleaved(result.data(), 3), 3u);
    EXPECT_EQ(std::vector<float>(result.begin(), result.begin() + 6),
              std::vector<float>(first.begin() + 12, first.end()));
    EXPECT_EQ(std::vector<float>(result.begin() + 6, result.end()), second);
}

TEST(SurroundAudioHistoryBuffer, ExtractsEightChannelTimeline) {
    AudioHistoryBuffer history(4, 48000, 8);
    std::vector<float> source(48);
    for (size_t index = 0; index < source.size(); ++index) {
        source[index] = static_cast<float>(index);
    }
    history.PushInterleaved(source.data(), 6, 50);
    EXPECT_EQ(history.GetOldestSample(), 52u);
    std::vector<float> result;
    ASSERT_TRUE(history.ExtractWindow(53, 2, result));
    ASSERT_EQ(result.size(), 16u);
    EXPECT_EQ(result, std::vector<float>(source.begin() + 24, source.begin() + 40));
}

TEST(SurroundPcmWav, RoundTripsExtensibleFivePointOneMask) {
    const auto path = std::filesystem::temp_directory_path() /
        "echoradar-surround-wav-test.wav";
    std::filesystem::remove(path);

    PcmAudio source;
    source.sampleRate = 48000;
    source.channels = 6;
    source.channelMask = WindowsSpeaker::Surround51Back;
    source.interleaved = {-1.0f, -0.5f, 0.0f, 0.25f, 0.5f, 1.0f};
    std::string error;
    ASSERT_TRUE(WritePcm16Wav(path, source, &error)) << error;

    PcmAudio loaded;
    ASSERT_TRUE(LoadPcmWav(path, loaded, &error)) << error;
    EXPECT_EQ(loaded.channels, 6u);
    EXPECT_EQ(loaded.channelMask, WindowsSpeaker::Surround51Back);
    ASSERT_EQ(loaded.interleaved.size(), source.interleaved.size());
    for (size_t index = 0; index < source.interleaved.size(); ++index) {
        EXPECT_NEAR(loaded.interleaved[index], source.interleaved[index],
                    1.0f / 32767.0f);
    }
    std::filesystem::remove(path);
}

TEST(Pcm16WavWriter, IncrementallyRoundTripsSevenPointOne) {
    const auto path = std::filesystem::temp_directory_path() /
        "echoradar-streaming-71-wav-test.wav";
    std::filesystem::remove(path);

    Pcm16WavWriter writer;
    Pcm16WavWriterConfig config;
    config.channels = 8;
    config.channelMask = WindowsSpeaker::Surround71;
    std::string error;
    ASSERT_TRUE(writer.Open(path, config, &error)) << error;

    const std::vector<float> first{
        -1.0f, -0.75f, -0.5f, -0.25f, 0.0f, 0.25f, 0.5f, 0.75f,
    };
    const std::vector<float> second{
        1.0f, 0.75f, 0.5f, 0.25f, 0.0f, -0.25f, -0.5f, -0.75f,
        0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f,
    };
    ASSERT_TRUE(writer.AppendFrames(first, 1, &error)) << error;
    ASSERT_TRUE(writer.AppendInterleaved(second, &error)) << error;
    EXPECT_EQ(writer.FrameCount(), 3u);
    EXPECT_EQ(writer.DataBytes(), 48u);
    ASSERT_TRUE(writer.Close(&error)) << error;

    PcmAudio loaded;
    ASSERT_TRUE(LoadPcmWav(path, loaded, &error)) << error;
    EXPECT_EQ(loaded.sampleRate, 48000u);
    EXPECT_EQ(loaded.channels, 8u);
    EXPECT_EQ(loaded.channelMask, WindowsSpeaker::Surround71);
    ASSERT_EQ(loaded.FrameCount(), 3u);
    std::vector<float> expected = first;
    expected.insert(expected.end(), second.begin(), second.end());
    for (size_t index = 0; index < expected.size(); ++index) {
        EXPECT_NEAR(loaded.interleaved[index], expected[index],
                    1.0f / 32767.0f);
    }
    std::filesystem::remove(path);
}

TEST(Pcm16WavWriter, DestructorFinalizesExtensibleHeader) {
    const auto path = std::filesystem::temp_directory_path() /
        "echoradar-streaming-destructor-wav-test.wav";
    std::filesystem::remove(path);
    std::string error;
    {
        Pcm16WavWriter writer;
        Pcm16WavWriterConfig config;
        config.channels = 6;
        config.channelMask = WindowsSpeaker::Surround51Back;
        ASSERT_TRUE(writer.Open(path, config, &error)) << error;
        const std::vector<float> frames(18, 0.25f);
        ASSERT_TRUE(writer.AppendFrames(frames, 3, &error)) << error;
    }

    PcmAudio loaded;
    ASSERT_TRUE(LoadPcmWav(path, loaded, &error)) << error;
    EXPECT_EQ(loaded.channels, 6u);
    EXPECT_EQ(loaded.channelMask, WindowsSpeaker::Surround51Back);
    EXPECT_EQ(loaded.FrameCount(), 3u);
    std::filesystem::remove(path);
}

TEST(Pcm16WavWriter, EnforcesFrameLimitAndStillFinalizesWrittenPrefix) {
    const auto path = std::filesystem::temp_directory_path() /
        "echoradar-streaming-limit-wav-test.wav";
    std::filesystem::remove(path);

    Pcm16WavWriter writer;
    Pcm16WavWriterConfig config;
    config.channels = 6;
    config.channelMask = WindowsSpeaker::Surround51Side;
    config.maximumFrames = 2;
    std::string error;
    ASSERT_TRUE(writer.Open(path, config, &error)) << error;
    const std::vector<float> prefix(12, 0.5f);
    ASSERT_TRUE(writer.AppendFrames(prefix, 2, &error)) << error;
    const std::vector<float> excess(6, 1.0f);
    EXPECT_FALSE(writer.AppendFrames(excess, 1, &error));
    EXPECT_TRUE(writer.HasError());
    EXPECT_EQ(writer.FrameCount(), 2u);
    EXPECT_FALSE(writer.Close(&error));

    PcmAudio loaded;
    ASSERT_TRUE(LoadPcmWav(path, loaded, &error)) << error;
    EXPECT_EQ(loaded.FrameCount(), 2u);
    EXPECT_EQ(loaded.channelMask, WindowsSpeaker::Surround51Side);
    std::filesystem::remove(path);
}

TEST(Pcm16WavWriter, RejectsMisalignedWritesAndMissingSurroundMask) {
    const auto path = std::filesystem::temp_directory_path() /
        "echoradar-streaming-invalid-wav-test.wav";
    std::filesystem::remove(path);
    std::string error;

    Pcm16WavWriter writer;
    Pcm16WavWriterConfig invalid;
    invalid.channels = 6;
    invalid.channelMask = 0;
    EXPECT_FALSE(writer.Open(path, invalid, &error));
    EXPECT_FALSE(writer.IsOpen());

    Pcm16WavWriterConfig stereo;
    stereo.channels = 2;
    ASSERT_TRUE(writer.Open(path, stereo, &error)) << error;
    const std::vector<float> misaligned{0.0f, 0.5f, 1.0f};
    EXPECT_FALSE(writer.AppendInterleaved(misaligned, &error));
    EXPECT_EQ(writer.DataBytes(), 0u);
    EXPECT_FALSE(writer.Close(&error));
    std::filesystem::remove(path);
}
