#include <gtest/gtest.h>

#include <audio/PcmWav.h>

#include <filesystem>

using namespace EchoRadar;

TEST(PcmWav, RoundTripsStereoPcm16) {
    const auto path = std::filesystem::temp_directory_path() /
        "echoradar-pcm-wav-test.wav";
    std::filesystem::remove(path);

    PcmAudio source;
    source.sampleRate = 48000;
    source.channels = 2;
    source.interleaved = {-1.0f, 1.0f, -0.25f, 0.25f, 0.0f, 0.5f};
    std::string error;
    ASSERT_TRUE(WritePcm16Wav(path, source, &error)) << error;

    PcmAudio loaded;
    ASSERT_TRUE(LoadPcmWav(path, loaded, &error)) << error;
    EXPECT_EQ(loaded.sampleRate, 48000u);
    EXPECT_EQ(loaded.channels, 2u);
    ASSERT_EQ(loaded.interleaved.size(), source.interleaved.size());
    for (size_t index = 0; index < source.interleaved.size(); ++index) {
        EXPECT_NEAR(loaded.interleaved[index], source.interleaved[index], 1.0f / 32767.0f);
    }
    std::filesystem::remove(path);
}

TEST(PcmWav, RejectsUnsupportedChannelCount) {
    const auto path = std::filesystem::temp_directory_path() /
        "echoradar-pcm-wav-invalid.wav";
    PcmAudio source;
    source.sampleRate = 48000;
    source.channels = 3;
    source.interleaved = {0.0f, 0.25f, -0.25f};
    std::string error;
    EXPECT_FALSE(WritePcm16Wav(path, source, &error));
    EXPECT_FALSE(error.empty());
    std::filesystem::remove(path);
}
