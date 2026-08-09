#pragma once

#include <localization/LocalizationTypes.h>
#include <recognition/V4RecognitionTypes.h>
#include <settings/AppSettings.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

namespace EchoRadar {

/// Independent transparent, click-through, topmost direction HUD.
class HudOverlayRenderer {
public:
    struct Config {
        std::shared_ptr<RuntimeSettingsStore> settings;
    };

    explicit HudOverlayRenderer(Config config);
    ~HudOverlayRenderer();

    bool Initialise();
    void Shutdown();
    void Render();
    void PushEvent(const V4SoundEvent& event, const DirectionResult& direction);
    bool IsRunning() const { return m_running; }

    struct PlatformImpl;

private:
    struct Marker {
        V4SoundEvent event;
        DirectionResult direction;
        std::chrono::steady_clock::time_point created;
    };

    Config m_config;
    bool m_running{false};
    bool m_hotkeyHidden{false};
    std::unique_ptr<PlatformImpl> m_platform;
    std::mutex m_mutex;
    std::vector<Marker> m_markers;
};

} // namespace EchoRadar
