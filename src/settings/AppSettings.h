#pragma once

#include <audio/AudioTypes.h>
#include <direction/DirectionTypes.h>

#include <filesystem>
#include <mutex>
#include <string>

namespace EchoRadar {

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

struct AppSettings {
    static constexpr uint32_t kSchemaVersion = 3;
    static constexpr float kDefaultUiScale = 1.25f;
    static constexpr float kMinUiScale = 0.75f;
    static constexpr float kMaxUiScale = 2.0f;

    uint32_t schemaVersion{kSchemaVersion};
    AudioProfile audioProfile;
    DirectionSettings direction;
    OverlaySettings overlay;
    float uiScale{kDefaultUiScale};
    bool sessionLogging{true};

    static AppSettings Clamp(AppSettings settings);
};

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

} // namespace EchoRadar
