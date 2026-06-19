#include "OverlayRenderer.h"
#include <iostream>

// DirectX 11 and ImGui headers are only available on Windows.
// Full implementation deferred to Milestone 10.

namespace EchoRadar {

OverlayRenderer::OverlayRenderer(Config cfg) : m_cfg(cfg) {}

OverlayRenderer::~OverlayRenderer() {
    Shutdown();
}

bool OverlayRenderer::Initialise() {
#ifdef _WIN32
    // TODO (Milestone 10): create a transparent Win32 window, init DX11 device,
    //   create ImGui context with ImGui_ImplDX11_Init / ImGui_ImplWin32_Init.
    std::cout << "[OverlayRenderer] Initialise (stub, Windows)\n";
    m_running = true;
    return true;
#else
    std::cout << "[OverlayRenderer] Overlay only supported on Windows.\n";
    return false;
#endif
}

void OverlayRenderer::Shutdown() {
    if (!m_running) return;
    m_running = false;
    // TODO (Milestone 10): ImGui_ImplDX11_Shutdown, destroy DX11 resources.
    std::cout << "[OverlayRenderer] Shutdown\n";
}

void OverlayRenderer::PushFootstep(const FootstepEvent& ev,
                                    const DirectionEstimate& dir) {
    m_markers.push_back({dir, /*is_gunshot=*/false, /*ttl=*/2.0f});
    (void)ev;
}

void OverlayRenderer::PushGunshot(const GunshotEvent& ev,
                                   const DirectionEstimate& dir) {
    m_markers.push_back({dir, /*is_gunshot=*/true, /*ttl=*/1.0f});
    (void)ev;
}

void OverlayRenderer::Render() {
    if (!m_running) return;
    // TODO (Milestone 10): ImGui::NewFrame(), draw radar circle and markers,
    //   ImGui::Render(), swap chain Present().
}

} // namespace EchoRadar
