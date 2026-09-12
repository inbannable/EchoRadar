#pragma once

#include <ui/AppState.h>

#include <memory>

namespace EchoRadar {

/// DirectX 11 / ImGui single-dashboard renderer. Runtime data enters only as
/// immutable AppSnapshot values; all user interaction leaves as typed commands.
class OverlayRenderer {
public:
    struct Config {
        int windowX{100};
        int windowY{100};
        int windowWidth{1280};
        int windowHeight{800};
        bool maximized{false};
        std::shared_ptr<LatestSnapshotPublisher> snapshots;
        std::shared_ptr<UiCommandQueue> commands;
    };

    OverlayRenderer();
    explicit OverlayRenderer(Config config);
    ~OverlayRenderer();

    OverlayRenderer(const OverlayRenderer&) = delete;
    OverlayRenderer& operator=(const OverlayRenderer&) = delete;

    bool Initialise();
    void Shutdown();
    void FlushPendingPlacement();
    void Render();
    bool IsRunning() const { return m_running; }

    struct PlatformImpl;
    // Platform-independent view, also rendered by the offline UI preview tool.
    void DrawDashboard(const AppSnapshot& snapshot);
    void SetPreviewPage(int page) { m_page = page >= 0 && page < 5 ? page : 0; }

private:
    Config m_config;
    bool m_running{false};
    int m_page{0};
    float m_appliedUiScale{1.0f};
    std::unique_ptr<PlatformImpl> m_platform;

    void ApplyUiScale(float scale);
    void DrawHeader(const AppSnapshot& snapshot);
    void DrawInlineStatus(const AppSnapshot& snapshot);
    void DrawRadarWorkspace(const AppSnapshot& snapshot);
    void DrawLiveRadar(const AppSnapshot& snapshot, float height);
    void DrawQuickControls(const AppSnapshot& snapshot, float height);
    void DrawRecentEvents(const AppSnapshot& snapshot);
    void DrawAudioSetup(const AppSnapshot& snapshot);
    void DrawEventDetails(const AppSnapshot& snapshot);
    void DrawHudEditor(const AppSnapshot& snapshot);
    void DrawAdvanced(const AppSnapshot& snapshot);
};

} // namespace EchoRadar
