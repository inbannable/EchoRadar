#include "AppSettings.h"

#include <dataset/DatasetJson.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace EchoRadar {
namespace {

HeadphoneEqProfile ParseEq(const std::string& value) {
    if (value == "crisp") return HeadphoneEqProfile::Crisp;
    if (value == "smooth") return HeadphoneEqProfile::Smooth;
    return HeadphoneEqProfile::Natural;
}

SpatialEnhancementState ParseEnhancement(const std::string& value) {
    if (value == "off") return SpatialEnhancementState::Off;
    if (value == "on") return SpatialEnhancementState::On;
    return SpatialEnhancementState::Unknown;
}

OverlaySettings::Visibility ParseVisibility(const std::string& value) {
    if (value == "off") return OverlaySettings::Visibility::Off;
    if (value == "always") return OverlaySettings::Visibility::Always;
    return OverlaySettings::Visibility::Cs2Only;
}

const char* VisibilityName(OverlaySettings::Visibility value) {
    switch (value) {
    case OverlaySettings::Visibility::Off: return "off";
    case OverlaySettings::Visibility::Cs2Only: return "cs2-only";
    case OverlaySettings::Visibility::Always: return "always";
    }
    return "cs2-only";
}

} // namespace

AppSettings AppSettings::Clamp(AppSettings settings) {
    settings.schemaVersion = kSchemaVersion;
    settings.audioProfile.leftRightIsolationPercent = std::clamp(
        settings.audioProfile.leftRightIsolationPercent, 0.0f, 100.0f);
    settings.audioProfile.displayAspectRatio = std::clamp(
        settings.audioProfile.displayAspectRatio, 1.0f, 4.0f);
    settings.localization.sampleWindowMs = std::clamp(
        settings.localization.sampleWindowMs, 100u, 600u);
    settings.localization.preOnsetMs = std::min(
        settings.localization.preOnsetMs, settings.localization.sampleWindowMs - 20u);
    settings.localization.minimumConfidence = std::clamp(
        settings.localization.minimumConfidence, 0.01f, 1.0f);
    settings.localization.secondaryRatio = std::clamp(
        settings.localization.secondaryRatio, 0.25f, 1.0f);
    settings.localization.secondaryMinimumSeparationDegrees = std::clamp(
        settings.localization.secondaryMinimumSeparationDegrees, 15.0f, 180.0f);
    settings.overlay.radiusPixels = std::clamp(settings.overlay.radiusPixels, 40.0f, 400.0f);
    settings.overlay.thicknessPixels = std::clamp(settings.overlay.thicknessPixels, 2.0f, 32.0f);
    settings.overlay.opacity = std::clamp(settings.overlay.opacity, 0.05f, 1.0f);
    settings.overlay.offsetX = std::clamp(settings.overlay.offsetX, -2000.0f, 2000.0f);
    settings.overlay.offsetY = std::clamp(settings.overlay.offsetY, -2000.0f, 2000.0f);
    settings.overlay.footstepLifetimeSeconds = std::clamp(
        settings.overlay.footstepLifetimeSeconds, 0.1f, 10.0f);
    settings.overlay.gunshotLifetimeSeconds = std::clamp(
        settings.overlay.gunshotLifetimeSeconds, 0.1f, 10.0f);
    return settings;
}

std::filesystem::path AppSettingsFile::DefaultPath() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        buffer.resize(length);
        return std::filesystem::path(buffer) / "EchoRadar" / "settings.json";
    }
#else
    if (const char* config = std::getenv("XDG_CONFIG_HOME")) {
        if (*config != '\0') return std::filesystem::path(config) / "EchoRadar" / "settings.json";
    }
#endif
    return std::filesystem::current_path() / ".echoradar" / "settings.json";
}

bool AppSettingsFile::Load(const std::filesystem::path& path,
                           AppSettings& settings,
                           std::string* error) {
    const std::string text = detail::ReadFileToString(path.string());
    if (text.empty()) {
        settings = AppSettings{};
        if (error) *error = "Settings file is unavailable; defaults are active";
        return false;
    }
    const auto values = detail::ParseFlatJson(text);
    const uint64_t schema = detail::GetU64(values, "schema_version", 0);
    if (schema == 0 || schema > AppSettings::kSchemaVersion) {
        settings = AppSettings{};
        if (error) *error = "Settings schema is incompatible; defaults are active";
        return false;
    }

    AppSettings loaded;
    loaded.audioProfile.name = detail::GetStr(values, "audio_profile_name", "Default");
    loaded.audioProfile.eqProfile = ParseEq(detail::GetStr(values, "eq_profile", "natural"));
    loaded.audioProfile.leftRightIsolationPercent = detail::GetFloatVal(
        values, "lr_isolation_percent", 0.0f);
    loaded.audioProfile.perspectiveCorrection = detail::GetBoolVal(
        values, "perspective_correction", true);
    loaded.audioProfile.displayAspectRatio = detail::GetFloatVal(
        values, "display_aspect_ratio", 16.0f / 9.0f);
    loaded.audioProfile.spatialEnhancement = ParseEnhancement(
        detail::GetStr(values, "spatial_enhancement", "unknown"));
    loaded.audioProfile.outputEndpointId = detail::GetStr(values, "output_endpoint_id");

    loaded.localization.localizeFootsteps = detail::GetBoolVal(
        values, "localize_footsteps", true);
    loaded.localization.localizeGunshots = detail::GetBoolVal(
        values, "localize_gunshots", true);
    loaded.localization.sampleWindowMs = static_cast<uint32_t>(detail::GetU64(
        values, "localization_sample_ms", 240));
    loaded.localization.preOnsetMs = static_cast<uint32_t>(detail::GetU64(
        values, "localization_pre_onset_ms", 40));
    loaded.localization.minimumConfidence = detail::GetFloatVal(
        values, "localization_min_confidence", 0.35f);
    loaded.localization.showSecondaryDirection = detail::GetBoolVal(
        values, "show_secondary_direction", false);
    loaded.localization.secondaryRatio = detail::GetFloatVal(
        values, "secondary_ratio", 0.75f);
    loaded.localization.secondaryMinimumSeparationDegrees = detail::GetFloatVal(
        values, "secondary_min_separation_degrees", 60.0f);

    loaded.overlay.visibility = ParseVisibility(
        detail::GetStr(values, "overlay_visibility", "cs2-only"));
    loaded.overlay.radiusPixels = detail::GetFloatVal(values, "overlay_radius_px", 110.0f);
    loaded.overlay.thicknessPixels = detail::GetFloatVal(values, "overlay_thickness_px", 8.0f);
    loaded.overlay.opacity = detail::GetFloatVal(values, "overlay_opacity", 0.90f);
    loaded.overlay.offsetX = detail::GetFloatVal(values, "overlay_offset_x", 0.0f);
    loaded.overlay.offsetY = detail::GetFloatVal(values, "overlay_offset_y", 0.0f);
    loaded.overlay.footstepLifetimeSeconds = detail::GetFloatVal(
        values, "overlay_footstep_lifetime_s", 1.2f);
    loaded.overlay.gunshotLifetimeSeconds = detail::GetFloatVal(
        values, "overlay_gunshot_lifetime_s", 0.8f);
    loaded.overlay.showCenterDot = detail::GetBoolVal(values, "overlay_center_dot", false);
    loaded.sessionLogging = detail::GetBoolVal(values, "session_logging", true);
    settings = AppSettings::Clamp(std::move(loaded));
    if (error) error->clear();
    return true;
}

bool AppSettingsFile::Save(const std::filesystem::path& path,
                           const AppSettings& settings,
                           std::string* error) {
    const AppSettings safe = AppSettings::Clamp(settings);
    std::error_code filesystemError;
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if (filesystemError) {
        if (error) *error = "Could not create settings directory";
        return false;
    }
    const std::filesystem::path temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        if (error) *error = "Could not open settings file for writing";
        return false;
    }
    const auto quoted = [&](const std::string& value) {
        return '"' + detail::JsonEscapeStr(value) + '"';
    };
    output << std::setprecision(9)
           << "{\n"
           << "  \"schema_version\": " << AppSettings::kSchemaVersion << ",\n"
           << "  \"audio_profile_name\": " << quoted(safe.audioProfile.name) << ",\n"
           << "  \"eq_profile\": " << quoted(ToString(safe.audioProfile.eqProfile)) << ",\n"
           << "  \"lr_isolation_percent\": " << safe.audioProfile.leftRightIsolationPercent << ",\n"
           << "  \"perspective_correction\": " << (safe.audioProfile.perspectiveCorrection ? "true" : "false") << ",\n"
           << "  \"display_aspect_ratio\": " << safe.audioProfile.displayAspectRatio << ",\n"
           << "  \"spatial_enhancement\": " << quoted(ToString(safe.audioProfile.spatialEnhancement)) << ",\n"
           << "  \"output_endpoint_id\": " << quoted(safe.audioProfile.outputEndpointId) << ",\n"
           << "  \"localize_footsteps\": " << (safe.localization.localizeFootsteps ? "true" : "false") << ",\n"
           << "  \"localize_gunshots\": " << (safe.localization.localizeGunshots ? "true" : "false") << ",\n"
           << "  \"localization_sample_ms\": " << safe.localization.sampleWindowMs << ",\n"
           << "  \"localization_pre_onset_ms\": " << safe.localization.preOnsetMs << ",\n"
           << "  \"localization_min_confidence\": " << safe.localization.minimumConfidence << ",\n"
           << "  \"show_secondary_direction\": " << (safe.localization.showSecondaryDirection ? "true" : "false") << ",\n"
           << "  \"secondary_ratio\": " << safe.localization.secondaryRatio << ",\n"
           << "  \"secondary_min_separation_degrees\": " << safe.localization.secondaryMinimumSeparationDegrees << ",\n"
           << "  \"overlay_visibility\": " << quoted(VisibilityName(safe.overlay.visibility)) << ",\n"
           << "  \"overlay_radius_px\": " << safe.overlay.radiusPixels << ",\n"
           << "  \"overlay_thickness_px\": " << safe.overlay.thicknessPixels << ",\n"
           << "  \"overlay_opacity\": " << safe.overlay.opacity << ",\n"
           << "  \"overlay_offset_x\": " << safe.overlay.offsetX << ",\n"
           << "  \"overlay_offset_y\": " << safe.overlay.offsetY << ",\n"
           << "  \"overlay_footstep_lifetime_s\": " << safe.overlay.footstepLifetimeSeconds << ",\n"
           << "  \"overlay_gunshot_lifetime_s\": " << safe.overlay.gunshotLifetimeSeconds << ",\n"
           << "  \"overlay_center_dot\": " << (safe.overlay.showCenterDot ? "true" : "false") << ",\n"
           << "  \"session_logging\": " << (safe.sessionLogging ? "true" : "false") << "\n"
           << "}\n";
    output.flush();
    if (!output) {
        if (error) *error = "Could not finish writing settings file";
        return false;
    }
    output.close();
#ifdef _WIN32
    if (!MoveFileExW(temporary.wstring().c_str(), path.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (error) *error = "Could not atomically publish settings file";
        return false;
    }
#else
    std::filesystem::rename(temporary, path, filesystemError);
    if (filesystemError) {
        if (error) *error = "Could not atomically publish settings file";
        return false;
    }
#endif
    if (error) error->clear();
    return true;
}

RuntimeSettingsStore::RuntimeSettingsStore(std::filesystem::path path)
    : m_path(std::move(path)) {}

bool RuntimeSettingsStore::Load(std::string* error) {
    AppSettings loaded;
    const bool success = AppSettingsFile::Load(m_path, loaded, error);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_settings = AppSettings::Clamp(std::move(loaded));
    return success;
}

bool RuntimeSettingsStore::Save(std::string* error) const {
    return AppSettingsFile::Save(m_path, Snapshot(), error);
}

AppSettings RuntimeSettingsStore::Snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_settings;
}

bool RuntimeSettingsStore::Update(const AppSettings& settings, bool persist,
                                  std::string* error) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_settings = AppSettings::Clamp(settings);
    }
    return !persist || Save(error);
}

void RuntimeSettingsStore::Reset(bool persist) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_settings = {};
    }
    if (persist) Save(nullptr);
}

} // namespace EchoRadar
