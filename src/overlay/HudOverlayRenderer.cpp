#include "HudOverlayRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <windows.h>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd,
                                                              UINT msg,
                                                              WPARAM wParam,
                                                              LPARAM lParam);
#endif

namespace EchoRadar {

#ifdef _WIN32
struct HudOverlayRenderer::PlatformImpl {
    HWND window{nullptr};
    ID3D11Device* device{nullptr};
    ID3D11DeviceContext* deviceContext{nullptr};
    IDXGISwapChain1* swapChain{nullptr};
    ID3D11RenderTargetView* renderTarget{nullptr};
    IDCompositionDevice* compositionDevice{nullptr};
    IDCompositionTarget* compositionTarget{nullptr};
    IDCompositionVisual* compositionVisual{nullptr};
    HINSTANCE instance{nullptr};
    ImGuiContext* imguiContext{nullptr};
    bool classRegistered{false};
    bool imguiInitialized{false};
};

namespace {

constexpr wchar_t kHudWindowClassName[] = L"EchoRadarDirectionHudWindow";
constexpr int kHudHotkeyId = 0xEC40;

void CreateRenderTarget(HudOverlayRenderer::PlatformImpl& platform) {
    ID3D11Texture2D* backBuffer = nullptr;
    if (platform.swapChain && SUCCEEDED(platform.swapChain->GetBuffer(
            0, IID_PPV_ARGS(&backBuffer)))) {
        platform.device->CreateRenderTargetView(backBuffer, nullptr, &platform.renderTarget);
        backBuffer->Release();
    }
}

void CleanupRenderTarget(HudOverlayRenderer::PlatformImpl& platform) {
    if (platform.renderTarget) {
        platform.renderTarget->Release();
        platform.renderTarget = nullptr;
    }
}

bool CreateDevice(HudOverlayRenderer::PlatformImpl& platform) {
    const D3D_FEATURE_LEVEL featureLevels[]{
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selected{};
    HRESULT result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
        &platform.device, &selected, &platform.deviceContext);
    if (FAILED(result)) {
        result = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
            &platform.device, &selected, &platform.deviceContext);
    }
    if (FAILED(result)) return false;

    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    result = platform.device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (SUCCEEDED(result)) result = dxgiDevice->GetAdapter(&adapter);
    if (SUCCEEDED(result)) result = adapter->GetParent(IID_PPV_ARGS(&factory));
    RECT client{};
    GetClientRect(platform.window, &client);
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = std::max<LONG>(1, client.right - client.left);
    description.Height = std::max<LONG>(1, client.bottom - client.top);
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (SUCCEEDED(result)) {
        result = factory->CreateSwapChainForComposition(
            platform.device, &description, nullptr, &platform.swapChain);
    }
    if (SUCCEEDED(result)) {
        result = DCompositionCreateDevice(
            dxgiDevice, __uuidof(IDCompositionDevice),
            reinterpret_cast<void**>(&platform.compositionDevice));
    }
    if (SUCCEEDED(result)) {
        result = platform.compositionDevice->CreateTargetForHwnd(
            platform.window, TRUE, &platform.compositionTarget);
    }
    if (SUCCEEDED(result)) {
        result = platform.compositionDevice->CreateVisual(&platform.compositionVisual);
    }
    if (SUCCEEDED(result)) {
        result = platform.compositionVisual->SetContent(platform.swapChain);
    }
    if (SUCCEEDED(result)) {
        result = platform.compositionTarget->SetRoot(platform.compositionVisual);
    }
    if (SUCCEEDED(result)) result = platform.compositionDevice->Commit();
    if (factory) factory->Release();
    if (adapter) adapter->Release();
    if (dxgiDevice) dxgiDevice->Release();
    if (FAILED(result)) return false;
    CreateRenderTarget(platform);
    return platform.renderTarget != nullptr;
}

void CleanupDevice(HudOverlayRenderer::PlatformImpl& platform) {
    CleanupRenderTarget(platform);
    if (platform.compositionVisual) platform.compositionVisual->Release();
    if (platform.compositionTarget) platform.compositionTarget->Release();
    if (platform.compositionDevice) platform.compositionDevice->Release();
    if (platform.swapChain) platform.swapChain->Release();
    if (platform.deviceContext) platform.deviceContext->Release();
    if (platform.device) platform.device->Release();
    platform.swapChain = nullptr;
    platform.compositionVisual = nullptr;
    platform.compositionTarget = nullptr;
    platform.compositionDevice = nullptr;
    platform.deviceContext = nullptr;
    platform.device = nullptr;
}

LRESULT WINAPI HudWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* platform = reinterpret_cast<HudOverlayRenderer::PlatformImpl*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        platform = static_cast<HudOverlayRenderer::PlatformImpl*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(platform));
    }
    // This window is display-only.  Handle pointer messages before ImGui so its
    // Win32 backend never installs an arrow cursor or captures/activates the HUD.
    // In borderless fullscreen the game can therefore keep its cursor hidden and
    // receives clicks exactly as if the HUD were not present.
    switch (message) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR:
        SetCursor(nullptr);
        return TRUE;
    default: break;
    }
<<<<<<< HEAD
    if (platform && platform->imguiContext) {
        ImGui::SetCurrentContext(platform->imguiContext);
        if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) {
            return true;
        }
    }
=======
    if (platform && platform->imguiContext) ImGui::SetCurrentContext(platform->imguiContext);
    const bool isMouseMessage =
        (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
        (message >= WM_NCMOUSEMOVE && message <= WM_NCXBUTTONDBLCLK) ||
        message == WM_MOUSEHOVER || message == WM_MOUSELEAVE ||
        message == WM_NCMOUSEHOVER || message == WM_NCMOUSELEAVE ||
        message == WM_CAPTURECHANGED;
    if (!isMouseMessage && ImGui::GetCurrentContext() &&
        ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) return true;
>>>>>>> fd6579ebd09daa30de54739f32935b96cc1036fd
    switch (message) {
    case WM_SIZE:
        if (platform && platform->device && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget(*platform);
            platform->swapChain->ResizeBuffers(
                0, static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)),
                DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget(*platform);
        }
        return 0;
    case WM_DESTROY: return 0;
    default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

struct WindowSearch {
    HWND result{nullptr};
};

BOOL CALLBACK FindCs2Window(HWND window, LPARAM parameter) {
    if (!IsWindowVisible(window)) return TRUE;
    wchar_t title[256]{};
    GetWindowTextW(window, title, ARRAYSIZE(title));
    if (std::wstring(title).find(L"Counter-Strike 2") != std::wstring::npos) {
        reinterpret_cast<WindowSearch*>(parameter)->result = window;
        return FALSE;
    }
    return TRUE;
}

HWND Cs2Window() {
    WindowSearch search;
    EnumWindows(FindCs2Window, reinterpret_cast<LPARAM>(&search));
    return search.result;
}

RECT WindowScreenRect(HWND window) {
    RECT rectangle{};
    if (!window || !GetClientRect(window, &rectangle)) {
        rectangle = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        return rectangle;
    }
    POINT topLeft{rectangle.left, rectangle.top};
    POINT bottomRight{rectangle.right, rectangle.bottom};
    ClientToScreen(window, &topLeft);
    ClientToScreen(window, &bottomRight);
    return {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
}

void DrawArc(ImDrawList* drawList, ImVec2 center, float radius,
             float angleDegrees, float uncertaintyDegrees,
             ImU32 color, float thickness) {
    const float start = (angleDegrees - uncertaintyDegrees - 90.0f) *
        3.14159265358979323846f / 180.0f;
    const float finish = (angleDegrees + uncertaintyDegrees - 90.0f) *
        3.14159265358979323846f / 180.0f;
    drawList->PathArcTo(center, radius, start, finish, 40);
    drawList->PathStroke(color, 0, thickness);
}

} // namespace
#else
struct HudOverlayRenderer::PlatformImpl {};
#endif

HudOverlayRenderer::HudOverlayRenderer(Config config) : m_config(std::move(config)) {}

HudOverlayRenderer::~HudOverlayRenderer() {
    Shutdown();
}

bool HudOverlayRenderer::Initialise() {
#ifdef _WIN32
    if (m_running) return true;
    m_platform = std::make_unique<PlatformImpl>();
    m_platform->instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = HudWindowProc;
    windowClass.hInstance = m_platform->instance;
    // A display-only overlay must not own a cursor.  The underlying game owns
    // cursor visibility and shape, including its hidden in-game state.
    windowClass.hCursor = nullptr;
    windowClass.lpszClassName = kHudWindowClassName;
    if (RegisterClassExW(&windowClass)) m_platform->classRegistered = true;
    else if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    const RECT rectangle = WindowScreenRect(Cs2Window());
    const DWORD exStyle =
        WS_EX_TOPMOST |
        WS_EX_LAYERED |
        WS_EX_TRANSPARENT |
        WS_EX_TOOLWINDOW |
        WS_EX_NOACTIVATE |
        WS_EX_NOREDIRECTIONBITMAP;
    m_platform->window = CreateWindowExW(
        exStyle,
        kHudWindowClassName, L"EchoRadar Direction HUD", WS_POPUP,
        rectangle.left, rectangle.top, rectangle.right - rectangle.left,
        rectangle.bottom - rectangle.top, nullptr, nullptr,
        m_platform->instance, m_platform.get());
    if (!m_platform->window || !CreateDevice(*m_platform)) {
        Shutdown();
        return false;
    }
    m_platform->imguiContext = ImGui::CreateContext();
    ImGui::SetCurrentContext(m_platform->imguiContext);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui_ImplWin32_Init(m_platform->window);
    ImGui_ImplDX11_Init(m_platform->device, m_platform->deviceContext);
    m_platform->imguiInitialized = true;
    RegisterHotKey(m_platform->window, kHudHotkeyId, MOD_CONTROL | MOD_ALT, 'O');
    ShowWindow(m_platform->window, SW_SHOWNOACTIVATE);
    SetWindowPos(m_platform->window, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    m_running = true;
    return true;
#else
    return false;
#endif
}

void HudOverlayRenderer::Shutdown() {
#ifdef _WIN32
    if (m_platform) {
        if (m_platform->window) UnregisterHotKey(m_platform->window, kHudHotkeyId);
        if (m_platform->imguiInitialized) {
            ImGui::SetCurrentContext(m_platform->imguiContext);
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext(m_platform->imguiContext);
        }
        CleanupDevice(*m_platform);
        if (m_platform->window) DestroyWindow(m_platform->window);
        if (m_platform->classRegistered) {
            UnregisterClassW(kHudWindowClassName, m_platform->instance);
        }
        m_platform.reset();
    }
#endif
    m_running = false;
}

void HudOverlayRenderer::PushEvent(const V4SoundEvent& event,
                                   const DirectionResult& direction) {
    DirectionSceneResult scene;
    scene.sceneId = direction.sceneId;
    scene.status = direction.status;
    if (direction.status == DirectionStatus::Estimated ||
        direction.status == DirectionStatus::LowConfidence) {
        scene.sources[0] = {
            direction.primaryAngleDegrees,
            direction.primaryElevationDegrees,
            direction.confidence,
            direction.uncertaintyDegrees,
        };
        scene.sourceCount = 1;
        if (direction.secondaryAngleDegrees) {
            scene.sources[1] = {
                *direction.secondaryAngleDegrees,
                direction.secondaryElevationDegrees.value_or(0.0f),
                direction.secondaryConfidence,
                direction.uncertaintyDegrees,
            };
            scene.sourceCount = 2;
        }
    }
    PushScene(event, scene);
}

void HudOverlayRenderer::PushScene(const V4SoundEvent& event,
                                   const DirectionSceneResult& direction) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_markers.push_back({event, direction, std::chrono::steady_clock::now()});
    if (m_markers.size() > 64) m_markers.erase(m_markers.begin());
}

void HudOverlayRenderer::Render() {
#ifdef _WIN32
    if (!m_running || !m_platform || !m_platform->imguiInitialized) return;
    ImGui::SetCurrentContext(m_platform->imguiContext);
    MSG message{};
    while (PeekMessageW(&message, m_platform->window, 0, 0, PM_REMOVE)) {
        if (message.message == WM_HOTKEY && message.wParam == kHudHotkeyId) {
            m_hotkeyHidden = !m_hotkeyHidden;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    const AppSettings settings = m_config.settings
        ? m_config.settings->Snapshot() : AppSettings{};
    const HWND cs2 = Cs2Window();
    const bool cs2Foreground = cs2 && GetForegroundWindow() == cs2;
    const bool shouldShow = !m_hotkeyHidden &&
        settings.overlay.visibility != OverlaySettings::Visibility::Off &&
        (settings.overlay.visibility == OverlaySettings::Visibility::Always || cs2Foreground);
    if (!shouldShow) {
        ShowWindow(m_platform->window, SW_HIDE);
        return;
    }
    const RECT rectangle = WindowScreenRect(
        settings.overlay.visibility == OverlaySettings::Visibility::Cs2Only ? cs2 : nullptr);
    SetWindowPos(m_platform->window, HWND_TOPMOST, rectangle.left, rectangle.top,
                 rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);

    const auto now = std::chrono::steady_clock::now();
    std::vector<Marker> markers;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_markers.erase(std::remove_if(m_markers.begin(), m_markers.end(), [&](const Marker& marker) {
            const float lifetime = marker.event.soundClass == SoundClass::Gunshot
                ? settings.overlay.gunshotLifetimeSeconds
                : settings.overlay.footstepLifetimeSeconds;
            return std::chrono::duration<float>(now - marker.created).count() >= lifetime;
        }), m_markers.end());
        markers = m_markers;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    const ImVec2 center{
        io.DisplaySize.x * 0.5f + settings.overlay.offsetX,
        io.DisplaySize.y * 0.5f + settings.overlay.offsetY,
    };
    for (const Marker& marker : markers) {
        if (marker.direction.status != DirectionStatus::Estimated &&
            marker.direction.status != DirectionStatus::LowConfidence) continue;
        const float lifetime = marker.event.soundClass == SoundClass::Gunshot
            ? settings.overlay.gunshotLifetimeSeconds
            : settings.overlay.footstepLifetimeSeconds;
        const float age = std::chrono::duration<float>(now - marker.created).count();
        const float fade = std::clamp(1.0f - age / std::max(0.1f, lifetime), 0.0f, 1.0f);
        // Keep uncertain but usable estimates visible instead of silently
        // dropping recognizer events.  Their lower opacity still communicates
        // that the bearing should be treated cautiously.
        for (uint32_t sourceIndex = 0;
             sourceIndex < std::min<uint32_t>(
                 marker.direction.sourceCount,
                 static_cast<uint32_t>(DirectionSceneResult::kMaximumSources));
             ++sourceIndex) {
            const DirectionSourceEstimate& source = marker.direction.sources[sourceIndex];
            const float visibleConfidence = marker.direction.status == DirectionStatus::Estimated
                ? std::max(0.25f, source.confidence)
                : std::max(0.16f, source.confidence * 0.65f);
            const float alpha = std::clamp(
                settings.overlay.opacity * fade * visibleConfidence, 0.0f, 1.0f);
            const ImVec4 color = marker.event.soundClass == SoundClass::Gunshot
                ? ImVec4(1.0f, 0.30f, 0.22f, alpha)
                : ImVec4(0.20f, 0.82f, 1.0f, alpha);
            const ImVec2 sourceCenter{
                center.x,
                center.y - std::clamp(source.elevationDegrees, -60.0f, 60.0f) * 0.70f,
            };
            DrawArc(drawList, sourceCenter, settings.overlay.radiusPixels,
                    source.azimuthDegrees,
                    std::clamp(source.uncertaintyDegrees, 1.0f, 180.0f),
                    ImGui::ColorConvertFloat4ToU32(color), settings.overlay.thicknessPixels);

            const float radians = (source.azimuthDegrees - 90.0f) *
                3.14159265358979323846f / 180.0f;
            const ImVec2 point{
                sourceCenter.x + std::cos(radians) * settings.overlay.radiusPixels,
                sourceCenter.y + std::sin(radians) * settings.overlay.radiusPixels,
            };
            const ImU32 packed = ImGui::ColorConvertFloat4ToU32(color);
            if (std::abs(source.elevationDegrees) >= 4.0f) {
                const float sign = source.elevationDegrees > 0.0f ? -1.0f : 1.0f;
                drawList->AddTriangleFilled(
                    ImVec2(point.x, point.y + sign * 4.0f),
                    ImVec2(point.x - 5.0f, point.y - sign * 4.0f),
                    ImVec2(point.x + 5.0f, point.y - sign * 4.0f), packed);
            }
            char elevationText[24]{};
            std::snprintf(elevationText, sizeof(elevationText), "%+.0f deg",
                          source.elevationDegrees);
            drawList->AddText(ImVec2(point.x + 8.0f, point.y - 7.0f), packed, elevationText);
        }
    }
    if (settings.overlay.showCenterDot) {
        drawList->AddCircleFilled(center, 2.5f, IM_COL32(255, 255, 255, 180));
    }
    ImGui::Render();
    const float clear[4]{0.0f, 0.0f, 0.0f, 0.0f};
    m_platform->deviceContext->OMSetRenderTargets(1, &m_platform->renderTarget, nullptr);
    m_platform->deviceContext->ClearRenderTargetView(m_platform->renderTarget, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    m_platform->swapChain->Present(1, 0);
#endif
}

} // namespace EchoRadar
