#include <gtest/gtest.h>

#include <settings/AppSettings.h>

#include <filesystem>

using namespace EchoRadar;

TEST(AppSettings, RoundTripPreservesAudioLocalizationAndOverlaySettings) {
    const auto root = std::filesystem::temp_directory_path() / "echoradar-settings-test";
    std::filesystem::remove_all(root);
    const auto path = root / "settings.json";

    AppSettings source;
    source.audioProfile.name = "My CS2 profile";
    source.audioProfile.eqProfile = HeadphoneEqProfile::Crisp;
    source.audioProfile.leftRightIsolationPercent = 67.0f;
    source.audioProfile.perspectiveCorrection = false;
    source.localization.localizeGunshots = true;
    source.localization.showSecondaryDirection = true;
    source.localization.sampleWindowMs = 320;
    source.overlay.visibility = OverlaySettings::Visibility::Always;
    source.overlay.radiusPixels = 144.0f;
    source.uiScale = 1.5f;

    std::string error;
    ASSERT_TRUE(AppSettingsFile::Save(path, source, &error)) << error;
    AppSettings loaded;
    ASSERT_TRUE(AppSettingsFile::Load(path, loaded, &error)) << error;
    EXPECT_EQ(loaded.audioProfile.name, "My CS2 profile");
    EXPECT_EQ(loaded.audioProfile.eqProfile, HeadphoneEqProfile::Crisp);
    EXPECT_FLOAT_EQ(loaded.audioProfile.leftRightIsolationPercent, 67.0f);
    EXPECT_FALSE(loaded.audioProfile.perspectiveCorrection);
    EXPECT_TRUE(loaded.localization.localizeGunshots);
    EXPECT_TRUE(loaded.localization.showSecondaryDirection);
    EXPECT_EQ(loaded.localization.sampleWindowMs, 320u);
    EXPECT_EQ(loaded.overlay.visibility, OverlaySettings::Visibility::Always);
    EXPECT_FLOAT_EQ(loaded.overlay.radiusPixels, 144.0f);
    EXPECT_FLOAT_EQ(loaded.uiScale, 1.5f);
    std::filesystem::remove_all(root);
}

TEST(AppSettings, ClampsUnsafeOrIncompatibleValues) {
    AppSettings settings;
    settings.localization.sampleWindowMs = 1;
    settings.localization.preOnsetMs = 900;
    settings.localization.minimumConfidence = -2.0f;
    settings.overlay.opacity = 8.0f;
    settings.uiScale = 9.0f;
    settings = AppSettings::Clamp(settings);
    EXPECT_EQ(settings.localization.sampleWindowMs, 100u);
    EXPECT_EQ(settings.localization.preOnsetMs, 80u);
    EXPECT_FLOAT_EQ(settings.localization.minimumConfidence, 0.01f);
    EXPECT_FLOAT_EQ(settings.overlay.opacity, 1.0f);
    EXPECT_FLOAT_EQ(settings.uiScale, AppSettings::kMaxUiScale);

    settings.uiScale = 2.86985925e-42f;
    settings = AppSettings::Clamp(settings);
    EXPECT_FLOAT_EQ(settings.uiScale, AppSettings::kDefaultUiScale);
}
