#include <gtest/gtest.h>
#include "../src/dsp/STFTProcessor.h"
#include "../src/dsp/WindowFunctions.h"
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

using namespace EchoRadar;

namespace {

constexpr float kPi = 3.14159265358979323846f;

uint32_t PeakBin(const STFTChannelFrame& channel) {
    uint32_t peakBin = 0;
    float maxMag = -1.0f;
    for (uint32_t i = 0; i < channel.magnitudes.size(); ++i) {
        if (channel.magnitudes[i] > maxMag) {
            maxMag = channel.magnitudes[i];
            peakBin = i;
        }
    }
    return peakBin;
}

std::vector<float> BuildInterleavedTone(float leftHz,
                                        float rightHz,
                                        uint32_t sampleRate,
                                        size_t frameCount) {
    std::vector<float> out(frameCount * 2, 0.0f);
    for (size_t i = 0; i < frameCount; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(sampleRate);
        out[i * 2] = std::sin(2.0f * kPi * leftHz * t);
        out[i * 2 + 1] = std::sin(2.0f * kPi * rightHz * t);
    }
    return out;
}

} // namespace

TEST(STFTProcessor, ConstructDefault) {
    EXPECT_NO_THROW(STFTProcessor p);
}

TEST(STFTProcessor, InvalidFFTSize) {
    STFTProcessor::Config cfg;
    cfg.fft_size = 1000;
    EXPECT_THROW(STFTProcessor p(cfg), std::invalid_argument);
}

TEST(STFTProcessor, BinToHz) {
    STFTProcessor p;
    EXPECT_FLOAT_EQ(p.BinToHz(0), 0.0f);
    EXPECT_NEAR(p.BinToHz(512), 24000.0f, 1.0f);
}

TEST(WindowFunctions, HannWindowBasicProperties) {
    const auto window = MakeHannWindow(1024);
    ASSERT_EQ(window.size(), 1024u);
    EXPECT_NEAR(window.front(), 0.0f, 1e-6f);
    EXPECT_NEAR(window.back(), 0.0f, 1e-6f);
    EXPECT_NEAR(window[512], 1.0f, 1e-3f);

    for (float v : window) {
        EXPECT_GE(v, 0.0f);
        EXPECT_LE(v, 1.0f);
    }
}

TEST(STFTProcessor, StreamingFrameAndHopBehavior) {
    STFTProcessor p;
    std::vector<float> chunkA(700 * 2, 0.0f);
    std::vector<float> chunkB(900 * 2, 0.0f);

    p.PushInterleaved(chunkA.data(), 700);
    EXPECT_EQ(p.GetAvailableSTFTFrames(), 0u);

    p.PushInterleaved(chunkB.data(), 900);
    EXPECT_EQ(p.GetAvailableSTFTFrames(), 2u);

    STFTFrame f0, f1;
    ASSERT_TRUE(p.PopFrame(f0));
    ASSERT_TRUE(p.PopFrame(f1));
    EXPECT_EQ(f0.start_sample, 0u);
    EXPECT_EQ(f1.start_sample, 512u);
    EXPECT_EQ(f0.frame_index, 0u);
    EXPECT_EQ(f1.frame_index, 1u);
    EXPECT_FALSE(p.PopFrame(f1));
}

TEST(STFTProcessor, StereoDeinterleaveChannelIsolation) {
    STFTProcessor p;
    std::vector<float> interleaved(1024 * 2, 0.0f);
    for (size_t i = 0; i < 1024; ++i) {
        interleaved[i * 2] = 1.0f;
        interleaved[i * 2 + 1] = 0.0f;
    }

    p.PushInterleaved(interleaved.data(), 1024);
    STFTFrame frame;
    ASSERT_TRUE(p.PopFrame(frame));

    const float rightEnergy = std::accumulate(
        frame.right.power.begin(), frame.right.power.end(), 0.0f);
    const float leftEnergy = std::accumulate(
        frame.left.power.begin(), frame.left.power.end(), 0.0f);

    EXPECT_GT(leftEnergy, 0.1f);
    EXPECT_NEAR(rightEnergy, 0.0f, 1e-5f);
}

TEST(STFTProcessor, PureTonePeakBinAndStereoSeparation) {
    STFTProcessor p;
    constexpr float leftHz = 1000.0f;
    constexpr float rightHz = 2000.0f;

    const auto samples = BuildInterleavedTone(leftHz, rightHz, 48000, 4096);
    p.PushInterleaved(samples.data(), 4096);

    STFTFrame frame;
    STFTFrame last;
    bool got = false;
    while (p.PopFrame(frame)) {
        last = frame;
        got = true;
    }
    ASSERT_TRUE(got);

    const float binHz = static_cast<float>(last.sample_rate) /
                        static_cast<float>(last.fft_size);
    const float leftPeakHz = static_cast<float>(PeakBin(last.left)) * binHz;
    const float rightPeakHz = static_cast<float>(PeakBin(last.right)) * binHz;

    EXPECT_NEAR(leftPeakHz, leftHz, 60.0f);
    EXPECT_NEAR(rightPeakHz, rightHz, 60.0f);
    EXPECT_GT(std::abs(leftPeakHz - rightPeakHz), 700.0f);
}
