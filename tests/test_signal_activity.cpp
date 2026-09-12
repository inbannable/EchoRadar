#include <audio/SignalActivity.h>
#include <gtest/gtest.h>
#include <array>
#include <limits>
using namespace EchoRadar;

TEST(SignalActivity, HeadphoneEnergyIsNotAnAzimuthOrSemanticEvent) {
    SignalActivity activity;
    const auto layout = *MakeAudioChannelLayout(2, WindowsSpeaker::Stereo);
    std::array<float, 960> pcm{};
    for (size_t i = 0; i < 480; ++i) pcm[2 * i] = 0.01f;
    activity.Push(pcm.data(), 480, layout);
    EXPECT_TRUE(activity.Snapshot().active);
    EXPECT_TRUE(activity.Snapshot().stereoBalanceAvailable);
    EXPECT_NEAR(activity.Snapshot().balance, -1.0f, 1e-6f);
    EXPECT_NEAR(activity.Snapshot().rmsDbfs, -43.0103f, 0.001f);
    for (size_t i = 0; i < 480; ++i) { pcm[2*i] = 0; pcm[2*i+1] = 0.01f; }
    activity.Push(pcm.data(), 480, layout);
    EXPECT_NEAR(activity.Snapshot().balance, 1.0f, 1e-6f);
}

TEST(SignalActivity, HoldsAcrossShortSilenceThenReleasesAndResets) {
    SignalActivity activity;
    const auto layout = *MakeAudioChannelLayout(2, WindowsSpeaker::Stereo);
    std::array<float, 960> pcm;
    pcm.fill(0.01f);
    activity.Push(pcm.data(), 480, layout);
    pcm.fill(0.0f);
    for (int i = 0; i < 14; ++i) activity.Push(pcm.data(), 480, layout);
    EXPECT_TRUE(activity.Snapshot().active);
    activity.Push(pcm.data(), 480, layout);
    EXPECT_FALSE(activity.Snapshot().active);
    pcm.fill(0.01f);
    activity.Push(pcm.data(), 480, layout);
    activity.Reset();
    EXPECT_FALSE(activity.Snapshot().active);
    EXPECT_EQ(activity.Snapshot().rmsDbfs, -120.0f);
}

TEST(SignalActivity, LfeActivityIsVisibleWithoutClaimingStereoDirection) {
    SignalActivity activity;
    const auto layout = *MakeAudioChannelLayout(6, WindowsSpeaker::Surround51Back);
    std::array<float, 2880> pcm{};
    for (size_t i = 0; i < 480; ++i) pcm[6*i+3] = 0.1f;
    activity.Push(pcm.data(), 480, layout);
    EXPECT_TRUE(activity.Snapshot().active);
    EXPECT_FALSE(activity.Snapshot().stereoBalanceAvailable);
    pcm.fill(std::numeric_limits<float>::quiet_NaN());
    activity.Push(pcm.data(), 480, layout);
    EXPECT_TRUE(std::isfinite(activity.Snapshot().rmsDbfs));
}
