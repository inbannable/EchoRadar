#include <gtest/gtest.h>
#include "../src/audio/AudioHistoryBuffer.h"
#include <vector>

using namespace EchoRadar;

namespace {

std::vector<float> MakeRampStereo(size_t frames) {
    std::vector<float> out(frames * 2, 0.0f);
    for (size_t i = 0; i < frames; ++i) {
        out[i * 2] = static_cast<float>(i);
        out[i * 2 + 1] = static_cast<float>(1000 + i);
    }
    return out;
}

} // namespace

TEST(AudioHistoryBuffer, ExtractWindowBySampleRange) {
    AudioHistoryBuffer history(16, 48000);
    const auto block = MakeRampStereo(10);
    history.PushInterleaved(block.data(), 10, 100);

    std::vector<float> out;
    ASSERT_TRUE(history.ExtractWindow(103, 4, out));
    ASSERT_EQ(out.size(), 8u);
    EXPECT_FLOAT_EQ(out[0], 3.0f);
    EXPECT_FLOAT_EQ(out[1], 1003.0f);
    EXPECT_FLOAT_EQ(out[6], 6.0f);
    EXPECT_FLOAT_EQ(out[7], 1006.0f);
}

TEST(AudioHistoryBuffer, KeepsMostRecentFramesWhenOverflowing) {
    AudioHistoryBuffer history(8, 48000);
    const auto block = MakeRampStereo(12);
    history.PushInterleaved(block.data(), 12, 200);

    EXPECT_EQ(history.GetStoredFrames(), 8u);
    EXPECT_EQ(history.GetOldestSample(), 204u);
    EXPECT_EQ(history.GetNewestSampleExclusive(), 212u);
}

TEST(AudioHistoryBuffer, ExtractWindowByTimeUsesPreAndPost) {
    AudioHistoryBuffer history(48000, 48000);
    const auto block = MakeRampStereo(1000);
    history.PushInterleaved(block.data(), 1000, 0);

    std::vector<float> out;
    uint64_t startSample = 0;
    ASSERT_TRUE(history.ExtractWindowByTime(0.010, 0.002, 0.002, out, startSample));
    EXPECT_EQ(startSample, 384u);
    EXPECT_EQ(out.size(), static_cast<size_t>((96 + 96) * 2));
}
