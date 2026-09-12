#pragma once

#include <audio/AudioTypes.h>
#include <direction/DirectionTypes.h>
#include <radar/RadarTypes.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace EchoRadar {

// Kept for the v1 recognition/direction HUD while the v2 HUD is rolled out.
struct OverlaySettings {
    enum class Visibility : uint8_t {
        Off,
        Cs2Only,
        Always,
    };

    Visibility visibility{Visibility::Cs2Only};
    float radiusPixels{110.0f};
    float thicknessPixels{8.0f};
    float opacity{0.90f};
    float offsetX{0.0f};
    float offsetY{0.0f};
    float footstepLifetimeSeconds{1.2f};
    float gunshotLifetimeSeconds{0.8f};
    bool showCenterDot{false};
};

// The persisted tuning object is intentionally the processor's public config:
// applying a settings snapshot cannot lose units or reinterpret curve values.
using RadarTuningSettings = RadarProcessorConfig;

struct DashboardSettings {
    bool audioSetupCollapsed{false};
    bool eventDetailsCollapsed{false};
    bool hudEditorCollapsed{false};
    bool advancedCollapsed{true};
    float windowX{100.0f};
    float windowY{100.0f};
    float windowWidth{1280.0f};
    float windowHeight{800.0f};
    bool maximized{false};
};

enum class SetupStep : uint8_t {
    SelectEndpoint,
    ValidateFormat,
    ConfirmChannels,
    PreviewHud,
    Complete,
};

enum class SetupSupportState : uint8_t {
    Unknown,
    Supported,
    NoEndpoint,
    UnsupportedStereo,
    UnsupportedSampleRate,
    UnsupportedChannelMask,
    CaptureFailure,
    HeadphoneStereo,
};

struct OnboardingSettings {
    SetupStep step{SetupStep::SelectEndpoint};
    SetupSupportState supportState{SetupSupportState::Unknown};
    bool endpointSelected{false};
    bool formatValidated{false};
    bool channelsConfirmed{false};
    bool hudPreviewed{false};
    bool completed{false};
};

struct EventDisplaySettings {
    bool showTimeline{true};
    bool showConfidence{true};
    bool showMiniRadar{true};
    bool showSuppressedEvents{false};
    uint32_t maximumRecentEvents{100};
    float minimumConfidence{0.0f};
    float markerPersistenceSeconds{2.0f};
};

struct HudColor {
    float red{0.0f};
    float green{0.85f};
    float blue{1.0f};
    float alpha{1.0f};
};

enum class HudAnchor : uint8_t {
    TopLeft,
    TopCenter,
    TopRight,
    CenterLeft,
    Center,
    CenterRight,
    BottomLeft,
    BottomCenter,
    BottomRight,
};

enum class HudPreviewBackground : uint8_t {
    Transparent,
    TacticalDark,
    Light,
    Checkerboard,
};

struct HudDisplaySettings {
    std::string displayId{"default"};
    float scale{1.0f};
    float opacity{0.90f};
    HudAnchor anchor{HudAnchor::Center};
    float offsetX{0.0f};
    float offsetY{0.0f};
    HudColor sectorColor{};
    HudColor strongestColor{1.0f, 0.70f, 0.10f, 1.0f};
    HudColor inactiveColor{0.40f, 0.46f, 0.50f, 0.55f};
    HudColor errorColor{1.0f, 0.20f, 0.18f, 1.0f};
    bool showCardinalLabels{true};
    bool showDegreeLabels{false};
    float persistenceSeconds{1.2f};
    HudPreviewBackground previewBackground{HudPreviewBackground::TacticalDark};
    bool editMode{false};
    bool visible{true};
};

struct HudRect {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
};

struct AppSettings {
    static constexpr uint32_t kSchemaVersion = 4;
    static constexpr float kDefaultUiScale = 1.25f;
    static constexpr float kMinUiScale = 0.75f;
    static constexpr float kMaxUiScale = 2.0f;
    static constexpr size_t kMaximumHudDisplays = 16;

    uint32_t schemaVersion{kSchemaVersion};
    AudioProfile audioProfile;
    DirectionSettings direction;
    OverlaySettings overlay;
    RadarTuningSettings radar;
    DashboardSettings dashboard;
    OnboardingSettings onboarding;
    EventDisplaySettings eventDisplay;
    std::vector<HudDisplaySettings> hudDisplays{HudDisplaySettings{}};
    float uiScale{kDefaultUiScale};
    bool sessionLogging{true};

    static AppSettings Clamp(AppSettings settings);
};

const HudDisplaySettings* FindHudDisplaySettings(
    const AppSettings& settings, std::string_view displayId);
HudDisplaySettings* FindHudDisplaySettings(
    AppSettings& settings, std::string_view displayId);

// Adds a new display configuration or replaces the matching one. Returns false
// when displayId is empty or the bounded collection is full.
bool UpdateHudDisplaySettings(AppSettings& settings,
                              std::string_view displayId,
                              HudDisplaySettings configuration);

// Pure geometry helpers. Coordinates are desktop pixels and usableBounds is a
// monitor work area (taskbar/dock already excluded).
HudRect ClampHudToUsableMonitorBounds(HudRect hudBounds,
                                      HudRect usableBounds);
HudRect ResolveHudBounds(const HudDisplaySettings& settings,
                         HudRect usableBounds,
                         float unscaledWidth,
                         float unscaledHeight);

class AppSettingsFile {
public:
    static std::filesystem::path DefaultPath();
    static bool Load(const std::filesystem::path& path, AppSettings& settings,
                     std::string* error = nullptr);
    static bool Save(const std::filesystem::path& path, const AppSettings& settings,
                     std::string* error = nullptr);
};

class RuntimeSettingsStore {
public:
    explicit RuntimeSettingsStore(
        std::filesystem::path path = AppSettingsFile::DefaultPath());

    bool Load(std::string* error = nullptr);
    bool Save(std::string* error = nullptr) const;
    AppSettings Snapshot() const;
    bool Update(const AppSettings& settings, bool persist = true,
                std::string* error = nullptr);
    void Reset(bool persist = true);

    const std::filesystem::path& Path() const { return m_path; }

private:
    std::filesystem::path m_path;
    mutable std::mutex m_mutex;
    AppSettings m_settings;
};

// Coalesces rapid settings mutations and performs file I/O away from the DSP
// thread. Continuous input cannot postpone persistence beyond maximumDelay,
// and Stop()/destruction synchronously flush the latest requested generation.
class DebouncedSettingsSaver {
public:
    explicit DebouncedSettingsSaver(
        std::shared_ptr<RuntimeSettingsStore> store,
        std::chrono::milliseconds debounceDelay =
            std::chrono::milliseconds(250),
        std::chrono::milliseconds maximumDelay =
            std::chrono::milliseconds(2000));
    ~DebouncedSettingsSaver();

    DebouncedSettingsSaver(const DebouncedSettingsSaver&) = delete;
    DebouncedSettingsSaver& operator=(const DebouncedSettingsSaver&) = delete;

    void RequestSave();
    void RequestSaveNow();
    bool Flush(std::string* error = nullptr);
    bool Stop(std::string* error = nullptr);

private:
    using Clock = std::chrono::steady_clock;

    std::shared_ptr<RuntimeSettingsStore> m_store;
    std::chrono::milliseconds m_debounceDelay;
    std::chrono::milliseconds m_maximumDelay;
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    std::thread m_worker;
    bool m_stopRequested{false};
    bool m_dirty{false};
    bool m_immediate{false};
    bool m_saveInProgress{false};
    bool m_lastSaveSucceeded{true};
    uint64_t m_requestedGeneration{0};
    uint64_t m_completedGeneration{0};
    Clock::time_point m_firstRequest{};
    Clock::time_point m_lastRequest{};
    std::string m_lastError;

    void QueueSave(bool immediate);
    void WorkerLoop();
    bool CopyLastResult(std::string* error) const;
};

} // namespace EchoRadar
