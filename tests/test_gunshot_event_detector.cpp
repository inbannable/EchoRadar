#include <gtest/gtest.h>
#include "../src/detector/GunshotEventDetector.h"
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <vector>

using namespace EchoRadar;

namespace {

constexpr double kFrameStepSec = 512.0 / 48000.0;

AudioFeatures MakeFrame(float logEnergy,
                        float energyRise,
                        float spectralFlux,
                        float hfEnergyRatio,
                        float transientScore,
                        float centroid,
                        float flatness,
                        float leftRightBalance = 0.0f) {
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

AudioFeatures MakeSilenceFrame() {
    return MakeFrame(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 400.0f, 0.2f);
}

void AppendSilence(std::vector<AudioFeatures>& frames, int count) {
    for (int i = 0; i < count; ++i) {
        frames.push_back(MakeSilenceFrame());
    }
}

void AppendGunshotPulse(std::vector<AudioFeatures>& frames, float strength = 1.0f) {
    frames.push_back(MakeFrame(0.18f, 0.10f * strength, 0.08f * strength, 0.12f, 0.10f * strength, 1200.0f, 0.20f, -0.05f));
    frames.push_back(MakeFrame(1.48f, 1.55f * strength, 1.40f * strength, 0.82f, 0.98f * strength, 3100.0f, 0.22f, -0.10f));
    frames.push_back(MakeFrame(0.70f, 0.20f * strength, 0.24f * strength, 0.30f, 0.26f * strength, 1800.0f, 0.24f, -0.07f));
    frames.push_back(MakeFrame(0.20f, 0.03f, 0.02f, 0.08f, 0.06f, 1100.0f, 0.28f));
}

void AppendFootstep(std::vector<AudioFeatures>& frames) {
    frames.push_back(MakeFrame(0.28f, 0.08f, 0.05f, 0.05f, 0.06f, 520.0f, 0.16f));
    frames.push_back(MakeFrame(0.42f, 0.10f, 0.06f, 0.06f, 0.08f, 560.0f, 0.18f));
    frames.push_back(MakeFrame(0.48f, 0.07f, 0.05f, 0.06f, 0.08f, 590.0f, 0.19f));
    frames.push_back(MakeFrame(0.40f, 0.02f, 0.03f, 0.05f, 0.05f, 540.0f, 0.20f));
    frames.push_back(MakeFrame(0.22f, 0.01f, 0.02f, 0.04f, 0.03f, 500.0f, 0.18f));
}

void AppendWeaponSwitch(std::vector<AudioFeatures>& frames) {
    frames.push_back(MakeFrame(0.50f, 0.18f, 0.16f, 0.10f, 0.11f, 880.0f, 0.42f));
    frames.push_back(MakeFrame(0.78f, 0.25f, 0.22f, 0.12f, 0.14f, 920.0f, 0.48f));
    frames.push_back(MakeFrame(0.90f, 0.21f, 0.18f, 0.11f, 0.13f, 940.0f, 0.46f));
    frames.push_back(MakeFrame(0.84f, 0.08f, 0.07f, 0.10f, 0.08f, 860.0f, 0.44f));
    frames.push_back(MakeFrame(0.66f, 0.03f, 0.04f, 0.09f, 0.06f, 820.0f, 0.40f));
}

void AppendReload(std::vector<AudioFeatures>& frames) {
    frames.push_back(MakeFrame(0.44f, 0.14f, 0.10f, 0.09f, 0.10f, 760.0f, 0.36f));
    frames.push_back(MakeFrame(0.64f, 0.16f, 0.12f, 0.10f, 0.12f, 840.0f, 0.50f));
    frames.push_back(MakeFrame(0.82f, 0.18f, 0.14f, 0.11f, 0.13f, 900.0f, 0.58f));
    frames.push_back(MakeFrame(0.78f, 0.08f, 0.08f, 0.10f, 0.09f, 860.0f, 0.57f));
    frames.push_back(MakeFrame(0.70f, 0.05f, 0.05f, 0.09f, 0.07f, 820.0f, 0.55f));
    frames.push_back(MakeFrame(0.40f, 0.02f, 0.03f, 0.08f, 0.05f, 760.0f, 0.50f));
}

void PushFrames(GunshotEventDetector& detector, const std::vector<AudioFeatures>& frames, int startFrame = 0) {
    for (size_t i = 0; i < frames.size(); ++i) {
        detector.PushFrame(frames[i],
                           (startFrame + static_cast<int>(i)) * kFrameStepSec,
                           startFrame + static_cast<int>(i));
    }
}

std::vector<GunshotEvent> DrainEvents(GunshotEventDetector& detector) {
    std::vector<GunshotEvent> out;
    GunshotEvent ev{};
    while (detector.PopEvent(ev)) {
        out.push_back(ev);
    }
    return out;
}

std::vector<CandidateDecision> DrainDecisions(GunshotEventDetector& detector) {
    std::vector<CandidateDecision> out;
    CandidateDecision decision{};
    while (detector.PopDecision(decision)) {
        out.push_back(decision);
    }
    return out;
}

int CountDecision(const std::vector<CandidateDecision>& decisions, CandidateDecisionType type) {
    int count = 0;
    for (const CandidateDecision& d : decisions) {
        if (d.type == type) {
            ++count;
        }
    }
    return count;
}

std::string DecisionStats(const std::vector<CandidateDecision>& decisions) {
    std::ostringstream oss;
    oss << "peak=" << CountDecision(decisions, CandidateDecisionType::Peak)
        << " accepted=" << CountDecision(decisions, CandidateDecisionType::Accepted)
        << " rej_temporal=" << CountDecision(decisions, CandidateDecisionType::RejectedTemporal)
        << " rej_fp=" << CountDecision(decisions, CandidateDecisionType::RejectedFalsePositive)
        << " rej_conf=" << CountDecision(decisions, CandidateDecisionType::RejectedConfidence);
    return oss.str();
}

} // namespace

TEST(GunshotEventDetector, SilenceDoesNotTrigger) {
    GunshotEventDetector detector;
    std::vector<AudioFeatures> frames;
    AppendSilence(frames, 160);
    PushFrames(detector, frames);

    EXPECT_TRUE(DrainEvents(detector).empty());
}

TEST(GunshotEventDetector, USP_SingleShotProducesOneEventWithConfidence) {
    GunshotEventDetector detector;
    std::vector<AudioFeatures> frames;
    AppendSilence(frames, 24);
    AppendGunshotPulse(frames, 1.0f);
    AppendSilence(frames, 20);
    PushFrames(detector, frames);

    std::vector<CandidateDecision> decisions = DrainDecisions(detector);
    std::vector<GunshotEvent> events = DrainEvents(detector);
    ASSERT_EQ(events.size(), 1u) << DecisionStats(decisions);
    EXPECT_GE(events[0].confidence, detector.GetConfig().minConfidence);
    EXPECT_GE(events[0].candidateScore, detector.GetTriggerThreshold());
}

TEST(GunshotEventDetector, AKBurstIsSplitIntoMultipleEvents) {
    GunshotEventDetector detector;
    std::vector<AudioFeatures> frames;
    AppendSilence(frames, 24);
    for (int i = 0; i < 4; ++i) {
        AppendGunshotPulse(frames, 1.0f);
        AppendSilence(frames, 3);
    }
    AppendSilence(frames, 24);
    PushFrames(detector, frames);

    std::vector<GunshotEvent> events = DrainEvents(detector);
    ASSERT_EQ(events.size(), 4u);
    for (size_t i = 1; i < events.size(); ++i) {
        EXPECT_GE(events[i].peakFrame - events[i - 1].peakFrame, detector.GetConfig().minEventSeparationFrames);
    }
}

TEST(GunshotEventDetector, M4BurstIsSplitIntoMultipleEvents) {
    GunshotEventDetector detector;
    std::vector<AudioFeatures> frames;
    AppendSilence(frames, 20);
    for (int i = 0; i < 5; ++i) {
        AppendGunshotPulse(frames, 0.95f);
        AppendSilence(frames, 2);
    }
    AppendSilence(frames, 20);
    PushFrames(detector, frames);

    std::vector<GunshotEvent> events = DrainEvents(detector);
    ASSERT_EQ(events.size(), 5u);
}

TEST(GunshotEventDetector, FootstepsDoNotTriggerGunshotEvents) {
    GunshotEventDetector detector;
    std::vector<AudioFeatures> frames;
    AppendSilence(frames, 20);
    for (int i = 0; i < 8; ++i) {
        AppendFootstep(frames);
        AppendSilence(frames, 3);
    }
    AppendSilence(frames, 12);
    PushFrames(detector, frames);

    EXPECT_TRUE(DrainEvents(detector).empty());
}

TEST(GunshotEventDetector, WeaponSwitchDoesNotTriggerGunshotEvents) {
    GunshotEventDetector detector;
    std::vector<AudioFeatures> frames;
    AppendSilence(frames, 24);
    for (int i = 0; i < 5; ++i) {
        AppendWeaponSwitch(frames);
        AppendSilence(frames, 2);
    }
    AppendSilence(frames, 20);
    PushFrames(detector, frames);

    EXPECT_TRUE(DrainEvents(detector).empty());
}

TEST(GunshotEventDetector, ReloadDoesNotTriggerGunshotEvents) {
    GunshotEventDetector detector;
    std::vector<AudioFeatures> frames;
    AppendSilence(frames, 24);
    for (int i = 0; i < 4; ++i) {
        AppendReload(frames);
        AppendSilence(frames, 3);
    }
    AppendSilence(frames, 18);
    PushFrames(detector, frames);

    EXPECT_TRUE(DrainEvents(detector).empty());
}

TEST(GunshotEventDetector, MixedAudioKeepsGunshotsAndRejectsNonGunshots) {
    GunshotEventDetector detector;
    std::vector<AudioFeatures> frames;
    AppendSilence(frames, 20);
    AppendFootstep(frames);
    AppendSilence(frames, 4);
    AppendGunshotPulse(frames, 1.0f);
    AppendSilence(frames, 5);
    AppendWeaponSwitch(frames);
    AppendSilence(frames, 5);
    AppendGunshotPulse(frames, 0.95f);
    AppendSilence(frames, 5);
    AppendReload(frames);
    AppendSilence(frames, 5);
    AppendGunshotPulse(frames, 1.05f);
    AppendSilence(frames, 20);
    PushFrames(detector, frames);

    std::vector<GunshotEvent> events = DrainEvents(detector);
    ASSERT_EQ(events.size(), 3u);
}

TEST(GunshotEventDetector, PeakPickingPrefersLocalMaximumWithoutRepeatingPlateau) {
    GunshotEventDetector detector;
    std::vector<AudioFeatures> frames;
    AppendSilence(frames, 20);
    frames.push_back(MakeFrame(0.60f, 0.70f, 0.60f, 0.60f, 0.70f, 2400.0f, 0.25f));
    frames.push_back(MakeFrame(0.85f, 1.00f, 0.92f, 0.68f, 0.90f, 2800.0f, 0.22f));
    frames.push_back(MakeFrame(0.84f, 0.99f, 0.91f, 0.68f, 0.90f, 2820.0f, 0.22f));
    frames.push_back(MakeFrame(0.72f, 0.60f, 0.50f, 0.40f, 0.50f, 2000.0f, 0.24f));
    AppendSilence(frames, 12);
    PushFrames(detector, frames);

    std::vector<CandidateDecision> decisions = DrainDecisions(detector);
    EXPECT_EQ(CountDecision(decisions, CandidateDecisionType::Peak), 1);
}

TEST(GunshotEventDetector, TriggerThresholdCanBeUpdatedAtRuntime) {
    GunshotEventDetector detector;
    detector.SetTriggerThreshold(0.80f);
    EXPECT_FLOAT_EQ(detector.GetTriggerThreshold(), 0.80f);
    EXPECT_THROW(detector.SetTriggerThreshold(detector.GetReleaseThreshold()), std::invalid_argument);
}
