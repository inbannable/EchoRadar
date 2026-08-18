#include <gtest/gtest.h>

#include <settings/AppSettings.h>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace EchoRadar;

namespace {

std::filesystem::path TestRoot(const char* name) {
    const auto root = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

} // namespace

TEST(AppSettings, RoundTripPreservesRuntimeSettings) {
    const auto root = TestRoot("echoradar-settings-test");
    const auto path = root / "settings.json";

    AppSettings source;
    source.audioProfile.name = "My CS2 profile";
    source.audioProfile.eqProfile = HeadphoneEqProfile::Crisp;
    source.audioProfile.leftRightIsolationPercent = 67.0f;
    source.audioProfile.perspectiveCorrection = false;
    source.audioProfile.spatialEnhancement = SpatialEnhancementState::Off;
    source.audioProfile.outputEndpointId = "ma:1234";
    source.direction.enableFootsteps = false;
    source.direction.enableGunshots = true;
    source.overlay.visibility = OverlaySettings::Visibility::Always;
    source.overlay.radiusPixels = 144.0f;
    source.uiScale = 1.5f;
    source.sessionLogging = false;

    std::string error;
    ASSERT_TRUE(AppSettingsFile::Save(path, source, &error)) << error;
    AppSettings loaded;
    ASSERT_TRUE(AppSettingsFile::Load(path, loaded, &error)) << error;
    EXPECT_EQ(loaded.schemaVersion, AppSettings::kSchemaVersion);
    EXPECT_EQ(loaded.audioProfile.name, "My CS2 profile");
    EXPECT_EQ(loaded.audioProfile.eqProfile, HeadphoneEqProfile::Crisp);
    EXPECT_FLOAT_EQ(loaded.audioProfile.leftRightIsolationPercent, 67.0f);
    EXPECT_FALSE(loaded.audioProfile.perspectiveCorrection);
    EXPECT_EQ(loaded.audioProfile.spatialEnhancement, SpatialEnhancementState::Off);
    EXPECT_EQ(loaded.audioProfile.outputEndpointId, "ma:1234");
    EXPECT_FALSE(loaded.direction.enableFootsteps);
    EXPECT_TRUE(loaded.direction.enableGunshots);
    EXPECT_EQ(loaded.overlay.visibility, OverlaySettings::Visibility::Always);
    EXPECT_FLOAT_EQ(loaded.overlay.radiusPixels, 144.0f);
    EXPECT_FLOAT_EQ(loaded.uiScale, 1.5f);
    EXPECT_FALSE(loaded.sessionLogging);

    const std::string saved = ReadText(path);
    EXPECT_NE(saved.find("\"schema_version\": 3"), std::string::npos);
    EXPECT_EQ(saved.find("localization_"), std::string::npos);
    EXPECT_EQ(saved.find("calibration"), std::string::npos);
    std::filesystem::remove_all(root);
}

TEST(AppSettings, MigratesSchemaTwoAndLeavesCalibrationDataUntouched) {
    const auto root = TestRoot("echoradar-settings-v2-migration-test");
    const auto path = root / "settings.json";
    const auto calibration = root / "direction-calibration.tsv";
    {
        std::ofstream output(path);
        output << "{\"schema_version\":2,\"audio_profile_name\":\"Tournament\","
                  "\"eq_profile\":\"smooth\",\"localize_footsteps\":false,"
                  "\"localize_gunshots\":true,\"localization_sample_ms\":999,"
                  "\"overlay_visibility\":\"always\",\"ui_scale\":1.4}";
    }
    {
        std::ofstream output(calibration);
        output << "sentinel calibration data\n";
    }

    AppSettings loaded;
    std::string error;
    ASSERT_TRUE(AppSettingsFile::Load(path, loaded, &error)) << error;
    EXPECT_EQ(loaded.schemaVersion, AppSettings::kSchemaVersion);
    EXPECT_EQ(loaded.audioProfile.name, "Tournament");
    EXPECT_EQ(loaded.audioProfile.eqProfile, HeadphoneEqProfile::Smooth);
    EXPECT_FALSE(loaded.direction.enableFootsteps);
    EXPECT_TRUE(loaded.direction.enableGunshots);
    EXPECT_EQ(loaded.overlay.visibility, OverlaySettings::Visibility::Always);
    EXPECT_FLOAT_EQ(loaded.uiScale, 1.4f);

    ASSERT_TRUE(AppSettingsFile::Save(path, loaded, &error)) << error;
    EXPECT_EQ(ReadText(calibration), "sentinel calibration data\n");
    const std::string migrated = ReadText(path);
    EXPECT_NE(migrated.find("\"schema_version\": 3"), std::string::npos);
    EXPECT_EQ(migrated.find("localization_sample_ms"), std::string::npos);
    std::filesystem::remove_all(root);
}

TEST(AppSettings, ClampsUnsafeValues) {
    AppSettings settings;
    EXPECT_TRUE(settings.direction.enableFootsteps);
    EXPECT_TRUE(settings.direction.enableGunshots);
    settings.audioProfile.leftRightIsolationPercent = 900.0f;
    settings.audioProfile.displayAspectRatio = -1.0f;
    settings.overlay.opacity = 8.0f;
    settings.uiScale = 9.0f;
    settings = AppSettings::Clamp(settings);
    EXPECT_FLOAT_EQ(settings.audioProfile.leftRightIsolationPercent, 100.0f);
    EXPECT_FLOAT_EQ(settings.audioProfile.displayAspectRatio, 1.0f);
    EXPECT_FLOAT_EQ(settings.overlay.opacity, 1.0f);
    EXPECT_FLOAT_EQ(settings.uiScale, AppSettings::kMaxUiScale);

    settings.uiScale = 2.86985925e-42f;
    settings = AppSettings::Clamp(settings);
    EXPECT_FLOAT_EQ(settings.uiScale, AppSettings::kDefaultUiScale);
}
