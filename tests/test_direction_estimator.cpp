#include <gtest/gtest.h>

#include <localization/StereoDirectionEstimator.h>
#include <localization/CalibrationController.h>

#include <cmath>
#include <filesystem>
#include <fstream>
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

StereoDirectionFeatures CalibrationFeatures() {
    StereoDirectionFeatures features;
    features.rms = 0.1f;
    features.gccPeak = 0.8f;
    features.gccSharpness = 0.5f;
    features.gccPeakToSidelobe = 0.6f;
    features.peakToNoiseDb = 14.0f;
    features.activeFrameFraction = 0.12f;
    features.stereoQuality = 0.7f;
    features.bandCoherence.fill(0.8f);
    return features;
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
    // Shared golden vector with ml/tests/test_direction.py. Tolerances cover
    // KissFFT/NumPy floating-point implementation differences.
    EXPECT_EQ(rightFeatures.schemaVersion, StereoDirectionFeatures::kSchemaVersion);
    EXPECT_NEAR(rightFeatures.broadbandIldDb, -2.69309f, 0.03f);
    EXPECT_NEAR(std::abs(rightFeatures.gccDelaySamples), 7.0f, 0.75f);
    EXPECT_NEAR(rightFeatures.gccPeak, 0.99998f, 0.03f);
    EXPECT_NEAR(rightFeatures.gccPeakToSidelobe, 0.99990f, 0.20f);
    EXPECT_NEAR(rightFeatures.activeFrameFraction, 0.52108f, 0.02f);
    EXPECT_NEAR(rightFeatures.rms, 0.15244f, 0.01f);
    EXPECT_NEAR(rightFeatures.bandIldDb[0], -2.55719f, 0.08f);
    EXPECT_NEAR(rightFeatures.bandCoherence[0], 0.99974f, 0.02f);
    EXPECT_NEAR(rightFeatures.leftSpectralShape[0], 0.0014035f, 0.0002f);
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
    StereoDirectionFeatures features = CalibrationFeatures();
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
    StereoDirectionFeatures features = CalibrationFeatures();
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
    StereoDirectionFeatures features = CalibrationFeatures();
    controller.ArmNext();
    ASSERT_TRUE(controller.AcceptArmedSample(SoundClass::Footstep, features));
    AudioProfile changed = original;
    changed.leftRightIsolationPercent = 50.0f;
    EXPECT_TRUE(controller.ProfileSnapshot().Matches(original));
    EXPECT_FALSE(controller.ProfileSnapshot().Matches(changed));
}

TEST(CalibrationController, RejectsLowQualitySampleBeforeProfile) {
    CalibrationController controller;
    AudioProfile audio;
    controller.Begin(CalibrationController::Mode::Quick, audio);
    controller.ArmNext();
    StereoDirectionFeatures lowQuality;
    lowQuality.rms = 0.1f;
    lowQuality.peakToNoiseDb = 2.0f;
    EXPECT_FALSE(controller.AcceptArmedSample(SoundClass::Footstep, lowQuality));
    EXPECT_EQ(controller.ProfileSnapshot().SampleCount(), 0u);
    EXPECT_NE(controller.Snapshot().lastMessage.find("rejected"), std::string::npos);
}

TEST(StereoDirectionEstimator, DistantCalibrationPrototypeCannotForceGuess) {
    StereoDirectionEstimator estimator;
    const auto stereo = DirectionFixture(7);
    StereoDirectionFeatures query;
    ASSERT_TRUE(estimator.ExtractFeatures(stereo, query));
    StereoDirectionFeatures distant = query;
    distant.broadbandIldDb += 80.0f;
    distant.gccDelaySamples -= 25.0f;
    for (float& value : distant.bandIldDb) value += 60.0f;
    AudioProfile audio;
    DirectionCalibrationProfile calibration;
    calibration.SetAudioProfileKey(audio.StableKey());
    calibration.AddSample({SoundClass::Footstep, 225.0f, distant});
    LocalizationTuning tuning;
    const auto result = estimator.EstimateFeatures(
        1, SoundClass::Footstep, query, audio, tuning, &calibration);
    EXPECT_EQ(result.profileSource, DirectionProfileSource::Synthetic);
}

TEST(StereoDirectionEstimator, PeakSelectionChoosesStrongestTransient) {
    StereoDirectionEstimator estimator;
    std::vector<float> broad(16000 * 2, 0.0005f);
    for (size_t offset = 0; offset < 240; ++offset) {
        const float weak = 0.08f * std::sin(static_cast<float>(offset) * 0.31f);
        broad[(2800 + offset) * 2] = weak;
        broad[(2800 + offset) * 2 + 1] = weak;
        const float strong = 0.55f * std::sin(static_cast<float>(offset) * 0.29f);
        broad[(9200 + offset) * 2] = strong;
        broad[(9200 + offset) * 2 + 1] = strong;
    }
    LocalizationTuning tuning;
    PeakWindowSelection selection;
    std::vector<float> selected;
    ASSERT_TRUE(estimator.SelectPeakWindow(
        broad, tuning.footstepPeak, selection, selected));
    EXPECT_NEAR(selection.peakFrame, 9320u, 180u);
    EXPECT_GT(selection.peakToNoiseDb, 20.0f);
    EXPECT_EQ(selected.size() / 2, selection.endFrame - selection.startFrame);
}

TEST(StereoDirectionEstimator, PeakSelectionRejectsNonPeakyLowSnrAudio) {
    StereoDirectionEstimator estimator;
    std::vector<float> broad(12000 * 2);
    for (size_t frame = 0; frame < broad.size() / 2; ++frame) {
        const float value = 0.03f * std::sin(static_cast<float>(frame) * 0.13f);
        broad[frame * 2] = value;
        broad[frame * 2 + 1] = value;
    }
    LocalizationTuning tuning;
    PeakWindowSelection selection;
    std::vector<float> selected;
    EXPECT_FALSE(estimator.SelectPeakWindow(
        broad, tuning.footstepPeak, selection, selected));
    EXPECT_FALSE(selection.accepted);
}

TEST(DirectionCalibrationProfile, RejectsLegacyFeatureSchema) {
    const auto root = std::filesystem::temp_directory_path() /
        "echoradar-direction-calibration-legacy-test";
    std::filesystem::create_directories(root);
    const auto path = root / "legacy.tsv";
    {
        std::ofstream output(path);
        output << "echoradar-direction-calibration-v1\nprofile\t\"legacy\"\n";
    }
    DirectionCalibrationProfile profile;
    std::string error;
    EXPECT_FALSE(profile.Load(path, &error));
    EXPECT_NE(error.find("recalibration"), std::string::npos);
    std::filesystem::remove_all(root);
}
