#include "OverlayRenderer.h"
#include "DashboardTheme.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <optional>
#include <utility>

#if defined(_WIN32) && !defined(ECHORADAR_HEADLESS_RENDERER)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d11.h>
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

#if defined(_WIN32) && !defined(ECHORADAR_HEADLESS_RENDERER)
struct OverlayRenderer::PlatformImpl {
    HWND window{nullptr};
    ID3D11Device* device{nullptr};
    ID3D11DeviceContext* deviceContext{nullptr};
    IDXGISwapChain* swapChain{nullptr};
    ID3D11RenderTargetView* renderTarget{nullptr};
    HINSTANCE instance{nullptr};
    bool classRegistered{false};
    bool imguiInitialized{false};
    bool windowDestroyed{false};
    ImGuiContext* imguiContext{nullptr};
    ImGuiStyle baseStyle{};
    int observedWindowX{100};
    int observedWindowY{100};
    int observedClientWidth{1280};
    int observedClientHeight{800};
    UINT observedDpi{96};
    bool observedMaximized{false};
    bool haveObservedPlacement{false};
    bool placementSavePending{false};
    std::chrono::steady_clock::time_point lastPlacementChange{};
};

namespace {

constexpr wchar_t kWindowClassName[] = L"EchoRadarV2DashboardWindow";
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

    // ImGui does not use Windows font linking. Merge a compact CJK range from
    // the system UI fallback so UTF-8 endpoint names remain readable on Asian
    // Windows installations without redistributing a font with EchoRadar.
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

void CreateRenderTarget(OverlayRenderer::PlatformImpl& platform) {
    if (!platform.swapChain || !platform.device) return;
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(platform.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        platform.device->CreateRenderTargetView(
            backBuffer, nullptr, &platform.renderTarget);
        backBuffer->Release();
    }
}

void CleanupRenderTarget(OverlayRenderer::PlatformImpl& platform) {
    if (platform.renderTarget) {
        platform.renderTarget->Release();
        platform.renderTarget = nullptr;
    }
}

bool CreateDevice(OverlayRenderer::PlatformImpl& platform) {
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 2;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferDesc.RefreshRate.Numerator = 60;
    description.BufferDesc.RefreshRate.Denominator = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = platform.window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[]{
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selected{};
    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
        ARRAYSIZE(levels), D3D11_SDK_VERSION, &description,
        &platform.swapChain, &platform.device, &selected,
        &platform.deviceContext);
    if (FAILED(result)) {
        result = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels,
            ARRAYSIZE(levels), D3D11_SDK_VERSION, &description,
            &platform.swapChain, &platform.device, &selected,
            &platform.deviceContext);
    }
    if (FAILED(result)) return false;
    CreateRenderTarget(platform);
    return platform.renderTarget != nullptr;
}

void CleanupDevice(OverlayRenderer::PlatformImpl& platform) {
    CleanupRenderTarget(platform);
    if (platform.swapChain) platform.swapChain->Release();
    if (platform.deviceContext) platform.deviceContext->Release();
    if (platform.device) platform.device->Release();
    platform.swapChain = nullptr;
    platform.deviceContext = nullptr;
    platform.device = nullptr;
}

void ObservePlacement(OverlayRenderer::PlatformImpl& platform,
                      HWND window) {
    if (!platform.imguiInitialized || !window || IsIconic(window)) return;
    const bool maximized = IsZoomed(window) != FALSE;
    int x = platform.observedWindowX;
    int y = platform.observedWindowY;
    int clientWidth = platform.observedClientWidth;
    int clientHeight = platform.observedClientHeight;
    const UINT dpi = GetDpiForWindow(window);
    const UINT effectiveDpi = dpi ? dpi : 96u;
    if (!maximized) {
        RECT windowRect{};
        RECT clientRect{};
        if (GetWindowRect(window, &windowRect) &&
            GetClientRect(window, &clientRect)) {
            x = windowRect.left;
            y = windowRect.top;
            clientWidth = MulDiv(
                clientRect.right - clientRect.left, 96, effectiveDpi);
            clientHeight = MulDiv(
                clientRect.bottom - clientRect.top, 96, effectiveDpi);
        }
    }
    const bool changed = !platform.haveObservedPlacement ||
        x != platform.observedWindowX || y != platform.observedWindowY ||
        clientWidth != platform.observedClientWidth ||
        clientHeight != platform.observedClientHeight ||
        maximized != platform.observedMaximized ||
        (dpi != 0 && dpi != platform.observedDpi);
    if (!changed) return;

    platform.observedWindowX = x;
    platform.observedWindowY = y;
    platform.observedClientWidth = clientWidth;
    platform.observedClientHeight = clientHeight;
    platform.observedMaximized = maximized;
    if (dpi != 0) platform.observedDpi = dpi;
    if (platform.haveObservedPlacement) {
        platform.placementSavePending = true;
        platform.lastPlacementChange = std::chrono::steady_clock::now();
    }
    platform.haveObservedPlacement = true;
}

LRESULT WINAPI WindowProc(HWND window, UINT message,
                          WPARAM wParam, LPARAM lParam) {
    auto* platform = reinterpret_cast<OverlayRenderer::PlatformImpl*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        platform = static_cast<OverlayRenderer::PlatformImpl*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(platform));
    }
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, wParam, lParam);
    }
    if (platform && platform->imguiInitialized && platform->imguiContext) {
        const ScopedImGuiContext context(platform->imguiContext);
        if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) {
            return true;
        }
    }
    switch (message) {
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = GetDpiForWindow(window);
        RECT minimum{0, 0,
                     MulDiv(1100, dpi ? dpi : 96u, 96),
                     MulDiv(700, dpi ? dpi : 96u, 96)};
        AdjustWindowRectExForDpi(
            &minimum, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi ? dpi : 96u);
        limits->ptMinTrackSize.x = minimum.right - minimum.left;
        limits->ptMinTrackSize.y = minimum.bottom - minimum.top;
        return 0;
    }
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        return 0;
    }
    case WM_SIZE:
        if (platform && platform->device && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget(*platform);
            platform->swapChain->ResizeBuffers(
                0, static_cast<UINT>(LOWORD(lParam)),
                static_cast<UINT>(HIWORD(lParam)), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget(*platform);
        }
        if (platform && wParam != SIZE_MINIMIZED) {
            ObservePlacement(*platform, window);
        }
        return 0;
    case WM_MOVE:
        if (platform) ObservePlacement(*platform, window);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        if (platform) {
            platform->windowDestroyed = true;
            PostQuitMessage(0);
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace
#else
struct OverlayRenderer::PlatformImpl {};
#endif

OverlayRenderer::OverlayRenderer() : OverlayRenderer(Config{}) {}
OverlayRenderer::OverlayRenderer(Config config) : m_config(std::move(config)) {}
OverlayRenderer::~OverlayRenderer() { Shutdown(); }

bool OverlayRenderer::Initialise() {
#if defined(_WIN32) && !defined(ECHORADAR_HEADLESS_RENDERER)
    if (m_running) return true;
    m_platform = std::make_unique<PlatformImpl>();
    m_platform->instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = m_platform->instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClassName;
    if (RegisterClassExW(&windowClass)) {
        m_platform->classRegistered = true;
    } else if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        m_platform.reset();
        return false;
    }

    RECT rectangle{0, 0, std::max(1100, m_config.windowWidth),
                    std::max(700, m_config.windowHeight)};
    AdjustWindowRectEx(&rectangle, WS_OVERLAPPEDWINDOW, FALSE, 0);
    const int width = rectangle.right - rectangle.left;
    const int height = rectangle.bottom - rectangle.top;
    int x = m_config.windowX;
    int y = m_config.windowY;
    RECT requested{x, y, x + width, y + height};
    if (!MonitorFromRect(&requested, MONITOR_DEFAULTTONULL)) {
        x = std::max(0, (GetSystemMetrics(SM_CXSCREEN) - width) / 2);
        y = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - height) / 2);
    }
    m_platform->window = CreateWindowExW(
        WS_EX_APPWINDOW, kWindowClassName, L"EchoRadar v2 - Directional Radar",
        WS_OVERLAPPEDWINDOW, x, y, width, height, nullptr, nullptr,
        m_platform->instance, m_platform.get());
    if (!m_platform->window) {
        Shutdown();
        return false;
    }
    const UINT initialDpi = GetDpiForWindow(m_platform->window);
    const UINT effectiveDpi = initialDpi ? initialDpi : 96u;
    const int logicalClientWidth = std::max(1100, m_config.windowWidth);
    const int logicalClientHeight = std::max(700, m_config.windowHeight);
    RECT dpiRectangle{
        0, 0,
        MulDiv(logicalClientWidth, effectiveDpi, 96),
        MulDiv(logicalClientHeight, effectiveDpi, 96)};
    AdjustWindowRectExForDpi(
        &dpiRectangle, WS_OVERLAPPEDWINDOW, FALSE, 0, effectiveDpi);
    SetWindowPos(
        m_platform->window, nullptr, x, y,
        dpiRectangle.right - dpiRectangle.left,
        dpiRectangle.bottom - dpiRectangle.top,
        SWP_NOACTIVATE | SWP_NOZORDER);
    if (!CreateDevice(*m_platform)) {
        Shutdown();
        return false;
    }

    IMGUI_CHECKVERSION();
    m_platform->imguiContext = ImGui::CreateContext();
    {
        const ScopedImGuiContext context(m_platform->imguiContext);
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;
        AddUiFont(io, 16.0f);

        ApplyDashboardTheme();
        ImGuiStyle& style = ImGui::GetStyle();
        m_platform->baseStyle = style;

        ImGui_ImplWin32_Init(m_platform->window);
        ImGui_ImplDX11_Init(m_platform->device, m_platform->deviceContext);
        m_platform->imguiInitialized = true;
    }
    RECT initialWindowRect{};
    GetWindowRect(m_platform->window, &initialWindowRect);
    m_platform->observedWindowX = initialWindowRect.left;
    m_platform->observedWindowY = initialWindowRect.top;
    m_platform->observedClientWidth = logicalClientWidth;
    m_platform->observedClientHeight = logicalClientHeight;
    m_platform->observedDpi = effectiveDpi;
    m_platform->observedMaximized = m_config.maximized;
    m_platform->haveObservedPlacement = true;
    ShowWindow(m_platform->window,
               m_config.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    UpdateWindow(m_platform->window);
    m_running = true;
    return true;
#else
    std::cout << "[EchoRadar] Dashboard is available only in the Windows build.\n";
    return false;
#endif
}

void OverlayRenderer::Shutdown() {
#if defined(_WIN32) && !defined(ECHORADAR_HEADLESS_RENDERER)
    if (m_platform) {
        ImGuiContext* context = m_platform->imguiContext;
        ImGuiContext* previous = ImGui::GetCurrentContext();
        if (m_platform->window && IsWindow(m_platform->window)) {
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

void OverlayRenderer::FlushPendingPlacement() {
#if defined(_WIN32) && !defined(ECHORADAR_HEADLESS_RENDERER)
    if (!m_platform || !m_platform->placementSavePending ||
        !m_config.commands) {
        return;
    }
    m_config.commands->Emplace<UpdateDashboardPlacementCommand>(
        static_cast<float>(m_platform->observedWindowX),
        static_cast<float>(m_platform->observedWindowY),
        static_cast<float>(m_platform->observedClientWidth),
        static_cast<float>(m_platform->observedClientHeight),
        m_platform->observedMaximized);
    m_platform->placementSavePending = false;
#endif
}

void OverlayRenderer::Render() {
#if defined(_WIN32) && !defined(ECHORADAR_HEADLESS_RENDERER)
    if (!m_running || !m_platform || !m_platform->imguiInitialized) return;
    if (m_platform->windowDestroyed || !IsWindow(m_platform->window)) {
        FlushPendingPlacement();
        m_running = false;
        return;
    }
    const ScopedImGuiContext context(m_platform->imguiContext);
    MSG message{};
    while (PeekMessageW(&message, m_platform->window, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            FlushPendingPlacement();
            m_running = false;
            return;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
        if (m_platform->windowDestroyed || !IsWindow(m_platform->window)) {
            FlushPendingPlacement();
            m_running = false;
            return;
        }
    }
    if (PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {
        FlushPendingPlacement();
        m_running = false;
        return;
    }

    const std::shared_ptr<const AppSnapshot> snapshot = m_config.snapshots
        ? m_config.snapshots->Latest() : std::make_shared<AppSnapshot>();
    const UINT windowDpi = GetDpiForWindow(m_platform->window);
    const float dpiScale = static_cast<float>(windowDpi ? windowDpi : 96u) /
        96.0f;
    ApplyUiScale(snapshot->settings.uiScale * dpiScale);

    if (!IsIconic(m_platform->window)) {
        ObservePlacement(*m_platform, m_platform->window);
        if (m_platform->placementSavePending && m_config.commands &&
            std::chrono::steady_clock::now() -
                    m_platform->lastPlacementChange >=
                std::chrono::milliseconds(250)) {
            FlushPendingPlacement();
        }
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawDashboard(*snapshot);
    ImGui::Render();

    const float clear[4]{0.025f, 0.031f, 0.037f, 1.0f};
    m_platform->deviceContext->OMSetRenderTargets(
        1, &m_platform->renderTarget, nullptr);
    m_platform->deviceContext->ClearRenderTargetView(
        m_platform->renderTarget, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    m_platform->swapChain->Present(1, 0);
#endif
}

void OverlayRenderer::ApplyUiScale(float scale) {
#if defined(_WIN32) && !defined(ECHORADAR_HEADLESS_RENDERER)
    if (!m_platform) return;
    if (!std::isfinite(scale)) scale = AppSettings::kDefaultUiScale;
    scale = std::clamp(scale, AppSettings::kMinUiScale,
                       AppSettings::kMaxUiScale * 4.0f);
    if (std::abs(scale - m_appliedUiScale) < 0.001f) return;
    ImGuiStyle style = m_platform->baseStyle;
    style.ScaleAllSizes(scale);
    ImGui::GetStyle() = style;
    ImGui::GetIO().FontGlobalScale = scale;
    m_appliedUiScale = scale;
#else
    (void)scale;
#endif
}

} // namespace EchoRadar
