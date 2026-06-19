#pragma once
#include "../common/Types.h"
#include <vector>

namespace EchoRadar {

/// Renders the EchoRadar HUD overlay using Dear ImGui + DirectX 11.
/// Creates a borderless, always-on-top transparent window.
///
/// Only compiled on Windows (_WIN32).
class OverlayRenderer {
public:
    struct Config {
        int   window_width{400};
        int   window_height{400};
        float radar_radius{150.0f};
        float opacity{0.85f};
    };

    explicit OverlayRenderer(Config cfg = {});
    ~OverlayRenderer();

    OverlayRenderer(const OverlayRenderer&)            = delete;
    OverlayRenderer& operator=(const OverlayRenderer&) = delete;

    /// Initialise the overlay window and DX11 device.
    bool Initialise();
    void Shutdown();

    /// Push a new set of events to display (called from processing thread).
    void PushFootstep(const FootstepEvent&     ev, const DirectionEstimate& dir);
    void PushGunshot (const GunshotEvent&      ev, const DirectionEstimate& dir);

    /// Main render tick – call from the main/UI thread.
    void Render();

    bool IsRunning() const { return m_running; }

private:
    Config m_cfg;
    bool   m_running{false};

    struct ActiveMarker {
        DirectionEstimate dir;
        bool              is_gunshot{false};
        float             ttl{2.0f};  // seconds remaining
    };
    std::vector<ActiveMarker> m_markers;

    // Platform handles – opaque here; defined in .cpp
    struct PlatformImpl;
    // std::unique_ptr<PlatformImpl> m_platform;  // uncomment in Milestone 10
};

} // namespace EchoRadar
