#include "AppSettings.h"

#include <support/FlatJson.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace EchoRadar {
namespace {

constexpr float kMinimumHudScale = 0.25f;
constexpr float kMaximumHudScale = 4.0f;
constexpr float kMaximumDesktopCoordinate = 32768.0f;

template <typename Value>
Value ClampFinite(Value value, Value minimum, Value maximum, Value fallback) {
    if (!std::isfinite(value)) return fallback;
    return std::clamp(value, minimum, maximum);
}

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

RadarMode ParseRadarMode(const std::string& value) {
    if (value == "events") return RadarMode::Events;
    if (value == "combined") return RadarMode::Combined;
    return RadarMode::Continuous;
}

const char* RadarModeName(RadarMode value) {
    switch (value) {
    case RadarMode::Continuous: return "continuous";
    case RadarMode::Events: return "events";
    case RadarMode::Combined: return "combined";
    }
    return "continuous";
}

RadarPreset ParseRadarPreset(const std::string& value) {
    if (value == "footsteps") return RadarPreset::Footsteps;
    if (value == "gunshots") return RadarPreset::Gunshots;
    if (value == "custom") return RadarPreset::Custom;
    return RadarPreset::All;
}

const char* RadarPresetName(RadarPreset value) {
    switch (value) {
    case RadarPreset::All: return "all";
    case RadarPreset::Footsteps: return "footsteps";
    case RadarPreset::Gunshots: return "gunshots";
    case RadarPreset::Custom: return "custom";
    }
    return "all";
}

SetupStep ParseSetupStep(const std::string& value) {
    if (value == "validate-format") return SetupStep::ValidateFormat;
    if (value == "confirm-channels") return SetupStep::ConfirmChannels;
    if (value == "preview-hud") return SetupStep::PreviewHud;
    if (value == "complete") return SetupStep::Complete;
    return SetupStep::SelectEndpoint;
}

const char* SetupStepName(SetupStep value) {
    switch (value) {
    case SetupStep::SelectEndpoint: return "select-endpoint";
    case SetupStep::ValidateFormat: return "validate-format";
    case SetupStep::ConfirmChannels: return "confirm-channels";
    case SetupStep::PreviewHud: return "preview-hud";
    case SetupStep::Complete: return "complete";
    }
    return "select-endpoint";
}

SetupSupportState ParseSetupSupport(const std::string& value) {
    if (value == "headphone-stereo") return SetupSupportState::HeadphoneStereo;
    if (value == "supported") return SetupSupportState::Supported;
    if (value == "no-endpoint") return SetupSupportState::NoEndpoint;
    if (value == "unsupported-stereo") {
        return SetupSupportState::UnsupportedStereo;
    }
    if (value == "unsupported-sample-rate") {
        return SetupSupportState::UnsupportedSampleRate;
    }
    if (value == "unsupported-channel-mask") {
        return SetupSupportState::UnsupportedChannelMask;
    }
    if (value == "capture-failure") return SetupSupportState::CaptureFailure;
    return SetupSupportState::Unknown;
}

const char* SetupSupportName(SetupSupportState value) {
    switch (value) {
    case SetupSupportState::Unknown: return "unknown";
    case SetupSupportState::Supported: return "supported";
    case SetupSupportState::HeadphoneStereo: return "headphone-stereo";
    case SetupSupportState::NoEndpoint: return "no-endpoint";
    case SetupSupportState::UnsupportedStereo: return "unsupported-stereo";
    case SetupSupportState::UnsupportedSampleRate:
        return "unsupported-sample-rate";
    case SetupSupportState::UnsupportedChannelMask:
        return "unsupported-channel-mask";
    case SetupSupportState::CaptureFailure: return "capture-failure";
    }
    return "unknown";
}

HudAnchor ParseHudAnchor(const std::string& value) {
    if (value == "top-left") return HudAnchor::TopLeft;
    if (value == "top-center") return HudAnchor::TopCenter;
    if (value == "top-right") return HudAnchor::TopRight;
    if (value == "center-left") return HudAnchor::CenterLeft;
    if (value == "center-right") return HudAnchor::CenterRight;
    if (value == "bottom-left") return HudAnchor::BottomLeft;
    if (value == "bottom-center") return HudAnchor::BottomCenter;
    if (value == "bottom-right") return HudAnchor::BottomRight;
    return HudAnchor::Center;
}

const char* HudAnchorName(HudAnchor value) {
    switch (value) {
    case HudAnchor::TopLeft: return "top-left";
    case HudAnchor::TopCenter: return "top-center";
    case HudAnchor::TopRight: return "top-right";
    case HudAnchor::CenterLeft: return "center-left";
    case HudAnchor::Center: return "center";
    case HudAnchor::CenterRight: return "center-right";
    case HudAnchor::BottomLeft: return "bottom-left";
    case HudAnchor::BottomCenter: return "bottom-center";
    case HudAnchor::BottomRight: return "bottom-right";
    }
    return "center";
}

HudPreviewBackground ParsePreviewBackground(const std::string& value) {
    if (value == "transparent") return HudPreviewBackground::Transparent;
    if (value == "light") return HudPreviewBackground::Light;
    if (value == "checkerboard") return HudPreviewBackground::Checkerboard;
    return HudPreviewBackground::TacticalDark;
}

const char* PreviewBackgroundName(HudPreviewBackground value) {
    switch (value) {
    case HudPreviewBackground::Transparent: return "transparent";
    case HudPreviewBackground::TacticalDark: return "tactical-dark";
    case HudPreviewBackground::Light: return "light";
    case HudPreviewBackground::Checkerboard: return "checkerboard";
    }
    return "tactical-dark";
}

std::string HudKey(size_t index, const char* suffix) {
    return "hud_" + std::to_string(index) + "_" + suffix;
}

HudColor LoadColor(const std::map<std::string, std::string>& values,
                   size_t index,
                   const char* name,
                   HudColor fallback) {
    const std::string prefix = HudKey(index, name);
    fallback.red = detail::GetFloatVal(values, prefix + "_r", fallback.red);
    fallback.green = detail::GetFloatVal(values, prefix + "_g", fallback.green);
    fallback.blue = detail::GetFloatVal(values, prefix + "_b", fallback.blue);
    fallback.alpha = detail::GetFloatVal(values, prefix + "_a", fallback.alpha);
    return fallback;
}

HudColor ClampColor(HudColor color, HudColor fallback) {
    color.red = ClampFinite(color.red, 0.0f, 1.0f, fallback.red);
    color.green = ClampFinite(color.green, 0.0f, 1.0f, fallback.green);
    color.blue = ClampFinite(color.blue, 0.0f, 1.0f, fallback.blue);
    color.alpha = ClampFinite(color.alpha, 0.0f, 1.0f, fallback.alpha);
    return color;
}

bool IsValid(HudAnchor value) {
    return value >= HudAnchor::TopLeft && value <= HudAnchor::BottomRight;
}

HudDisplaySettings ClampHud(HudDisplaySettings hud) {
    const HudDisplaySettings defaults;
    if (hud.displayId.size() > 256) hud.displayId.resize(256);
    hud.scale = ClampFinite(
        hud.scale, kMinimumHudScale, kMaximumHudScale, defaults.scale);
    hud.opacity = ClampFinite(hud.opacity, 0.05f, 1.0f, defaults.opacity);
    if (!IsValid(hud.anchor)) hud.anchor = defaults.anchor;
    hud.offsetX = ClampFinite(
        hud.offsetX, -kMaximumDesktopCoordinate, kMaximumDesktopCoordinate, 0.0f);
    hud.offsetY = ClampFinite(
        hud.offsetY, -kMaximumDesktopCoordinate, kMaximumDesktopCoordinate, 0.0f);
    hud.sectorColor = ClampColor(hud.sectorColor, defaults.sectorColor);
    hud.strongestColor = ClampColor(
        hud.strongestColor, defaults.strongestColor);
    hud.inactiveColor = ClampColor(hud.inactiveColor, defaults.inactiveColor);
    hud.errorColor = ClampColor(hud.errorColor, defaults.errorColor);
    hud.persistenceSeconds = ClampFinite(
        hud.persistenceSeconds, 0.0f, 30.0f, defaults.persistenceSeconds);
    if (hud.previewBackground < HudPreviewBackground::Transparent ||
        hud.previewBackground > HudPreviewBackground::Checkerboard) {
        hud.previewBackground = defaults.previewBackground;
    }
    return hud;
}

void NormalizeOnboarding(OnboardingSettings& setup) {
    if (setup.completed || setup.step == SetupStep::Complete) {
        setup.endpointSelected = true;
        setup.formatValidated = true;
        setup.channelsConfirmed = true;
        setup.hudPreviewed = true;
        setup.completed = true;
        setup.step = SetupStep::Complete;
        if (setup.supportState != SetupSupportState::HeadphoneStereo)
            setup.supportState = SetupSupportState::Supported;
        return;
    }
    if (!setup.endpointSelected) {
        setup.step = SetupStep::SelectEndpoint;
        setup.formatValidated = false;
        setup.channelsConfirmed = false;
        setup.hudPreviewed = false;
        return;
    }
    if (!setup.formatValidated) {
        setup.step = SetupStep::ValidateFormat;
        setup.channelsConfirmed = false;
        setup.hudPreviewed = false;
        return;
    }
    if (!setup.channelsConfirmed) {
        setup.step = SetupStep::ConfirmChannels;
        setup.hudPreviewed = false;
        return;
    }
    setup.step = SetupStep::PreviewHud;
}

void MigrateLegacyOverlayToHud(AppSettings& settings) {
    HudDisplaySettings hud;
    hud.scale = settings.overlay.radiusPixels / 110.0f;
    hud.opacity = settings.overlay.opacity;
    hud.offsetX = settings.overlay.offsetX;
    hud.offsetY = settings.overlay.offsetY;
    hud.persistenceSeconds = std::max(
        settings.overlay.footstepLifetimeSeconds,
        settings.overlay.gunshotLifetimeSeconds);
    hud.visible = settings.overlay.visibility != OverlaySettings::Visibility::Off;
    settings.hudDisplays.assign(1, std::move(hud));
}

} // namespace

AppSettings AppSettings::Clamp(AppSettings settings) {
    settings.schemaVersion = kSchemaVersion;
    settings.audioProfile.leftRightIsolationPercent = ClampFinite(
        settings.audioProfile.leftRightIsolationPercent, 0.0f, 100.0f, 0.0f);
    settings.audioProfile.displayAspectRatio = ClampFinite(
        settings.audioProfile.displayAspectRatio, 1.0f, 4.0f, 16.0f / 9.0f);

    settings.overlay.radiusPixels = ClampFinite(
        settings.overlay.radiusPixels, 40.0f, 400.0f, 110.0f);
    settings.overlay.thicknessPixels = ClampFinite(
        settings.overlay.thicknessPixels, 2.0f, 32.0f, 8.0f);
    settings.overlay.opacity = ClampFinite(
        settings.overlay.opacity, 0.05f, 1.0f, 0.90f);
    settings.overlay.offsetX = ClampFinite(
        settings.overlay.offsetX, -2000.0f, 2000.0f, 0.0f);
    settings.overlay.offsetY = ClampFinite(
        settings.overlay.offsetY, -2000.0f, 2000.0f, 0.0f);
    settings.overlay.footstepLifetimeSeconds = ClampFinite(
        settings.overlay.footstepLifetimeSeconds, 0.1f, 10.0f, 1.2f);
    settings.overlay.gunshotLifetimeSeconds = ClampFinite(
        settings.overlay.gunshotLifetimeSeconds, 0.1f, 10.0f, 0.8f);

    if (settings.radar.mode < RadarMode::Continuous ||
        settings.radar.mode > RadarMode::Combined) {
        settings.radar.mode = RadarMode::Continuous;
    }
    if (settings.radar.preset < RadarPreset::All ||
        settings.radar.preset > RadarPreset::Custom) {
        settings.radar.preset = RadarPreset::All;
    }
    settings.radar.sensitivityDbfs = ClampFinite(
        settings.radar.sensitivityDbfs, -96.0f, 0.0f, -48.0f);
    settings.radar.attackMilliseconds = std::min(
        settings.radar.attackMilliseconds, 2000u);
    settings.radar.releaseMilliseconds = std::min(
        settings.radar.releaseMilliseconds, 5000u);
    settings.radar.holdMilliseconds = std::min(
        settings.radar.holdMilliseconds, 2000u);
    for (float& point : settings.radar.customCurveDb) {
        point = ClampFinite(point, -24.0f, 24.0f, 0.0f);
    }

    settings.dashboard.windowX = ClampFinite(
        settings.dashboard.windowX, -kMaximumDesktopCoordinate,
        kMaximumDesktopCoordinate, 100.0f);
    settings.dashboard.windowY = ClampFinite(
        settings.dashboard.windowY, -kMaximumDesktopCoordinate,
        kMaximumDesktopCoordinate, 100.0f);
    settings.dashboard.windowWidth = ClampFinite(
        settings.dashboard.windowWidth, 1100.0f, 7680.0f, 1280.0f);
    settings.dashboard.windowHeight = ClampFinite(
        settings.dashboard.windowHeight, 700.0f, 4320.0f, 800.0f);

    NormalizeOnboarding(settings.onboarding);
    settings.eventDisplay.maximumRecentEvents = std::clamp(
        settings.eventDisplay.maximumRecentEvents, 1u, 500u);
    settings.eventDisplay.minimumConfidence = ClampFinite(
        settings.eventDisplay.minimumConfidence, 0.0f, 1.0f, 0.0f);
    settings.eventDisplay.markerPersistenceSeconds = ClampFinite(
        settings.eventDisplay.markerPersistenceSeconds, 0.0f, 30.0f, 2.0f);

    std::vector<HudDisplaySettings> displays;
    displays.reserve(std::min(
        settings.hudDisplays.size(), AppSettings::kMaximumHudDisplays));
    for (HudDisplaySettings& candidate : settings.hudDisplays) {
        candidate = ClampHud(std::move(candidate));
        if (candidate.displayId.empty()) continue;
        const auto duplicate = std::find_if(
            displays.begin(), displays.end(), [&](const HudDisplaySettings& item) {
                return item.displayId == candidate.displayId;
            });
        if (duplicate == displays.end()) {
            displays.push_back(std::move(candidate));
        } else {
            *duplicate = std::move(candidate);
        }
        if (displays.size() == AppSettings::kMaximumHudDisplays) break;
    }
    if (displays.empty()) displays.emplace_back();
    settings.hudDisplays = std::move(displays);

    if (!std::isfinite(settings.uiScale) ||
        (settings.uiScale > 0.0f &&
         settings.uiScale < kMinUiScale * 0.5f)) {
        settings.uiScale = kDefaultUiScale;
    }
    settings.uiScale = std::clamp(
        settings.uiScale, kMinUiScale, kMaxUiScale);
    return settings;
}

const HudDisplaySettings* FindHudDisplaySettings(
    const AppSettings& settings, std::string_view displayId) {
    const auto found = std::find_if(
        settings.hudDisplays.begin(), settings.hudDisplays.end(),
        [&](const HudDisplaySettings& item) {
            return item.displayId == displayId;
        });
    return found == settings.hudDisplays.end() ? nullptr : &*found;
}

HudDisplaySettings* FindHudDisplaySettings(
    AppSettings& settings, std::string_view displayId) {
    const auto found = std::find_if(
        settings.hudDisplays.begin(), settings.hudDisplays.end(),
        [&](const HudDisplaySettings& item) {
            return item.displayId == displayId;
        });
    return found == settings.hudDisplays.end() ? nullptr : &*found;
}

bool UpdateHudDisplaySettings(AppSettings& settings,
                              std::string_view displayId,
                              HudDisplaySettings configuration) {
    if (displayId.empty()) return false;
    configuration.displayId.assign(displayId);
    configuration = ClampHud(std::move(configuration));
    if (HudDisplaySettings* existing =
            FindHudDisplaySettings(settings, displayId)) {
        *existing = std::move(configuration);
        return true;
    }
    if (settings.hudDisplays.size() >= AppSettings::kMaximumHudDisplays) {
        return false;
    }
    settings.hudDisplays.push_back(std::move(configuration));
    return true;
}

HudRect ClampHudToUsableMonitorBounds(HudRect hudBounds,
                                      HudRect usableBounds) {
    if (!std::isfinite(usableBounds.x)) usableBounds.x = 0.0f;
    if (!std::isfinite(usableBounds.y)) usableBounds.y = 0.0f;
    if (!std::isfinite(usableBounds.width) || usableBounds.width < 0.0f) {
        usableBounds.width = 0.0f;
    }
    if (!std::isfinite(usableBounds.height) || usableBounds.height < 0.0f) {
        usableBounds.height = 0.0f;
    }
    if (!std::isfinite(hudBounds.x)) hudBounds.x = usableBounds.x;
    if (!std::isfinite(hudBounds.y)) hudBounds.y = usableBounds.y;
    if (!std::isfinite(hudBounds.width) || hudBounds.width < 0.0f) {
        hudBounds.width = 0.0f;
    }
    if (!std::isfinite(hudBounds.height) || hudBounds.height < 0.0f) {
        hudBounds.height = 0.0f;
    }

    hudBounds.width = std::min(hudBounds.width, usableBounds.width);
    hudBounds.height = std::min(hudBounds.height, usableBounds.height);
    hudBounds.x = std::clamp(
        hudBounds.x, usableBounds.x,
        usableBounds.x + usableBounds.width - hudBounds.width);
    hudBounds.y = std::clamp(
        hudBounds.y, usableBounds.y,
        usableBounds.y + usableBounds.height - hudBounds.height);
    return hudBounds;
}

HudRect ResolveHudBounds(const HudDisplaySettings& settings,
                         HudRect usableBounds,
                         float unscaledWidth,
                         float unscaledHeight) {
    const HudDisplaySettings safe = ClampHud(settings);
    HudRect result;
    result.width = std::max(0.0f, unscaledWidth) * safe.scale;
    result.height = std::max(0.0f, unscaledHeight) * safe.scale;

    const bool centeredX =
        safe.anchor == HudAnchor::TopCenter ||
        safe.anchor == HudAnchor::Center ||
        safe.anchor == HudAnchor::BottomCenter;
    const bool right =
        safe.anchor == HudAnchor::TopRight ||
        safe.anchor == HudAnchor::CenterRight ||
        safe.anchor == HudAnchor::BottomRight;
    const bool centeredY =
        safe.anchor == HudAnchor::CenterLeft ||
        safe.anchor == HudAnchor::Center ||
        safe.anchor == HudAnchor::CenterRight;
    const bool bottom =
        safe.anchor == HudAnchor::BottomLeft ||
        safe.anchor == HudAnchor::BottomCenter ||
        safe.anchor == HudAnchor::BottomRight;

    result.x = right
        ? usableBounds.x + usableBounds.width - result.width
        : (centeredX
            ? usableBounds.x + (usableBounds.width - result.width) * 0.5f
            : usableBounds.x);
    result.y = bottom
        ? usableBounds.y + usableBounds.height - result.height
        : (centeredY
            ? usableBounds.y + (usableBounds.height - result.height) * 0.5f
            : usableBounds.y);
    result.x += safe.offsetX;
    result.y += safe.offsetY;
    return ClampHudToUsableMonitorBounds(result, usableBounds);
}

std::filesystem::path AppSettingsFile::DefaultPath() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        buffer.resize(length);
        return std::filesystem::path(buffer) /
               "EchoRadar" / "v2" / "settings.json";
    }
#else
    if (const char* config = std::getenv("XDG_CONFIG_HOME")) {
        if (*config != '\0') {
            return std::filesystem::path(config) /
                   "EchoRadar" / "v2" / "settings.json";
        }
    }
#endif
    return std::filesystem::current_path() /
           ".echoradar" / "v2" / "settings.json";
}

bool AppSettingsFile::Load(const std::filesystem::path& path,
                           AppSettings& settings,
                           std::string* error) {
    const std::string text = detail::ReadFileToString(path.string());
    if (text.empty()) {
        settings = {};
        if (error) {
            *error = "Settings file is unavailable; defaults are active";
        }
        return false;
    }
    const auto values = detail::ParseFlatJson(text);
    const uint64_t schema = detail::GetU64(values, "schema_version", 0);
    if (schema != 2 && schema != 3 &&
        schema != AppSettings::kSchemaVersion) {
        settings = {};
        if (error) {
            *error = "Settings schema is incompatible; defaults are active";
        }
        return false;
    }

    AppSettings loaded;
    loaded.audioProfile.name =
        detail::GetStr(values, "audio_profile_name", "Default");
    loaded.audioProfile.eqProfile =
        ParseEq(detail::GetStr(values, "eq_profile", "natural"));
    loaded.audioProfile.leftRightIsolationPercent = detail::GetFloatVal(
        values, "lr_isolation_percent", 0.0f);
    loaded.audioProfile.perspectiveCorrection = detail::GetBoolVal(
        values, "perspective_correction", true);
    loaded.audioProfile.displayAspectRatio = detail::GetFloatVal(
        values, "display_aspect_ratio", 16.0f / 9.0f);
    loaded.audioProfile.spatialEnhancement = ParseEnhancement(
        detail::GetStr(values, "spatial_enhancement", "unknown"));
    loaded.audioProfile.outputEndpointId =
        detail::GetStr(values, "output_endpoint_id");

    if (schema == 2) {
        loaded.direction.enableFootsteps = detail::GetBoolVal(
            values, "localize_footsteps", true);
        loaded.direction.enableGunshots = detail::GetBoolVal(
            values, "localize_gunshots", true);
    } else {
        loaded.direction.enableFootsteps = detail::GetBoolVal(
            values, "direction_footsteps_enabled", true);
        loaded.direction.enableGunshots = detail::GetBoolVal(
            values, "direction_gunshots_enabled", true);
    }

    loaded.overlay.visibility = ParseVisibility(
        detail::GetStr(values, "overlay_visibility", "cs2-only"));
    loaded.overlay.radiusPixels = detail::GetFloatVal(
        values, "overlay_radius_px", 110.0f);
    loaded.overlay.thicknessPixels = detail::GetFloatVal(
        values, "overlay_thickness_px", 8.0f);
    loaded.overlay.opacity = detail::GetFloatVal(
        values, "overlay_opacity", 0.90f);
    loaded.overlay.offsetX = detail::GetFloatVal(
        values, "overlay_offset_x", 0.0f);
    loaded.overlay.offsetY = detail::GetFloatVal(
        values, "overlay_offset_y", 0.0f);
    loaded.overlay.footstepLifetimeSeconds = detail::GetFloatVal(
        values, "overlay_footstep_lifetime_s", 1.2f);
    loaded.overlay.gunshotLifetimeSeconds = detail::GetFloatVal(
        values, "overlay_gunshot_lifetime_s", 0.8f);
    loaded.overlay.showCenterDot = detail::GetBoolVal(
        values, "overlay_center_dot", false);

    loaded.radar.mode = ParseRadarMode(
        detail::GetStr(values, "radar_mode", "continuous"));
    loaded.radar.preset = ParseRadarPreset(
        detail::GetStr(values, "radar_preset", "all"));
    loaded.radar.sensitivityDbfs = detail::GetFloatVal(
        values, "radar_sensitivity_db", -48.0f);
    loaded.radar.attackMilliseconds = static_cast<uint32_t>(
        detail::GetU64(values, "radar_attack_ms", 30));
    loaded.radar.releaseMilliseconds = static_cast<uint32_t>(
        detail::GetU64(values, "radar_release_ms", 250));
    loaded.radar.holdMilliseconds = static_cast<uint32_t>(
        detail::GetU64(values, "radar_hold_ms", 300));
    for (size_t index = 0; index < kRadarCustomCurvePointCount; ++index) {
        loaded.radar.customCurveDb[index] = detail::GetFloatVal(
            values, "radar_custom_curve_" + std::to_string(index), 0.0f);
    }

    loaded.dashboard.audioSetupCollapsed = detail::GetBoolVal(
        values, "dashboard_audio_setup_collapsed", false);
    loaded.dashboard.eventDetailsCollapsed = detail::GetBoolVal(
        values, "dashboard_event_details_collapsed", false);
    loaded.dashboard.hudEditorCollapsed = detail::GetBoolVal(
        values, "dashboard_hud_editor_collapsed", false);
    loaded.dashboard.advancedCollapsed = detail::GetBoolVal(
        values, "dashboard_advanced_collapsed", true);
    loaded.dashboard.windowX = detail::GetFloatVal(
        values, "dashboard_window_x", 100.0f);
    loaded.dashboard.windowY = detail::GetFloatVal(
        values, "dashboard_window_y", 100.0f);
    loaded.dashboard.windowWidth = detail::GetFloatVal(
        values, "dashboard_window_width", 1280.0f);
    loaded.dashboard.windowHeight = detail::GetFloatVal(
        values, "dashboard_window_height", 800.0f);
    loaded.dashboard.maximized = detail::GetBoolVal(
        values, "dashboard_window_maximized", false);

    loaded.onboarding.step = ParseSetupStep(
        detail::GetStr(values, "onboarding_step", "select-endpoint"));
    loaded.onboarding.supportState = ParseSetupSupport(
        detail::GetStr(values, "onboarding_support_state", "unknown"));
    loaded.onboarding.endpointSelected = detail::GetBoolVal(
        values, "onboarding_endpoint_selected", false);
    loaded.onboarding.formatValidated = detail::GetBoolVal(
        values, "onboarding_format_validated", false);
    loaded.onboarding.channelsConfirmed = detail::GetBoolVal(
        values, "onboarding_channels_confirmed", false);
    loaded.onboarding.hudPreviewed = detail::GetBoolVal(
        values, "onboarding_hud_previewed", false);
    loaded.onboarding.completed = detail::GetBoolVal(
        values, "onboarding_completed", false);

    loaded.eventDisplay.showTimeline = detail::GetBoolVal(
        values, "event_show_timeline", true);
    loaded.eventDisplay.showConfidence = detail::GetBoolVal(
        values, "event_show_confidence", true);
    loaded.eventDisplay.showMiniRadar = detail::GetBoolVal(
        values, "event_show_mini_radar", true);
    loaded.eventDisplay.showSuppressedEvents = detail::GetBoolVal(
        values, "event_show_suppressed", false);
    loaded.eventDisplay.maximumRecentEvents = static_cast<uint32_t>(
        detail::GetU64(values, "event_maximum_recent", 100));
    loaded.eventDisplay.minimumConfidence = detail::GetFloatVal(
        values, "event_minimum_confidence", 0.0f);
    loaded.eventDisplay.markerPersistenceSeconds = detail::GetFloatVal(
        values, "event_marker_persistence_s", 2.0f);

    if (schema == AppSettings::kSchemaVersion) {
        const size_t count = std::min<size_t>(
            detail::GetU64(values, "hud_display_count", 1),
            AppSettings::kMaximumHudDisplays);
        loaded.hudDisplays.clear();
        loaded.hudDisplays.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            HudDisplaySettings hud;
            hud.displayId = detail::GetStr(
                values, HudKey(index, "display_id"),
                index == 0 ? "default" : std::string{});
            hud.scale = detail::GetFloatVal(
                values, HudKey(index, "scale"), hud.scale);
            hud.opacity = detail::GetFloatVal(
                values, HudKey(index, "opacity"), hud.opacity);
            hud.anchor = ParseHudAnchor(detail::GetStr(
                values, HudKey(index, "anchor"), "center"));
            hud.offsetX = detail::GetFloatVal(
                values, HudKey(index, "offset_x"), 0.0f);
            hud.offsetY = detail::GetFloatVal(
                values, HudKey(index, "offset_y"), 0.0f);
            hud.sectorColor = LoadColor(
                values, index, "sector_color", hud.sectorColor);
            hud.strongestColor = LoadColor(
                values, index, "strongest_color", hud.strongestColor);
            hud.inactiveColor = LoadColor(
                values, index, "inactive_color", hud.inactiveColor);
            hud.errorColor = LoadColor(
                values, index, "error_color", hud.errorColor);
            hud.showCardinalLabels = detail::GetBoolVal(
                values, HudKey(index, "show_cardinal_labels"), true);
            hud.showDegreeLabels = detail::GetBoolVal(
                values, HudKey(index, "show_degree_labels"), false);
            hud.persistenceSeconds = detail::GetFloatVal(
                values, HudKey(index, "persistence_s"), 1.2f);
            hud.previewBackground = ParsePreviewBackground(detail::GetStr(
                values, HudKey(index, "preview_background"), "tactical-dark"));
            hud.editMode = detail::GetBoolVal(
                values, HudKey(index, "edit_mode"), false);
            hud.visible = detail::GetBoolVal(
                values, HudKey(index, "visible"), true);
            loaded.hudDisplays.push_back(std::move(hud));
        }
    } else {
        MigrateLegacyOverlayToHud(loaded);
    }

    loaded.uiScale = detail::GetFloatVal(
        values, "ui_scale", AppSettings::kDefaultUiScale);
    loaded.sessionLogging =
        detail::GetBoolVal(values, "session_logging", true);

    settings = AppSettings::Clamp(std::move(loaded));
    if (error) error->clear();
    return true;
}

bool AppSettingsFile::Save(const std::filesystem::path& path,
                           const AppSettings& settings,
                           std::string* error) {
    const AppSettings safe = AppSettings::Clamp(settings);
    std::error_code filesystemError;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(
            path.parent_path(), filesystemError);
    }
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

    bool first = true;
    output << std::setprecision(9) << "{\n";
    const auto emitPrefix = [&](const std::string& key) {
        if (!first) output << ",\n";
        first = false;
        output << "  \"" << detail::JsonEscapeStr(key) << "\": ";
    };
    const auto emitString = [&](const std::string& key,
                                const std::string& value) {
        emitPrefix(key);
        output << '"' << detail::JsonEscapeStr(value) << '"';
    };
    const auto emitBool = [&](const std::string& key, bool value) {
        emitPrefix(key);
        output << (value ? "true" : "false");
    };
    const auto emitNumber = [&](const std::string& key, auto value) {
        emitPrefix(key);
        output << value;
    };
    const auto emitColor = [&](size_t index,
                               const char* name,
                               const HudColor& color) {
        const std::string prefix = HudKey(index, name);
        emitNumber(prefix + "_r", color.red);
        emitNumber(prefix + "_g", color.green);
        emitNumber(prefix + "_b", color.blue);
        emitNumber(prefix + "_a", color.alpha);
    };

    emitNumber("schema_version", AppSettings::kSchemaVersion);
    emitString("audio_profile_name", safe.audioProfile.name);
    emitString("eq_profile", ToString(safe.audioProfile.eqProfile));
    emitNumber(
        "lr_isolation_percent",
        safe.audioProfile.leftRightIsolationPercent);
    emitBool(
        "perspective_correction",
        safe.audioProfile.perspectiveCorrection);
    emitNumber(
        "display_aspect_ratio", safe.audioProfile.displayAspectRatio);
    emitString(
        "spatial_enhancement",
        ToString(safe.audioProfile.spatialEnhancement));
    emitString("output_endpoint_id", safe.audioProfile.outputEndpointId);
    emitBool(
        "direction_footsteps_enabled",
        safe.direction.enableFootsteps);
    emitBool(
        "direction_gunshots_enabled",
        safe.direction.enableGunshots);

    emitString(
        "overlay_visibility", VisibilityName(safe.overlay.visibility));
    emitNumber("overlay_radius_px", safe.overlay.radiusPixels);
    emitNumber("overlay_thickness_px", safe.overlay.thicknessPixels);
    emitNumber("overlay_opacity", safe.overlay.opacity);
    emitNumber("overlay_offset_x", safe.overlay.offsetX);
    emitNumber("overlay_offset_y", safe.overlay.offsetY);
    emitNumber(
        "overlay_footstep_lifetime_s",
        safe.overlay.footstepLifetimeSeconds);
    emitNumber(
        "overlay_gunshot_lifetime_s",
        safe.overlay.gunshotLifetimeSeconds);
    emitBool("overlay_center_dot", safe.overlay.showCenterDot);

    emitString("radar_mode", RadarModeName(safe.radar.mode));
    emitString("radar_preset", RadarPresetName(safe.radar.preset));
    emitNumber("radar_sensitivity_db", safe.radar.sensitivityDbfs);
    emitNumber("radar_attack_ms", safe.radar.attackMilliseconds);
    emitNumber("radar_release_ms", safe.radar.releaseMilliseconds);
    emitNumber("radar_hold_ms", safe.radar.holdMilliseconds);
    for (size_t index = 0; index < kRadarCustomCurvePointCount; ++index) {
        emitNumber(
            "radar_custom_curve_" + std::to_string(index),
            safe.radar.customCurveDb[index]);
    }

    emitBool(
        "dashboard_audio_setup_collapsed",
        safe.dashboard.audioSetupCollapsed);
    emitBool(
        "dashboard_event_details_collapsed",
        safe.dashboard.eventDetailsCollapsed);
    emitBool(
        "dashboard_hud_editor_collapsed",
        safe.dashboard.hudEditorCollapsed);
    emitBool(
        "dashboard_advanced_collapsed",
        safe.dashboard.advancedCollapsed);
    emitNumber("dashboard_window_x", safe.dashboard.windowX);
    emitNumber("dashboard_window_y", safe.dashboard.windowY);
    emitNumber("dashboard_window_width", safe.dashboard.windowWidth);
    emitNumber("dashboard_window_height", safe.dashboard.windowHeight);
    emitBool("dashboard_window_maximized", safe.dashboard.maximized);

    emitString("onboarding_step", SetupStepName(safe.onboarding.step));
    emitString(
        "onboarding_support_state",
        SetupSupportName(safe.onboarding.supportState));
    emitBool(
        "onboarding_endpoint_selected",
        safe.onboarding.endpointSelected);
    emitBool(
        "onboarding_format_validated",
        safe.onboarding.formatValidated);
    emitBool(
        "onboarding_channels_confirmed",
        safe.onboarding.channelsConfirmed);
    emitBool(
        "onboarding_hud_previewed",
        safe.onboarding.hudPreviewed);
    emitBool("onboarding_completed", safe.onboarding.completed);

    emitBool("event_show_timeline", safe.eventDisplay.showTimeline);
    emitBool("event_show_confidence", safe.eventDisplay.showConfidence);
    emitBool("event_show_mini_radar", safe.eventDisplay.showMiniRadar);
    emitBool(
        "event_show_suppressed",
        safe.eventDisplay.showSuppressedEvents);
    emitNumber(
        "event_maximum_recent",
        safe.eventDisplay.maximumRecentEvents);
    emitNumber(
        "event_minimum_confidence",
        safe.eventDisplay.minimumConfidence);
    emitNumber(
        "event_marker_persistence_s",
        safe.eventDisplay.markerPersistenceSeconds);

    emitNumber("hud_display_count", safe.hudDisplays.size());
    for (size_t index = 0; index < safe.hudDisplays.size(); ++index) {
        const HudDisplaySettings& hud = safe.hudDisplays[index];
        emitString(HudKey(index, "display_id"), hud.displayId);
        emitNumber(HudKey(index, "scale"), hud.scale);
        emitNumber(HudKey(index, "opacity"), hud.opacity);
        emitString(HudKey(index, "anchor"), HudAnchorName(hud.anchor));
        emitNumber(HudKey(index, "offset_x"), hud.offsetX);
        emitNumber(HudKey(index, "offset_y"), hud.offsetY);
        emitColor(index, "sector_color", hud.sectorColor);
        emitColor(index, "strongest_color", hud.strongestColor);
        emitColor(index, "inactive_color", hud.inactiveColor);
        emitColor(index, "error_color", hud.errorColor);
        emitBool(
            HudKey(index, "show_cardinal_labels"),
            hud.showCardinalLabels);
        emitBool(
            HudKey(index, "show_degree_labels"),
            hud.showDegreeLabels);
        emitNumber(
            HudKey(index, "persistence_s"),
            hud.persistenceSeconds);
        emitString(
            HudKey(index, "preview_background"),
            PreviewBackgroundName(hud.previewBackground));
        emitBool(HudKey(index, "edit_mode"), hud.editMode);
        emitBool(HudKey(index, "visible"), hud.visible);
    }

    emitNumber("ui_scale", safe.uiScale);
    emitBool("session_logging", safe.sessionLogging);
    output << "\n}\n";
    output.flush();
    if (!output) {
        if (error) *error = "Could not finish writing settings file";
        return false;
    }
    output.close();
#ifdef _WIN32
    if (!MoveFileExW(
            temporary.wstring().c_str(), path.wstring().c_str(),
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

bool RuntimeSettingsStore::Update(const AppSettings& settings,
                                  bool persist,
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

DebouncedSettingsSaver::DebouncedSettingsSaver(
    std::shared_ptr<RuntimeSettingsStore> store,
    std::chrono::milliseconds debounceDelay,
    std::chrono::milliseconds maximumDelay)
    : m_store(std::move(store)),
      m_debounceDelay(std::max(
          std::chrono::milliseconds::zero(), debounceDelay)),
      m_maximumDelay(std::max(m_debounceDelay, maximumDelay)),
      m_worker(&DebouncedSettingsSaver::WorkerLoop, this) {}

DebouncedSettingsSaver::~DebouncedSettingsSaver() {
    Stop(nullptr);
}

void DebouncedSettingsSaver::RequestSave() {
    QueueSave(false);
}

void DebouncedSettingsSaver::RequestSaveNow() {
    QueueSave(true);
}

void DebouncedSettingsSaver::QueueSave(bool immediate) {
    const Clock::time_point now = Clock::now();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopRequested) return;
        if (!m_dirty) m_firstRequest = now;
        m_lastRequest = now;
        m_dirty = true;
        m_immediate = m_immediate || immediate;
        ++m_requestedGeneration;
    }
    m_condition.notify_all();
}

bool DebouncedSettingsSaver::Flush(std::string* error) {
    uint64_t targetGeneration = 0;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_stopRequested) {
            // Force one final snapshot even when the caller updated the store
            // without a preceding RequestSave().
            if (!m_dirty) {
                const Clock::time_point now = Clock::now();
                m_firstRequest = now;
                m_lastRequest = now;
                m_dirty = true;
                ++m_requestedGeneration;
            }
            m_immediate = true;
            targetGeneration = m_requestedGeneration;
            m_condition.notify_all();
            m_condition.wait(lock, [&] {
                return m_completedGeneration >= targetGeneration;
            });
            const bool success = m_lastSaveSucceeded;
            if (error) *error = m_lastError;
            return success;
        }
    }
    return CopyLastResult(error);
}

bool DebouncedSettingsSaver::Stop(std::string* error) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_stopRequested) {
            m_stopRequested = true;
            m_immediate = true;
        }
    }
    m_condition.notify_all();
    if (m_worker.joinable()) m_worker.join();
    return CopyLastResult(error);
}

bool DebouncedSettingsSaver::CopyLastResult(std::string* error) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (error) *error = m_lastError;
    return m_lastSaveSucceeded;
}

void DebouncedSettingsSaver::WorkerLoop() {
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;) {
        m_condition.wait(lock, [&] {
            return m_stopRequested || m_dirty;
        });
        if (m_stopRequested && !m_dirty) break;

        if (!m_stopRequested && !m_immediate) {
            const Clock::time_point deadline = std::min(
                m_lastRequest + m_debounceDelay,
                m_firstRequest + m_maximumDelay);
            if (Clock::now() < deadline) {
                m_condition.wait_until(lock, deadline);
                continue;
            }
        }

        const uint64_t saveGeneration = m_requestedGeneration;
        m_dirty = false;
        m_immediate = false;
        m_saveInProgress = true;
        const std::shared_ptr<RuntimeSettingsStore> store = m_store;
        lock.unlock();

        std::string saveError;
        const bool success = store && store->Save(&saveError);
        if (!store && saveError.empty()) {
            saveError = "Settings store is unavailable";
        }

        lock.lock();
        m_saveInProgress = false;
        m_completedGeneration = std::max(
            m_completedGeneration, saveGeneration);
        m_lastSaveSucceeded = success;
        m_lastError = std::move(saveError);
        m_condition.notify_all();
        if (m_stopRequested && !m_dirty) break;
    }
    m_condition.notify_all();
}

} // namespace EchoRadar
