#include <gtest/gtest.h>
#include "../src/features/FeatureExtractor.h"
#include <cmath>

using namespace EchoRadar;

namespace {

STFTFrame MakeEmptyFrame(uint32_t fft_size = 1024, uint32_t sample_rate = 48000) {
    STFTFrame frame;
    frame.fft_size = fft_size;
    frame.hop_size = 512;
    frame.sample_rate = sample_rate;

    const size_t bins = (fft_size / 2) + 1;
    frame.left.spectrum.assign(bins, {});
    frame.right.spectrum.assign(bins, {});
    frame.left.magnitudes.assign(bins, 0.0f);
    frame.right.magnitudes.assign(bins, 0.0f);
    frame.left.power.assign(bins, 0.0f);
    frame.right.power.assign(bins, 0.0f);
    return frame;
}

TEST(FeatureExtractor, ConstructDefault) {
    EXPECT_NO_THROW(FeatureExtractor fe);
}

TEST(FeatureExtractor, InvalidBandConfig) {
    FeatureExtractor::Config cfg;
    cfg.low_band_hz = 300.0f;
    cfg.mid_band_hz = 200.0f;
    EXPECT_THROW(FeatureExtractor fe(cfg), std::invalid_argument);
}

TEST(FeatureExtractor, BandEnergyCorrectness) {
    FeatureExtractor fe;
    auto frame = MakeEmptyFrame();

    frame.left.power[2] = 10.0f;    // low
    frame.left.power[20] = 20.0f;   // mid
    frame.left.power[100] = 30.0f;  // high

    frame.left.magnitudes[2] = std::sqrt(frame.left.power[2]);
    frame.left.magnitudes[20] = std::sqrt(frame.left.power[20]);
    frame.left.magnitudes[100] = std::sqrt(frame.left.power[100]);

    const AudioFeatures out = fe.Extract(frame);
    EXPECT_NEAR(out.lowBandEnergy, 10.0f, 1e-3f);
    EXPECT_NEAR(out.midBandEnergy, 20.0f, 1e-3f);
    EXPECT_NEAR(out.highBandEnergy, 30.0f, 1e-3f);
    EXPECT_NEAR(out.totalEnergy, 60.0f, 1e-3f);
}

TEST(FeatureExtractor, CentroidSingleTone) {
    FeatureExtractor fe;
    auto frame = MakeEmptyFrame();
    constexpr uint32_t tone_bin = 21; // 984.375 Hz @ 48k/1024
    const float tone_mag = 8.0f;

    frame.left.magnitudes[tone_bin] = tone_mag;
    frame.right.magnitudes[tone_bin] = tone_mag;
    frame.left.power[tone_bin] = tone_mag * tone_mag;
    frame.right.power[tone_bin] = tone_mag * tone_mag;

    const AudioFeatures out = fe.Extract(frame);
    const float expected_hz = static_cast<float>(tone_bin) *
                              static_cast<float>(frame.sample_rate) /
                              static_cast<float>(frame.fft_size);
    EXPECT_NEAR(out.spectralCentroid, expected_hz, 1e-3f);
}

TEST(FeatureExtractor, LeftRightBalanceSign) {
    FeatureExtractor fe;
    auto frame = MakeEmptyFrame();

    frame.left.power[15] = 20.0f;
    frame.right.power[15] = 5.0f;
    frame.left.magnitudes[15] = std::sqrt(frame.left.power[15]);
    frame.right.magnitudes[15] = std::sqrt(frame.right.power[15]);

    const AudioFeatures out = fe.Extract(frame);
    EXPECT_GT(out.leftRightBalance, 0.0f);
    EXPECT_NEAR(out.leftRightBalance, (20.0f - 5.0f) / (20.0f + 5.0f), 1e-4f);
}

TEST(FeatureExtractor, TransientIncreasesOnEnergyStep) {
    FeatureExtractor fe;
    auto low = MakeEmptyFrame();
    auto high = MakeEmptyFrame();

    low.left.power[40] = 1.0f;
    low.right.power[40] = 1.0f;
    low.left.magnitudes[40] = 1.0f;
    low.right.magnitudes[40] = 1.0f;

    high.left.power[40] = 50.0f;
    high.right.power[40] = 50.0f;
    high.left.magnitudes[40] = std::sqrt(50.0f);
    high.right.magnitudes[40] = std::sqrt(50.0f);

    const AudioFeatures first = fe.Extract(low);
    const AudioFeatures second = fe.Extract(high);

    EXPECT_NEAR(first.energyDelta, 0.0f, 1e-6f);
    EXPECT_GT(second.energyDelta, 0.0f);
    EXPECT_GT(second.transientScore, first.transientScore);
}

TEST(FeatureExtractor, ExtendedFeaturesRespondToFrameChange) {
    FeatureExtractor fe;
    auto silent = MakeEmptyFrame();
    auto pulse = MakeEmptyFrame();

    pulse.left.power[120] = 40.0f;
    pulse.right.power[120] = 30.0f;
    pulse.left.magnitudes[120] = std::sqrt(pulse.left.power[120]);
    pulse.right.magnitudes[120] = std::sqrt(pulse.right.power[120]);

    const AudioFeatures first = fe.Extract(silent);
    const AudioFeatures second = fe.Extract(pulse);

    EXPECT_NEAR(first.energyRise, 0.0f, 1e-6f);
    EXPECT_NEAR(first.spectralFlux, 0.0f, 1e-6f);
    EXPECT_GT(second.logEnergy, first.logEnergy);
    EXPECT_GT(second.energyRise, 0.0f);
    EXPECT_GT(second.spectralFlux, 0.0f);
    EXPECT_GT(second.hfEnergyRatio, 0.0f);
}

} // namespace
