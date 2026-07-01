#include <gtest/gtest.h>
#include "../src/detector/GunshotEventDetector.h"
#include <cmath>
#include <stdexcept>
#include <vector>

using namespace EchoRadar;

namespace {

constexpr double kFrameStepSec = 512.0 / 48000.0;

AudioFeatures MakeNoiseFrame(float logEnergy,
                             float energyRise,
                             float spectralFlux,
                             float hfEnergyRatio,
                             float transientScore,
                             float leftRightBalance = 0.0f,
                             float centroid = 1200.0f,
                             float flatness = 0.3f) {
    AudioFeatures f{};
    f.totalEnergy = std::expm1(logEnergy);
    f.logEnergy = logEnergy;
    f.energyRise = energyRise;
    f.spectralFlux = spectralFlux;
    f.hfEnergyRatio = hfEnergyRatio;
    f.transientScore = transientScore;
    f.leftRightBalance = leftRightBalance;
    f.spectralCentroid = centroid;
    f.spectralFlatness = flatness;
    return f;
}

void PushFrames(GunshotEventDetector& detector, const std::vector<AudioFeatures>& frames, int startFrame = 0) {
    for (size_t i = 0; i < frames.size(); ++i) {
        detector.PushFrame(frames[i], (startFrame + static_cast<int>(i)) * kFrameStepSec, startFrame + static_cast<int>(i));
    }
}

std::vector<AudioFeatures> MakeSilenceFrames(int count) {
    std::vector<AudioFeatures> frames;
    frames.reserve(count);
    for (int i = 0; i < count; ++i) {
        frames.push_back(MakeNoiseFrame(0.0f, 0.0f, 0.0f, 0.0f, 0.0f));
    }
    return frames;
}

std::vector<AudioFeatures> MakePulseFrames(float peakScale = 1.0f) {
    return {
        MakeNoiseFrame(0.4f, 0.55f * peakScale, 0.50f * peakScale, 0.35f, 0.48f * peakScale, -0.08f, 1200.0f, 0.22f),
        MakeNoiseFrame(1.1f, 1.00f * peakScale, 0.92f * peakScale, 0.70f, 0.92f * peakScale, -0.12f, 2600.0f, 0.18f),
        MakeNoiseFrame(0.7f, 0.40f * peakScale, 0.35f * peakScale, 0.40f, 0.42f * peakScale, -0.10f, 1800.0f, 0.20f),
    };
}

} // namespace

TEST(GunshotEventDetector, SilenceDoesNotTrigger) {
    GunshotEventDetector detector;
    PushFrames(detector, MakeSilenceFrames(120));

    EXPECT_EQ(detector.GetAvailableEvents(), 0u);
    EXPECT_EQ(detector.GetState(), DetectorState::Idle);
    EXPECT_NEAR(detector.GetLastScore(), 0.0f, 1e-5f);
}

TEST(GunshotEventDetector, SinglePulseTriggersOneEvent) {
    GunshotEventDetector detector;

    auto frames = MakeSilenceFrames(10);
    auto pulse = MakePulseFrames();
    frames.insert(frames.end(), pulse.begin(), pulse.end());
    auto tail = MakeSilenceFrames(20);
    frames.insert(frames.end(), tail.begin(), tail.end());

    PushFrames(detector, frames);

    ASSERT_EQ(detector.GetAvailableEvents(), 1u);
    GunshotEvent ev;
    ASSERT_TRUE(detector.PopEvent(ev));

    EXPECT_GE(ev.candidateScore, 0.7f);
    EXPECT_GE(ev.peakFrame, ev.startFrame);
    EXPECT_GE(ev.endFrame, ev.peakFrame);
    EXPECT_FLOAT_EQ(ev.gunshotProb, ev.confidence);
    EXPECT_GE(ev.spectralCentroidAtPeak, 1000.0f);
}

TEST(GunshotEventDetector, ClosePulsesAreMerged) {
    GunshotEventDetector detector;

    auto frames = MakeSilenceFrames(10);
    auto pulse = MakePulseFrames();
    frames.insert(frames.end(), pulse.begin(), pulse.end());
    frames.push_back(MakeNoiseFrame(0.1f, 0.0f, 0.0f, 0.0f, 0.0f));
    frames.insert(frames.end(), pulse.begin(), pulse.end());
    auto gap = MakeSilenceFrames(15);
    frames.insert(frames.end(), gap.begin(), gap.end());

    PushFrames(detector, frames);

    EXPECT_EQ(detector.GetAvailableEvents(), 1u);
    GunshotEvent ev;
    ASSERT_TRUE(detector.PopEvent(ev));
    EXPECT_LT(ev.endFrame - ev.startFrame, 10);
}

TEST(GunshotEventDetector, DistantPulsesBecomeSeparateEvents) {
    GunshotEventDetector detector;

    auto frames = MakeSilenceFrames(10);
    auto pulse = MakePulseFrames();
    frames.insert(frames.end(), pulse.begin(), pulse.end());
    auto gap1 = MakeSilenceFrames(12);
    frames.insert(frames.end(), gap1.begin(), gap1.end());
    frames.insert(frames.end(), pulse.begin(), pulse.end());
    auto gap2 = MakeSilenceFrames(20);
    frames.insert(frames.end(), gap2.begin(), gap2.end());

    PushFrames(detector, frames);

    EXPECT_EQ(detector.GetAvailableEvents(), 2u);
    GunshotEvent first;
    GunshotEvent second;
    ASSERT_TRUE(detector.PopEvent(first));
    ASSERT_TRUE(detector.PopEvent(second));
    EXPECT_GT(second.startFrame, first.endFrame);
}

TEST(GunshotEventDetector, SustainedNoiseDoesNotSpamEvents) {
    GunshotEventDetector detector;
    std::vector<AudioFeatures> noise;
    noise.reserve(120);
    for (int i = 0; i < 120; ++i) {
        noise.push_back(MakeNoiseFrame(0.55f, 0.05f, 0.04f, 0.22f, 0.08f));
    }

    PushFrames(detector, noise);

    EXPECT_EQ(detector.GetAvailableEvents(), 0u);
    EXPECT_EQ(detector.GetState(), DetectorState::Idle);
    EXPECT_LT(detector.GetLastScore(), 0.5f);
}

TEST(GunshotEventDetector, PeakFrameIsTrackedCorrectly) {
    GunshotEventDetector detector;
    std::vector<AudioFeatures> frames = MakeSilenceFrames(8);
    frames.push_back(MakeNoiseFrame(0.4f, 0.4f, 0.4f, 0.3f, 0.3f));
    frames.push_back(MakeNoiseFrame(1.4f, 1.2f, 1.0f, 0.8f, 0.8f));
    frames.push_back(MakeNoiseFrame(0.9f, 0.7f, 0.6f, 0.5f, 0.5f));
    auto tail = MakeSilenceFrames(18);
    frames.insert(frames.end(), tail.begin(), tail.end());

    PushFrames(detector, frames);

    ASSERT_EQ(detector.GetAvailableEvents(), 1u);
    GunshotEvent ev;
    ASSERT_TRUE(detector.PopEvent(ev));
    EXPECT_EQ(ev.peakFrame, 9);
    EXPECT_GE(ev.candidateScore, 0.8f);
}

TEST(GunshotEventDetector, CooldownSuppressesImmediateRetrigger) {
    EventDetectorConfig cfg;
    cfg.refractoryFrames = 8;
    GunshotEventDetector detector(cfg);

    std::vector<AudioFeatures> frames = MakeSilenceFrames(8);
    auto pulse = MakePulseFrames();
    frames.insert(frames.end(), pulse.begin(), pulse.end());
    auto gap = MakeSilenceFrames(3);
    frames.insert(frames.end(), gap.begin(), gap.end());
    frames.insert(frames.end(), pulse.begin(), pulse.end());
    auto tail = MakeSilenceFrames(20);
    frames.insert(frames.end(), tail.begin(), tail.end());

    PushFrames(detector, frames);

    EXPECT_EQ(detector.GetAvailableEvents(), 1u);
}

TEST(GunshotEventDetector, TriggerThresholdCanBeUpdatedAtRuntime) {
    GunshotEventDetector detector;
    detector.SetTriggerThreshold(0.80f);
    EXPECT_FLOAT_EQ(detector.GetTriggerThreshold(), 0.80f);
    EXPECT_THROW(detector.SetTriggerThreshold(detector.GetReleaseThreshold()), std::invalid_argument);
}
