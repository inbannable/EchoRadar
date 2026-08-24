#pragma once

#include <ui/AppState.h>

#include <memory>
#include <string>

namespace EchoRadar {

/// Original EchoRadar v2 azimuth HUD. It consumes immutable snapshots, remains
/// click-through in normal mode, and submits persisted drag updates in edit mode.
class HudOverlayRenderer {
public:
    struct Config {
        std::shared_ptr<LatestSnapshotPublisher> snapshots;
        std::shared_ptr<UiCommandQueue> commands;
    };

    explicit HudOverlayRenderer(Config config);
    ~HudOverlayRenderer();

    bool Initialise();
    void Shutdown();
    void Render();
    bool IsRunning() const { return m_running; }

    struct PlatformImpl;

private:
    Config m_config;
    bool m_running{false};
    std::string m_reportedDisplayId;
    HudRect m_reportedWorkArea{};
    float m_reportedDpiScale{1.0f};
    std::unique_ptr<PlatformImpl> m_platform;
};

} // namespace EchoRadar
