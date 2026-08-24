#include "HudOverlayRenderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>

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
    bool editMode{false};
    bool dragActive{false};
    bool havePendingDrag{false};
    std::chrono::steady_clock::time_point dragReleasedAt{};
    std::string dragDisplayId;
    float dragOffsetX{0.0f};
    float dragOffsetY{0.0f};
    HWND targetWindow{nullptr};
    RECT interactiveDesktopRect{};
};

namespace {

constexpr wchar_t kWindowClassName[] = L"EchoRadarV2HudWindow";
constexpr int kHudHotkeyId = 0xEC42;
constexpr float kPi = 3.14159265358979323846f;
constexpr auto kPendingDragGrace = std::chrono::milliseconds(250);

class ScopedImGuiContext {
public:
    explicit ScopedImGuiContext(ImGuiContext* context)
        : m_previous(ImGui::GetCurrentContext()) {
        ImGui::SetCurrentContext(context);
    }

    ~ScopedImGuiContext() {
        ImGui::SetCurrentContext(m_previous);
    }

    ScopedImGuiContext(const ScopedImGuiContext&) = delete;
    ScopedImGuiContext& operator=(const ScopedImGuiContext&) = delete;

private:
    ImGuiContext* m_previous{nullptr};
};

void AddUiFont(ImGuiIO& io, float pixelSize) {
    if (!io.Fonts->AddFontFromFileTTF(
            "C:\\Windows\\Fonts\\segoeui.ttf", pixelSize)) {
        io.Fonts->AddFontDefault();
    }
    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\msyh.ttc") !=
        INVALID_FILE_ATTRIBUTES) {
        ImFontConfig fallback{};
        fallback.MergeMode = true;
        fallback.PixelSnapH = true;
        io.Fonts->AddFontFromFileTTF(
            "C:\\Windows\\Fonts\\msyh.ttc", pixelSize, &fallback,
            io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    }
}

void CreateRenderTarget(HudOverlayRenderer::PlatformImpl& platform) {
    ID3D11Texture2D* backBuffer = nullptr;
    if (platform.swapChain && SUCCEEDED(platform.swapChain->GetBuffer(
            0, IID_PPV_ARGS(&backBuffer)))) {
        platform.device->CreateRenderTargetView(
            backBuffer, nullptr, &platform.renderTarget);
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
    const D3D_FEATURE_LEVEL levels[]{
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selected{};
    HRESULT result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION, &platform.device, &selected,
        &platform.deviceContext);
    if (FAILED(result)) {
        result = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
            D3D11_SDK_VERSION, &platform.device, &selected,
            &platform.deviceContext);
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
        result = platform.compositionDevice->CreateVisual(
            &platform.compositionVisual);
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
    platform.compositionVisual = nullptr;
    platform.compositionTarget = nullptr;
    platform.compositionDevice = nullptr;
    platform.swapChain = nullptr;
    platform.deviceContext = nullptr;
    platform.device = nullptr;
}

bool PointInside(const RECT& rectangle, LPARAM lParam) {
    const POINT point{static_cast<short>(LOWORD(lParam)),
                      static_cast<short>(HIWORD(lParam))};
    return PtInRect(&rectangle, point) != FALSE;
}

LRESULT WINAPI WindowProc(HWND window, UINT message,
                          WPARAM wParam, LPARAM lParam) {
    auto* platform = reinterpret_cast<HudOverlayRenderer::PlatformImpl*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        platform = static_cast<HudOverlayRenderer::PlatformImpl*>(
            create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(platform));
    }
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, wParam, lParam);
    }
    switch (message) {
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        return 0;
    }
    case WM_NCHITTEST:
        return platform && platform->editMode &&
                       PointInside(platform->interactiveDesktopRect, lParam)
            ? HTCLIENT : HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        SetCursor(platform && platform->editMode
                      ? LoadCursor(nullptr, IDC_SIZEALL) : nullptr);
        return TRUE;
    default:
        break;
    }
    if (platform && platform->editMode && platform->imguiInitialized &&
        platform->imguiContext) {
        const ScopedImGuiContext context(platform->imguiContext);
        if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) {
            return true;
        }
    }
    switch (message) {
    case WM_SIZE:
        if (platform && platform->device && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget(*platform);
            platform->swapChain->ResizeBuffers(
                0, static_cast<UINT>(LOWORD(lParam)),
                static_cast<UINT>(HIWORD(lParam)), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget(*platform);
        }
        return 0;
    case WM_DESTROY:
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

HWND ForegroundExternalWindow() {
    const HWND foreground = GetForegroundWindow();
    if (!foreground || !IsWindowVisible(foreground) || IsIconic(foreground) ||
        foreground == GetShellWindow()) {
        return nullptr;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    if (processId == 0 || processId == GetCurrentProcessId()) return nullptr;
    if ((GetWindowLongPtrW(foreground, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) {
        return nullptr;
    }
    RECT rectangle{};
    if (!GetWindowRect(foreground, &rectangle) ||
        rectangle.right - rectangle.left < 640 ||
        rectangle.bottom - rectangle.top < 400) {
        return nullptr;
    }
    return foreground;
}

std::string Utf8(const wchar_t* value) {
    if (!value || !*value) return "default";
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return "default";
    std::string output(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, output.data(),
                        length, nullptr, nullptr);
    output.resize(static_cast<size_t>(length - 1));
    return output;
}

ImU32 Color(const HudColor& value, float alphaScale = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(
        value.red, value.green, value.blue,
        std::clamp(value.alpha * alphaScale, 0.0f, 1.0f)));
}

ImVec2 Point(ImVec2 center, float radius, float azimuthDegrees) {
    const float radians = (azimuthDegrees - 90.0f) * kPi / 180.0f;
    return {center.x + std::cos(radians) * radius,
            center.y + std::sin(radians) * radius};
}

float Intensity(float db, float sensitivity) {
    if (!std::isfinite(db) || db <= kRadarSilenceDbfs + 0.1f) return 0.0f;
    return std::clamp((db - sensitivity) /
                          std::max(1.0f, -sensitivity),
                      0.0f, 1.0f);
}

void DrawArc(ImDrawList* drawList, ImVec2 center, float radius,
             float azimuth, float uncertainty, ImU32 color,
             float thickness) {
    const float start = (azimuth - uncertainty - 90.0f) * kPi / 180.0f;
    const float finish = (azimuth + uncertainty - 90.0f) * kPi / 180.0f;
    drawList->PathArcTo(center, radius, start, finish, 32);
    drawList->PathStroke(color, 0, thickness);
}

} // namespace
#else
struct HudOverlayRenderer::PlatformImpl {};
#endif

HudOverlayRenderer::HudOverlayRenderer(Config config)
    : m_config(std::move(config)) {}

HudOverlayRenderer::~HudOverlayRenderer() { Shutdown(); }

bool HudOverlayRenderer::Initialise() {
#ifdef _WIN32
    if (m_running) return true;
    m_platform = std::make_unique<PlatformImpl>();
    m_platform->instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = m_platform->instance;
    windowClass.hCursor = nullptr;
    windowClass.lpszClassName = kWindowClassName;
    if (RegisterClassExW(&windowClass)) {
        m_platform->classRegistered = true;
    } else if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP;
    m_platform->window = CreateWindowExW(
        exStyle, kWindowClassName, L"EchoRadar v2 HUD", WS_POPUP,
        0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
        nullptr, nullptr, m_platform->instance, m_platform.get());
    if (!m_platform->window || !CreateDevice(*m_platform)) {
        Shutdown();
        return false;
    }
    m_platform->imguiContext = ImGui::CreateContext();
    {
        const ScopedImGuiContext context(m_platform->imguiContext);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        AddUiFont(io, 15.0f);
        ImGui_ImplWin32_Init(m_platform->window);
        ImGui_ImplDX11_Init(m_platform->device, m_platform->deviceContext);
        m_platform->imguiInitialized = true;
    }
    RegisterHotKey(m_platform->window, kHudHotkeyId,
                   MOD_CONTROL | MOD_ALT, 'O');
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
        ImGuiContext* context = m_platform->imguiContext;
        ImGuiContext* previous = ImGui::GetCurrentContext();
        if (m_platform->window && IsWindow(m_platform->window)) {
            UnregisterHotKey(m_platform->window, kHudHotkeyId);
            SetWindowLongPtrW(m_platform->window, GWLP_USERDATA, 0);
        }
        if (m_platform->imguiInitialized && context) {
            ImGui::SetCurrentContext(context);
            m_platform->imguiInitialized = false;
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
        }
        if (m_platform->window && IsWindow(m_platform->window)) {
            DestroyWindow(m_platform->window);
        }
        m_platform->window = nullptr;
        if (context) {
            if (previous == context) previous = nullptr;
            ImGui::SetCurrentContext(context);
            ImGui::DestroyContext(context);
            m_platform->imguiContext = nullptr;
        }
        ImGui::SetCurrentContext(previous);
        CleanupDevice(*m_platform);
        if (m_platform->classRegistered) {
            UnregisterClassW(kWindowClassName, m_platform->instance);
        }
        m_platform.reset();
    }
#endif
    m_running = false;
}

void HudOverlayRenderer::Render() {
#ifdef _WIN32
    if (!m_running || !m_platform || !m_platform->imguiInitialized) return;
    const ScopedImGuiContext context(m_platform->imguiContext);
    const std::shared_ptr<const AppSnapshot> snapshot = m_config.snapshots
        ? m_config.snapshots->Latest() : std::make_shared<AppSnapshot>();

    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (const HWND foreground = ForegroundExternalWindow()) {
        m_platform->targetWindow = foreground;
    } else if (m_platform->targetWindow &&
               !IsWindow(m_platform->targetWindow)) {
        m_platform->targetWindow = nullptr;
    }
    const HWND game = m_platform->targetWindow;
    const HMONITOR monitor = MonitorFromWindow(
        game ? game : m_platform->window, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(monitor, &monitorInfo);
    const UINT targetDpi = GetDpiForWindow(game ? game : m_platform->window);
    const float dpiScale = static_cast<float>(targetDpi ? targetDpi : 96u) /
        96.0f;
    const std::string displayId = Utf8(monitorInfo.szDevice);
    const HudRect work{
        static_cast<float>(monitorInfo.rcWork.left),
        static_cast<float>(monitorInfo.rcWork.top),
        static_cast<float>(monitorInfo.rcWork.right - monitorInfo.rcWork.left),
        static_cast<float>(monitorInfo.rcWork.bottom - monitorInfo.rcWork.top),
    };
    if (displayId != m_reportedDisplayId ||
        work.x != m_reportedWorkArea.x || work.y != m_reportedWorkArea.y ||
        work.width != m_reportedWorkArea.width ||
        work.height != m_reportedWorkArea.height ||
        std::abs(dpiScale - m_reportedDpiScale) > 0.001f) {
        m_reportedDisplayId = displayId;
        m_reportedWorkArea = work;
        m_reportedDpiScale = dpiScale;
        if (m_config.commands) {
            m_config.commands->Emplace<SetActiveHudDisplayCommand>(
                displayId, work, dpiScale);
        }
    }
    HudDisplaySettings hud;
    if (const auto* exact = FindHudDisplaySettings(snapshot->settings, displayId)) {
        hud = *exact;
    } else if (const auto* fallback =
                   FindHudDisplaySettings(snapshot->settings, "default")) {
        hud = *fallback;
        hud.displayId = displayId;
    } else {
        hud.displayId = displayId;
    }
    if (m_platform->havePendingDrag) {
        const auto now = std::chrono::steady_clock::now();
        if (m_platform->dragDisplayId != displayId) {
            m_platform->dragActive = false;
            m_platform->havePendingDrag = false;
        } else {
            if (m_platform->dragActive && !hud.editMode) {
                m_platform->dragActive = false;
                m_platform->dragReleasedAt = now;
            }
            const bool acknowledged =
                std::abs(hud.offsetX - m_platform->dragOffsetX) < 0.01f &&
                std::abs(hud.offsetY - m_platform->dragOffsetY) < 0.01f;
            const bool graceExpired =
                !m_platform->dragActive &&
                m_platform->dragReleasedAt.time_since_epoch().count() != 0 &&
                now - m_platform->dragReleasedAt >= kPendingDragGrace;
            if (!m_platform->dragActive && (acknowledged || graceExpired)) {
                m_platform->havePendingDrag = false;
            } else {
                hud.offsetX = m_platform->dragOffsetX;
                hud.offsetY = m_platform->dragOffsetY;
            }
        }
    }

    m_platform->editMode = hud.editMode;
    LONG_PTR exStyle = GetWindowLongPtrW(m_platform->window, GWL_EXSTYLE);
    const bool transparent = (exStyle & WS_EX_TRANSPARENT) != 0;
    if (hud.editMode == transparent) {
        exStyle = hud.editMode
            ? (exStyle & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT))
            : (exStyle | WS_EX_TRANSPARENT);
        SetWindowLongPtrW(m_platform->window, GWL_EXSTYLE, exStyle);
    }

    MSG message{};
    while (PeekMessageW(&message, m_platform->window, 0, 0, PM_REMOVE)) {
        if (message.message == WM_HOTKEY && message.wParam == kHudHotkeyId) {
            hud.visible = !hud.visible;
            if (m_config.commands) {
                m_config.commands->Emplace<SetHudVisibleCommand>(hud.visible);
            }
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    DWORD foregroundProcessId = 0;
    if (const HWND foreground = GetForegroundWindow()) {
        GetWindowThreadProcessId(foreground, &foregroundProcessId);
    }
    const bool dashboardForeground =
        foregroundProcessId == GetCurrentProcessId();
    if (!hud.visible ||
        (dashboardForeground && !hud.editMode)) {
        ShowWindow(m_platform->window, SW_HIDE);
        return;
    }

    const RECT& screen = monitorInfo.rcMonitor;
    SetWindowPos(m_platform->window, HWND_TOPMOST, screen.left, screen.top,
                 screen.right - screen.left, screen.bottom - screen.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ImGui::GetIO().FontGlobalScale = dpiScale;
    const HudRect bounds = ResolveHudBounds(
        hud, work, 300.0f * dpiScale, 300.0f * dpiScale);
    m_platform->interactiveDesktopRect = {
        static_cast<LONG>(bounds.x), static_cast<LONG>(bounds.y),
        static_cast<LONG>(bounds.x + bounds.width),
        static_cast<LONG>(bounds.y + bounds.height),
    };
    const ImVec2 localTopLeft{
        bounds.x - static_cast<float>(screen.left),
        bounds.y - static_cast<float>(screen.top),
    };
    const ImVec2 center{localTopLeft.x + bounds.width * 0.5f,
                        localTopLeft.y + bounds.height * 0.5f};
    const float radius = std::min(bounds.width, bounds.height) * 0.37f;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    if (hud.editMode) {
        const ImVec2 bottomRight{
            localTopLeft.x + bounds.width,
            localTopLeft.y + bounds.height,
        };
        if (hud.previewBackground == HudPreviewBackground::TacticalDark) {
            drawList->AddRectFilled(localTopLeft, bottomRight,
                                    IM_COL32(10, 16, 20, 205), 14.0f);
        } else if (hud.previewBackground == HudPreviewBackground::Light) {
            drawList->AddRectFilled(localTopLeft, bottomRight,
                                    IM_COL32(220, 224, 226, 210), 14.0f);
        } else if (hud.previewBackground == HudPreviewBackground::Checkerboard) {
            constexpr float tile = 18.0f;
            for (float y = 0.0f; y < bounds.height; y += tile) {
                for (float x = 0.0f; x < bounds.width; x += tile) {
                    const int parity =
                        static_cast<int>(x / tile + y / tile) & 1;
                    drawList->AddRectFilled(
                        {localTopLeft.x + x, localTopLeft.y + y},
                        {std::min(bottomRight.x, localTopLeft.x + x + tile),
                         std::min(bottomRight.y, localTopLeft.y + y + tile)},
                        parity ? IM_COL32(46, 53, 57, 220)
                               : IM_COL32(91, 101, 106, 220));
                }
            }
        }
        drawList->AddRect(localTopLeft,
            bottomRight,
            IM_COL32(255, 183, 45, 255), 14.0f, 0, 2.0f);
        drawList->AddText({localTopLeft.x + 10.0f, localTopLeft.y + 8.0f},
                          IM_COL32(255, 183, 45, 255),
                          "EDIT MODE - DRAG TO POSITION");
    }

    const float opacity = hud.opacity;
    drawList->AddCircle(center, radius,
                        Color(hud.inactiveColor, opacity), 96, 2.0f);
    drawList->AddCircle(center, radius * 0.66f,
                        Color(hud.inactiveColor, opacity * 0.65f), 72, 1.0f);
    for (int azimuth = 0; azimuth < 360; azimuth += 45) {
        drawList->AddLine(Point(center, radius * 0.86f,
                                static_cast<float>(azimuth)),
                          Point(center, radius, static_cast<float>(azimuth)),
                          Color(hud.inactiveColor, opacity), 1.0f);
    }

    const float sensitivity = snapshot->settings.radar.sensitivityDbfs;
    const RadarFrame& frame = snapshot->radar.frame;
    if (snapshot->radar.mode != RadarMode::Events &&
        frame.layout.IsDirectional()) {
        for (size_t index = 0; index < frame.sectorActivitiesDbfs.size(); ++index) {
            const float intensity = Intensity(
                frame.sectorActivitiesDbfs[index], sensitivity);
            if (intensity <= 0.01f) continue;
            const float azimuth = static_cast<float>(index) * 15.0f;
            const ImVec2 start = Point(center, radius * 0.42f, azimuth);
            const ImVec2 end = Point(
                center, radius * (0.48f + 0.50f * intensity), azimuth);
            drawList->AddLine(start, end,
                Color(hud.sectorColor, opacity * (0.25f + 0.75f * intensity)),
                3.0f + 5.0f * intensity);
        }
    }

    std::optional<float> newestEventAzimuth;
    if (snapshot->capture.state == AudioCaptureState::Running &&
        snapshot->radar.mode != RadarMode::Continuous) {
        const double nowAudio = static_cast<double>(
            snapshot->capture.streamSample) / 48000.0;
        for (const RecentEventSnapshot& event : snapshot->recentEvents) {
            if (event.streamGeneration != snapshot->capture.streamGeneration ||
                event.peakCount == 0 || nowAudio < event.timestampSeconds) {
                continue;
            }
            const float age = static_cast<float>(
                nowAudio - event.timestampSeconds);
            if (age > hud.persistenceSeconds) continue;
            const float fade = hud.persistenceSeconds > 0.0f
                ? std::clamp(1.0f - age / hud.persistenceSeconds, 0.0f, 1.0f)
                : 1.0f;
            for (uint32_t index = 0; index < event.peakCount; ++index) {
                DrawArc(drawList, center, radius,
                        event.peaks[index].azimuthDegrees,
                        std::clamp(event.peaks[index].angularUncertaintyDegrees,
                                   4.0f, 45.0f),
                        Color(hud.sectorColor,
                              opacity * fade *
                                  std::max(0.18f, event.peaks[index].confidence)),
                        7.0f);
            }
            newestEventAzimuth = event.strongestAzimuthDegrees;
        }
    }

    const bool useContinuousArrow =
        snapshot->capture.state == AudioCaptureState::Running &&
        snapshot->radar.mode != RadarMode::Events &&
        frame.status == RadarRuntimeStatus::Active;
    const std::optional<float> arrowAzimuth = useContinuousArrow
        ? std::optional<float>(frame.strongestAzimuthDegrees)
        : newestEventAzimuth;
    if (arrowAzimuth) {
        const ImVec2 tip = Point(center, radius * 0.92f,
                                 *arrowAzimuth);
        const ImVec2 left = Point(center, radius * 0.68f,
                                  *arrowAzimuth - 6.0f);
        const ImVec2 right = Point(center, radius * 0.68f,
                                   *arrowAzimuth + 6.0f);
        drawList->AddTriangleFilled(tip, left, right,
                                    Color(hud.strongestColor, opacity));
    }
    drawList->AddCircleFilled(center, 3.0f,
                              IM_COL32(230, 238, 240,
                                       static_cast<int>(220 * opacity)));

    if (hud.showCardinalLabels) {
        const auto text = [&](const char* value, float azimuth) {
            const ImVec2 point = Point(center, radius + 12.0f, azimuth);
            const ImVec2 size = ImGui::CalcTextSize(value);
            drawList->AddText({point.x - size.x * 0.5f,
                               point.y - size.y * 0.5f},
                              IM_COL32(210, 222, 225,
                                       static_cast<int>(220 * opacity)), value);
        };
        text(hud.showDegreeLabels ? "FRONT 0" : "FRONT", 0.0f);
        text(hud.showDegreeLabels ? "R 90" : "R", 90.0f);
        text(hud.showDegreeLabels ? "REAR 180" : "REAR", 180.0f);
        text(hud.showDegreeLabels ? "L 270" : "L", 270.0f);
    }

    char preset[64]{};
    std::snprintf(preset, sizeof(preset), "%s / %s",
                  ToString(snapshot->radar.mode),
                  ToString(snapshot->radar.preset));
    drawList->AddText({localTopLeft.x + 12.0f,
                       localTopLeft.y + bounds.height - 28.0f},
                      IM_COL32(140, 211, 220,
                               static_cast<int>(220 * opacity)), preset);
    const bool captureRunning =
        snapshot->capture.state == AudioCaptureState::Running;
    const bool captureHealthy = captureRunning &&
        snapshot->layout.directionalRadarAvailable && !snapshot->error;
    drawList->AddText({localTopLeft.x + bounds.width - 108.0f,
                       localTopLeft.y + bounds.height - 28.0f},
                      captureHealthy
                          ? IM_COL32(30, 220, 230,
                                    static_cast<int>(230 * opacity))
                          : Color(hud.errorColor, opacity),
                      captureHealthy ? "+ RADAR"
                          : (captureRunning ? "! RADAR" : "! AUDIO"));

    if (hud.editMode) {
        ImGui::SetNextWindowPos(localTopLeft);
        ImGui::SetNextWindowSize({bounds.width, bounds.height});
        constexpr ImGuiWindowFlags dragFlags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin("HudDragSurface", nullptr, dragFlags);
        ImGui::InvisibleButton("HudDrag", ImGui::GetContentRegionAvail());
        if (ImGui::IsItemActivated()) {
            m_platform->dragActive = true;
            m_platform->havePendingDrag = true;
            m_platform->dragDisplayId = displayId;
            m_platform->dragOffsetX = hud.offsetX;
            m_platform->dragOffsetY = hud.offsetY;
            m_platform->dragReleasedAt = {};
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const ImVec2 delta = ImGui::GetIO().MouseDelta;
            if ((delta.x != 0.0f || delta.y != 0.0f) && m_config.commands) {
                m_platform->dragOffsetX += delta.x;
                m_platform->dragOffsetY += delta.y;
                m_config.commands->Emplace<SetHudOffsetCommand>(
                    displayId, m_platform->dragOffsetX,
                    m_platform->dragOffsetY);
            }
        }
        if (ImGui::IsItemDeactivated()) {
            m_platform->dragActive = false;
            m_platform->dragReleasedAt = std::chrono::steady_clock::now();
        }
        ImGui::End();
    }

    ImGui::Render();
    const float clear[4]{0.0f, 0.0f, 0.0f, 0.0f};
    m_platform->deviceContext->OMSetRenderTargets(
        1, &m_platform->renderTarget, nullptr);
    m_platform->deviceContext->ClearRenderTargetView(
        m_platform->renderTarget, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    m_platform->swapChain->Present(1, 0);
#endif
}

} // namespace EchoRadar
