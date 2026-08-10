#include <gtest/gtest.h>

#include <localization/StereoDirectionEstimator.h>
#include <localization/CalibrationController.h>

#include <cmath>
#include <filesystem>
#include <vector>

using namespace EchoRadar;

namespace {

std::vector<float> DirectionFixture(int rightLeadSamples, float rightGain = 1.35f) {
    constexpr size_t frames = 12000;
    std::vector<float> mono(frames + 64, 0.0f);
    uint32_t state = 0x12345678u;
    for (size_t index = 0; index < mono.size(); ++index) {
        state = state * 1664525u + 1013904223u;
        const float noise = static_cast<float>((state >> 8) & 0xffffu) / 32768.0f - 1.0f;
        mono[index] = 0.15f * noise + 0.12f * std::sin(index * 0.17f);
    }
    std::vector<float> stereo(frames * 2, 0.0f);
    const size_t delay = static_cast<size_t>(std::abs(rightLeadSamples));
    for (size_t frame = delay; frame < frames; ++frame) {
        if (rightLeadSamples >= 0) {
            stereo[frame * 2] = mono[frame - delay];
            stereo[frame * 2 + 1] = mono[frame] * rightGain;
        } else {
            stereo[frame * 2] = mono[frame] * rightGain;
            stereo[frame * 2 + 1] = mono[frame - delay];
        }
    }
    return stereo;
}

} // namespace

TEST(StereoDirectionEstimator, ChannelSwapMirrorsPrimaryDirection) {
    StereoDirectionEstimator estimator;
    AudioProfile profile;
    LocalizationTuning tuning;
    tuning.minimumConfidence = 0.01f;
    const auto right = DirectionFixture(7);
    StereoDirectionFeatures rightFeatures;
    ASSERT_TRUE(estimator.ExtractFeatures(right, rightFeatures));
    EXPECT_NEAR(std::abs(rightFeatures.itdSamples), 7.0f, 0.75f);
    std::vector<float> swapped(right.size());
    for (size_t frame = 0; frame < right.size() / 2; ++frame) {
        swapped[frame * 2] = right[frame * 2 + 1];
        swapped[frame * 2 + 1] = right[frame * 2];
    }
    const auto rightResult = estimator.Estimate(
        1, SoundClass::Footstep, right, profile, tuning);
    const auto leftResult = estimator.Estimate(
        2, SoundClass::Footstep, swapped, profile, tuning);
    EXPECT_LT(CircularDistanceDegrees(
        leftResult.primaryAngleDegrees,
        WrapDirectionDegrees(360.0f - rightResult.primaryAngleDegrees)), 20.0f);
    EXPECT_GT(rightResult.primaryAngleDegrees, 0.0f);
    EXPECT_LT(rightResult.primaryAngleDegrees, 180.0f);
}

TEST(StereoDirectionEstimator, MonoInputHasLowConfidence) {
    StereoDirectionEstimator estimator;
    std::vector<float> stereo(12000 * 2);
    for (size_t frame = 0; frame < stereo.size() / 2; ++frame) {
        const float sample = 0.2f * std::sin(frame * 0.11f);
        stereo[frame * 2] = sample;
        stereo[frame * 2 + 1] = sample;
    }
    AudioProfile profile;
    LocalizationTuning tuning;
    const auto result = estimator.Estimate(
        1, SoundClass::Footstep, stereo, profile, tuning);
    EXPECT_EQ(result.status, DirectionStatus::LowConfidence);
    EXPECT_LT(result.confidence, tuning.minimumConfidence);
}

TEST(StereoDirectionEstimator, CalibrationPrototypeResolvesRearMode) {
    StereoDirectionEstimator estimator;
    const auto stereo = DirectionFixture(7);
    StereoDirectionFeatures features;
    ASSERT_TRUE(estimator.ExtractFeatures(stereo, features));

    AudioProfile profile;
    DirectionCalibrationProfile calibration;
    calibration.SetAudioProfileKey(profile.StableKey());
    for (int index = 0; index < 6; ++index) {
        calibration.AddSample({SoundClass::Footstep, 135.0f, features});
    }
    LocalizationTuning tuning;
    tuning.minimumConfidence = 0.01f;
    const auto result = estimator.EstimateFeatures(
        9, SoundClass::Footstep, features, profile, tuning, &calibration);
    EXPECT_EQ(result.profileSource, DirectionProfileSource::Calibrated);
    EXPECT_LT(CircularDistanceDegrees(result.primaryAngleDegrees, 135.0f), 16.0f);
}

TEST(DirectionCalibrationProfile, SaveLoadRoundTrip) {
    const auto root = std::filesystem::temp_directory_path() /
        "echoradar-direction-calibration-test";
    std::filesystem::remove_all(root);
    DirectionCalibrationProfile profile;
    AudioProfile audio;
    audio.leftRightIsolationPercent = 42.0f;
    profile.SetAudioProfileKey(audio.StableKey());
    StereoDirectionFeatures features;
    features.rms = 0.1f;
    features.stereoQuality = 0.8f;
    profile.AddSample({SoundClass::Footstep, 270.0f, features});

    std::string error;
    ASSERT_TRUE(profile.Save(root / "calibration.tsv", &error)) << error;
    DirectionCalibrationProfile loaded;
    ASSERT_TRUE(loaded.Load(root / "calibration.tsv", &error)) << error;
    EXPECT_TRUE(loaded.Matches(audio));
    ASSERT_EQ(loaded.SampleCount(), 1u);
    EXPECT_FLOAT_EQ(loaded.Samples()[0].angleDegrees, 270.0f);
    std::filesystem::remove_all(root);
}

TEST(CalibrationController, QuickModeAcceptsOneIndependentEventPerTarget) {
    const auto root = std::filesystem::temp_directory_path() /
        "echoradar-calibration-controller-test";
    std::filesystem::remove_all(root);
    CalibrationController controller(root / "profile.tsv");
    AudioProfile audio;
    controller.Begin(CalibrationController::Mode::Quick, audio);
    StereoDirectionFeatures features;
    features.rms = 0.1f;
    features.correlationPeak = 0.8f;
    features.stereoQuality = 0.7f;
    for (size_t index = 0; index < 24; ++index) {
        controller.ArmNext();
        ASSERT_TRUE(controller.AcceptArmedSample(SoundClass::Footstep, features));
        EXPECT_FALSE(controller.AcceptArmedSample(SoundClass::Footstep, features));
    }
    const auto state = controller.Snapshot();
    EXPECT_TRUE(state.complete);
    EXPECT_FALSE(state.active);
    EXPECT_EQ(state.acceptedSamples, 24u);
    EXPECT_EQ(controller.ProfileSnapshot().SampleCount(SoundClass::Footstep), 24u);
    std::filesystem::remove_all(root);
}

TEST(CalibrationController, AudioSettingChangeInvalidatesProfileMatch) {
    const auto root = std::filesystem::temp_directory_path() /
        "echoradar-calibration-profile-match-test";
    CalibrationController controller(root / "profile.tsv");
    AudioProfile original;
    controller.Begin(CalibrationController::Mode::Quick, original);
    StereoDirectionFeatures features;
    features.rms = 0.1f;
    features.correlationPeak = 0.8f;
    controller.ArmNext();
    ASSERT_TRUE(controller.AcceptArmedSample(SoundClass::Footstep, features));
    AudioProfile changed = original;
    changed.leftRightIsolationPercent = 50.0f;
    EXPECT_TRUE(controller.ProfileSnapshot().Matches(original));
    EXPECT_FALSE(controller.ProfileSnapshot().Matches(changed));
}
