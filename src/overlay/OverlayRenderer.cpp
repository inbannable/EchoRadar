#include "OverlayRenderer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <optional>
#include <utility>

#ifdef _WIN32

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

#ifdef _WIN32
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
constexpr float kPi = 3.14159265358979323846f;
constexpr ImVec4 kCyan{0.10f, 0.82f, 0.92f, 1.0f};
constexpr ImVec4 kAmber{1.0f, 0.66f, 0.16f, 1.0f};
constexpr ImVec4 kRed{1.0f, 0.28f, 0.25f, 1.0f};
constexpr ImVec4 kMuted{0.50f, 0.58f, 0.65f, 1.0f};

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

const char* CaptureStateText(AudioCaptureState state) {
    switch (state) {
    case AudioCaptureState::Stopped: return "STOPPED";
    case AudioCaptureState::Starting: return "STARTING";
    case AudioCaptureState::Running: return "CAPTURING";
    case AudioCaptureState::Recovering: return "RECOVERING";
    case AudioCaptureState::Failed: return "FAILED";
    case AudioCaptureState::Unsupported: return "UNSUPPORTED";
    }
    return "FAILED";
}

const char* ModelStateText(ModelUiState state) {
    switch (state) {
    case ModelUiState::Disabled: return "MODEL OFF";
    case ModelUiState::Loading: return "MODEL LOADING";
    case ModelUiState::Ready: return "MODEL READY";
    case ModelUiState::Missing: return "MODEL MISSING";
    case ModelUiState::Malformed: return "MODEL INVALID";
    case ModelUiState::Failed: return "MODEL FAILED";
    }
    return "MODEL OFF";
}

ImVec4 CaptureStateColor(AudioCaptureState state) {
    if (state == AudioCaptureState::Running) return kCyan;
    if (state == AudioCaptureState::Starting ||
        state == AudioCaptureState::Recovering) return kAmber;
    if (state == AudioCaptureState::Failed) return kRed;
    return kMuted;
}

void StatusChip(const char* shape, const char* text, const ImVec4& color) {
    ImGui::TextColored(color, "%s %s", shape, text);
}

float DbIntensity(float db, float sensitivity) {
    if (!std::isfinite(db) || db <= kRadarSilenceDbfs + 0.1f) return 0.0f;
    const float denominator = std::max(1.0f, -sensitivity);
    return std::clamp((db - sensitivity) / denominator, 0.0f, 1.0f);
}

ImVec2 DirectionPoint(ImVec2 center, float radius, float azimuthDegrees) {
    const float radians = (azimuthDegrees - 90.0f) * kPi / 180.0f;
    return {center.x + std::cos(radians) * radius,
            center.y + std::sin(radians) * radius};
}

void DrawMiniRadar(ImDrawList* drawList, ImVec2 center, float radius,
                   const std::array<float, kRadarSectorCount>& sectors,
                   float sensitivity) {
    drawList->AddCircle(center, radius, IM_COL32(90, 109, 120, 180), 0, 1.0f);
    for (size_t index = 0; index < sectors.size(); ++index) {
        const float intensity = DbIntensity(sectors[index], sensitivity);
        if (intensity <= 0.01f) continue;
        const float azimuth = static_cast<float>(index) * 15.0f;
        const ImVec2 end = DirectionPoint(
            center, radius * (0.35f + 0.65f * intensity), azimuth);
        drawList->AddLine(center, end,
                          IM_COL32(30, 215, 235,
                                   static_cast<int>(80 + 175 * intensity)),
                          1.5f);
    }
}

const char* SetupStepText(SetupStep step) {
    switch (step) {
    case SetupStep::SelectEndpoint: return "1 / 5  Select playback endpoint";
    case SetupStep::ValidateFormat: return "2 / 5  Validate native format";
    case SetupStep::ConfirmChannels: return "3 / 5  Confirm channel activity";
    case SetupStep::PreviewHud: return "4 / 5  Preview and position HUD";
    case SetupStep::Complete: return "5 / 5  Setup complete";
    }
    return "Setup";
}

} // namespace
#else
struct OverlayRenderer::PlatformImpl {};
#endif

OverlayRenderer::OverlayRenderer() : OverlayRenderer(Config{}) {}
OverlayRenderer::OverlayRenderer(Config config) : m_config(std::move(config)) {}
OverlayRenderer::~OverlayRenderer() { Shutdown(); }

bool OverlayRenderer::Initialise() {
#ifdef _WIN32
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

        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 10.0f;
        style.ChildRounding = 8.0f;
        style.FrameRounding = 5.0f;
        style.PopupRounding = 6.0f;
        style.GrabRounding = 5.0f;
        style.ScrollbarRounding = 8.0f;
        style.ItemSpacing = ImVec2(9.0f, 7.0f);
        style.FramePadding = ImVec2(9.0f, 6.0f);
        style.Colors[ImGuiCol_WindowBg] =
            ImVec4(0.035f, 0.043f, 0.052f, 1.0f);
        style.Colors[ImGuiCol_ChildBg] =
            ImVec4(0.055f, 0.066f, 0.078f, 1.0f);
        style.Colors[ImGuiCol_PopupBg] =
            ImVec4(0.045f, 0.055f, 0.065f, 0.98f);
        style.Colors[ImGuiCol_Border] =
            ImVec4(0.18f, 0.23f, 0.26f, 0.85f);
        style.Colors[ImGuiCol_FrameBg] =
            ImVec4(0.085f, 0.105f, 0.12f, 1.0f);
        style.Colors[ImGuiCol_FrameBgHovered] =
            ImVec4(0.11f, 0.18f, 0.20f, 1.0f);
        style.Colors[ImGuiCol_Button] =
            ImVec4(0.08f, 0.20f, 0.22f, 1.0f);
        style.Colors[ImGuiCol_ButtonHovered] =
            ImVec4(0.08f, 0.34f, 0.37f, 1.0f);
        style.Colors[ImGuiCol_Header] =
            ImVec4(0.07f, 0.22f, 0.24f, 1.0f);
        style.Colors[ImGuiCol_CheckMark] = kCyan;
        style.Colors[ImGuiCol_SliderGrab] = kCyan;
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
#ifdef _WIN32
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
#ifdef _WIN32
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
#ifdef _WIN32
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
#ifdef _WIN32
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

#ifdef _WIN32

void OverlayRenderer::DrawDashboard(const AppSnapshot& snapshot) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("EchoRadar v2", nullptr, flags);
    DrawHeader(snapshot);
    DrawInlineStatus(snapshot);
    DrawRadarWorkspace(snapshot);
    DrawRecentEvents(snapshot);

    ImGui::SetNextItemOpen(!snapshot.settings.dashboard.audioSetupCollapsed,
                           ImGuiCond_Appearing);
    const bool audioOpen = ImGui::CollapsingHeader(
        "Audio Setup", ImGuiTreeNodeFlags_DefaultOpen);
    if (ImGui::IsItemToggledOpen() && m_config.commands) {
        m_config.commands->Emplace<SetDashboardSectionCollapsedCommand>(
            DashboardSection::AudioSetup, !audioOpen);
    }
    if (audioOpen) DrawAudioSetup(snapshot);

    ImGui::SetNextItemOpen(!snapshot.settings.dashboard.eventDetailsCollapsed,
                           ImGuiCond_Appearing);
    const bool eventsOpen = ImGui::CollapsingHeader("Event Details");
    if (ImGui::IsItemToggledOpen() && m_config.commands) {
        m_config.commands->Emplace<SetDashboardSectionCollapsedCommand>(
            DashboardSection::EventDetails, !eventsOpen);
    }
    if (eventsOpen) DrawEventDetails(snapshot);

    ImGui::SetNextItemOpen(!snapshot.settings.dashboard.hudEditorCollapsed,
                           ImGuiCond_Appearing);
    const bool hudOpen = ImGui::CollapsingHeader("HUD Editor");
    if (ImGui::IsItemToggledOpen() && m_config.commands) {
        m_config.commands->Emplace<SetDashboardSectionCollapsedCommand>(
            DashboardSection::HudEditor, !hudOpen);
    }
    if (hudOpen) DrawHudEditor(snapshot);

    ImGui::SetNextItemOpen(!snapshot.settings.dashboard.advancedCollapsed,
                           ImGuiCond_Appearing);
    const bool advancedOpen = ImGui::CollapsingHeader("Advanced");
    if (ImGui::IsItemToggledOpen() && m_config.commands) {
        m_config.commands->Emplace<SetDashboardSectionCollapsedCommand>(
            DashboardSection::Advanced, !advancedOpen);
    }
    if (advancedOpen) DrawAdvanced(snapshot);
    ImGui::End();
}

void OverlayRenderer::DrawHeader(const AppSnapshot& snapshot) {
    ImGui::TextColored(kCyan, "ECHO RADAR");
    ImGui::SameLine();
    ImGui::TextDisabled("V2 / MULTICHANNEL RESEARCH");
    ImGui::SameLine();
    ImGui::TextDisabled("rev %llu",
                        static_cast<unsigned long long>(snapshot.revision));
    ImGui::Separator();

    if (ImGui::BeginTable("StatusHeader", 5,
                          ImGuiTableFlags_SizingStretchSame |
                              ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        StatusChip(snapshot.capture.state == AudioCaptureState::Running ? "[+]" : "[-]",
                   CaptureStateText(snapshot.capture.state),
                   CaptureStateColor(snapshot.capture.state));
        ImGui::TextDisabled("%s", snapshot.capture.endpointName.empty()
            ? "No endpoint" : snapshot.capture.endpointName.c_str());

        ImGui::TableNextColumn();
        StatusChip(snapshot.layout.directionalRadarAvailable ? "[+]" : "[!]",
                   ToString(snapshot.layout.detected.kind),
                   snapshot.layout.directionalRadarAvailable ? kCyan : kAmber);
        ImGui::TextDisabled("%u ch / %u Hz",
                            snapshot.layout.detected.channelCount,
                            snapshot.capture.sampleRate);

        ImGui::TableNextColumn();
        StatusChip("[*]", ToString(snapshot.radar.mode),
                   snapshot.layout.directionalRadarAvailable ? kCyan : kMuted);
        ImGui::TextDisabled("%s emphasis", ToString(snapshot.radar.preset));

        ImGui::TableNextColumn();
        StatusChip(snapshot.model.state == ModelUiState::Ready ? "[+]" : "[!]",
                   ModelStateText(snapshot.model.state),
                   snapshot.model.state == ModelUiState::Ready ? kCyan : kMuted);
        ImGui::TextDisabled("%s", snapshot.model.version.empty()
            ? "No model required" : snapshot.model.version.c_str());

        ImGui::TableNextColumn();
        const bool hudVisible = snapshot.hud.state == HudUiState::Visible ||
                                snapshot.hud.state == HudUiState::Editing;
        StatusChip(snapshot.hud.state == HudUiState::Editing ? "[E]" : "[H]",
                   snapshot.hud.state == HudUiState::Editing
                       ? "HUD EDIT" : (hudVisible ? "HUD ON" : "HUD HIDDEN"),
                   hudVisible ? kCyan : kMuted);
        ImGui::TextDisabled("%s", snapshot.hud.state == HudUiState::Editing
            ? "Pointer unlocked"
            : (hudVisible ? "Hidden over dashboard" : "Click-through"));
        ImGui::EndTable();
    }
}

void OverlayRenderer::DrawInlineStatus(const AppSnapshot& snapshot) {
    if (!snapshot.settings.onboarding.completed) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.11f, 0.09f, 0.035f, 1.0f));
        ImGui::BeginChild("SetupBanner", ImVec2(0.0f, 54.0f), true);
        ImGui::TextColored(kAmber, "[!] FIRST-RUN SETUP");
        ImGui::SameLine();
        ImGui::TextUnformatted(SetupStepText(snapshot.settings.onboarding.step));
        ImGui::TextDisabled("Progress is saved. Recognition and settings remain available while surround radar is unavailable.");
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    if (snapshot.error) {
        const ImVec4 color = snapshot.error.kind == InlineErrorKind::CaptureFailure ||
            snapshot.error.kind == InlineErrorKind::MalformedModel ? kRed : kAmber;
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
                              ImVec4(color.x * 0.12f, color.y * 0.12f,
                                     color.z * 0.12f, 1.0f));
        ImGui::BeginChild("InlineStatus", ImVec2(0.0f, 62.0f), true);
        ImGui::TextColored(color, "%s %s",
                           snapshot.error.kind == InlineErrorKind::Loading ? "~" : "!",
                           snapshot.error.message.c_str());
        if (!snapshot.error.recoveryAction.empty()) {
            ImGui::TextDisabled("%s", snapshot.error.recoveryAction.c_str());
        }
        if (snapshot.error.recoverable && m_config.commands) {
            ImGui::SameLine(ImGui::GetContentRegionMax().x - 130.0f);
            if (ImGui::Button("Retry capture")) {
                m_config.commands->Emplace<RetryCaptureCommand>();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}

void OverlayRenderer::DrawRadarWorkspace(const AppSnapshot& snapshot) {
    const float height = std::clamp(ImGui::GetContentRegionAvail().y * 0.48f,
                                    330.0f, 520.0f);
    if (ImGui::BeginTable("LiveWorkspace", 2,
                          ImGuiTableFlags_Resizable |
                              ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Radar", ImGuiTableColumnFlags_WidthStretch, 2.15f);
        ImGui::TableSetupColumn("Quick controls", ImGuiTableColumnFlags_WidthStretch, 0.85f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        DrawLiveRadar(snapshot, height);
        ImGui::TableNextColumn();
        DrawQuickControls(snapshot, height);
        ImGui::EndTable();
    }
}

void OverlayRenderer::DrawLiveRadar(const AppSnapshot& snapshot, float height) {
    ImGui::BeginChild("LiveRadarPanel", ImVec2(0.0f, height), true);
    ImGui::TextUnformatted("LIVE DIRECTIONAL ENERGY");
    ImGui::SameLine();
    ImGui::TextDisabled("azimuth only / 15 deg sectors");
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float radius = std::max(80.0f, std::min(available.x, available.y) * 0.41f);
    const ImVec2 center{ImGui::GetCursorScreenPos().x + available.x * 0.50f,
                        ImGui::GetCursorScreenPos().y + available.y * 0.50f};
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 grid = IM_COL32(75, 92, 101, 180);
    const ImU32 gridFaint = IM_COL32(56, 68, 76, 135);
    drawList->AddCircle(center, radius, grid, 96, 1.5f);
    drawList->AddCircle(center, radius * 0.66f, gridFaint, 72, 1.0f);
    drawList->AddCircle(center, radius * 0.33f, gridFaint, 48, 1.0f);
    for (int degrees = 0; degrees < 360; degrees += 30) {
        drawList->AddLine(DirectionPoint(center, radius * 0.12f,
                                         static_cast<float>(degrees)),
                          DirectionPoint(center, radius, static_cast<float>(degrees)),
                          gridFaint, degrees % 90 == 0 ? 1.25f : 0.7f);
    }
    const auto label = [&](const char* text, float azimuth) {
        const ImVec2 point = DirectionPoint(center, radius + 18.0f, azimuth);
        const ImVec2 size = ImGui::CalcTextSize(text);
        drawList->AddText({point.x - size.x * 0.5f, point.y - size.y * 0.5f},
                          IM_COL32(190, 205, 211, 255), text);
    };
    label("FRONT 0 deg", 0.0f);
    label("R 90 deg", 90.0f);
    label("REAR 180 deg", 180.0f);
    label("L 270 deg", 270.0f);

    const float sensitivity = snapshot.settings.radar.sensitivityDbfs;
    const bool drawContinuous =
        snapshot.radar.mode != RadarMode::Events;
    if (snapshot.layout.directionalRadarAvailable && drawContinuous) {
        for (size_t index = 0;
             index < snapshot.radar.frame.sectorActivitiesDbfs.size(); ++index) {
            const float intensity = DbIntensity(
                snapshot.radar.frame.sectorActivitiesDbfs[index], sensitivity);
            if (intensity <= 0.001f) continue;
            const float azimuth = static_cast<float>(index) * 15.0f;
            const float start = (azimuth - 7.0f - 90.0f) * kPi / 180.0f;
            const float finish = (azimuth + 7.0f - 90.0f) * kPi / 180.0f;
            const float activeRadius = radius * (0.24f + 0.76f * intensity);
            drawList->PathLineTo(center);
            drawList->PathArcTo(center, activeRadius, start, finish, 7);
            drawList->PathFillConvex(IM_COL32(
                18, 204, 224, static_cast<int>(35 + 170 * intensity)));
        }
    }

    std::optional<float> newestEventAzimuth;
    if (snapshot.layout.directionalRadarAvailable &&
        snapshot.capture.state == AudioCaptureState::Running &&
        snapshot.radar.mode != RadarMode::Continuous) {
        const EventDisplaySettings& display = snapshot.settings.eventDisplay;
        const double nowAudio = static_cast<double>(
            snapshot.capture.streamSample) / 48000.0;
        for (const RecentEventSnapshot& event : snapshot.recentEvents) {
            if (event.streamGeneration != snapshot.capture.streamGeneration ||
                event.peakCount == 0 || event.confidence < display.minimumConfidence ||
                (event.suppressed && !display.showSuppressedEvents) ||
                nowAudio < event.timestampSeconds) {
                continue;
            }
            const float age = static_cast<float>(
                nowAudio - event.timestampSeconds);
            if (age > display.markerPersistenceSeconds) continue;
            const float fade = display.markerPersistenceSeconds > 0.0f
                ? std::clamp(1.0f - age / display.markerPersistenceSeconds,
                             0.0f, 1.0f)
                : 1.0f;
            for (uint32_t index = 0; index < event.peakCount; ++index) {
                const RadarPeak& peak = event.peaks[index];
                const float uncertainty = std::clamp(
                    peak.angularUncertaintyDegrees, 4.0f, 45.0f);
                const float start =
                    (peak.azimuthDegrees - uncertainty - 90.0f) * kPi / 180.0f;
                const float finish =
                    (peak.azimuthDegrees + uncertainty - 90.0f) * kPi / 180.0f;
                const float confidence = std::clamp(peak.confidence, 0.0f, 1.0f);
                const ImU32 color = event.soundClass == SoundClass::Gunshot
                    ? IM_COL32(255, 176, 42,
                               static_cast<int>(255.0f * fade *
                                                std::max(0.18f, confidence)))
                    : IM_COL32(24, 212, 232,
                               static_cast<int>(255.0f * fade *
                                                std::max(0.18f, confidence)));
                drawList->PathArcTo(center, radius * (0.84f - index * 0.06f),
                                    start, finish, 32);
                drawList->PathStroke(color, 0, 4.0f + confidence * 4.0f);
            }
            newestEventAzimuth = event.strongestAzimuthDegrees;
        }
    }

    const bool useContinuousArrow = drawContinuous &&
        snapshot.capture.state == AudioCaptureState::Running &&
        snapshot.radar.frame.status == RadarRuntimeStatus::Active;
    const std::optional<float> arrowAzimuth = useContinuousArrow
        ? std::optional<float>(snapshot.radar.frame.strongestAzimuthDegrees)
        : newestEventAzimuth;
    if (arrowAzimuth) {
        const ImVec2 tip = DirectionPoint(center, radius * 0.92f, *arrowAzimuth);
        const ImVec2 left = DirectionPoint(center, radius * 0.70f,
                                           *arrowAzimuth - 5.0f);
        const ImVec2 right = DirectionPoint(center, radius * 0.70f,
                                            *arrowAzimuth + 5.0f);
        drawList->AddTriangleFilled(tip, left, right,
                                    IM_COL32(255, 180, 42, 245));
    }
    drawList->AddCircleFilled(center, 4.0f, IM_COL32(220, 230, 234, 230));
    const char* centerText = !snapshot.layout.directionalRadarAvailable
        ? "DIRECTIONAL RADAR DISABLED"
        : (snapshot.radar.mode == RadarMode::Events && !newestEventAzimuth
            ? "WAITING FOR EVENT"
            : ToString(snapshot.radar.frame.status));
    const ImVec2 textSize = ImGui::CalcTextSize(centerText);
    drawList->AddText({center.x - textSize.x * 0.5f,
                       center.y + radius * 0.52f},
                      snapshot.layout.directionalRadarAvailable
                          ? IM_COL32(130, 155, 164, 240)
                          : IM_COL32(255, 175, 46, 255), centerText);
    ImGui::Dummy(available);
    ImGui::EndChild();
}

void OverlayRenderer::DrawQuickControls(const AppSnapshot& snapshot,
                                        float height) {
    ImGui::BeginChild("QuickControls", ImVec2(0.0f, height), true);
    ImGui::TextUnformatted("QUICK CONTROLS");
    ImGui::Separator();

    const char* modes[]{"Continuous", "Events", "Combined"};
    int mode = static_cast<int>(snapshot.settings.radar.mode);
    ImGui::TextDisabled("Radar mode");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##RadarMode", &mode, modes, IM_ARRAYSIZE(modes)) &&
        m_config.commands) {
        m_config.commands->Emplace<SetRadarModeCommand>(
            static_cast<RadarMode>(mode));
    }
    const char* presets[]{"All", "Footsteps", "Gunshots", "Custom"};
    int preset = static_cast<int>(snapshot.settings.radar.preset);
    ImGui::TextDisabled("Spectral preset");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##RadarPreset", &preset, presets,
                     IM_ARRAYSIZE(presets)) && m_config.commands) {
        m_config.commands->Emplace<SetRadarPresetCommand>(
            static_cast<RadarPreset>(preset));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Frequency emphasis only; this does not classify sounds.");
    }
    float sensitivity = snapshot.settings.radar.sensitivityDbfs;
    ImGui::TextDisabled("Sensitivity");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##RadarSensitivity", &sensitivity,
                           -96.0f, 0.0f, "%.0f dBFS") && m_config.commands) {
        m_config.commands->Emplace<SetRadarSensitivityCommand>(sensitivity);
    }
    float persistence = snapshot.settings.eventDisplay.markerPersistenceSeconds;
    ImGui::TextDisabled("Event persistence");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##EventPersistence", &persistence,
                           0.0f, 10.0f, "%.1f s") && m_config.commands) {
        EventDisplaySettings display = snapshot.settings.eventDisplay;
        display.markerPersistenceSeconds = persistence;
        m_config.commands->Emplace<UpdateEventDisplayCommand>(display);
    }

    bool hudVisible = snapshot.hud.state == HudUiState::Visible ||
                      snapshot.hud.state == HudUiState::Editing;
    if (ImGui::Checkbox("Show in-game HUD", &hudVisible) && m_config.commands) {
        m_config.commands->Emplace<SetHudVisibleCommand>(hudVisible);
    }
    bool recording = snapshot.recording;
    if (ImGui::Checkbox("Record multichannel WAV", &recording) &&
        m_config.commands) {
        m_config.commands->Emplace<SetRecordingCommand>(recording);
    }
    ImGui::Separator();
    ImGui::TextDisabled("Strongest bearing");
    const RecentEventSnapshot* latestEvent = nullptr;
    if (snapshot.radar.mode == RadarMode::Events) {
        for (auto iterator = snapshot.recentEvents.rbegin();
             iterator != snapshot.recentEvents.rend(); ++iterator) {
            if (iterator->streamGeneration ==
                    snapshot.capture.streamGeneration &&
                iterator->peakCount > 0) {
                latestEvent = &*iterator;
                break;
            }
        }
    }
    if (latestEvent) {
        ImGui::TextColored(kAmber, "%.0f deg  /  %.0f dBFS",
                           latestEvent->strongestAzimuthDegrees,
                           latestEvent->peaks[0].energyDbfs);
        ImGui::Text("Event confidence %.0f%%",
                    latestEvent->confidence * 100.0f);
    } else if (snapshot.radar.frame.status == RadarRuntimeStatus::Active &&
               snapshot.radar.mode != RadarMode::Events) {
        ImGui::TextColored(kAmber, "%.0f deg  /  %.0f dBFS",
                           snapshot.radar.frame.strongestAzimuthDegrees,
                           snapshot.radar.frame.sectorActivitiesDbfs[
                               snapshot.radar.frame.strongestSector]);
        ImGui::Text("Confidence %.0f%%",
                    snapshot.radar.frame.confidence * 100.0f);
    } else {
        ImGui::TextDisabled("No active sector");
    }
    ImGui::Spacing();
    if (snapshot.capture.state == AudioCaptureState::Recovering) {
        ImGui::TextColored(kAmber, "~ Device recovery attempt %u",
                           snapshot.recovery.attempt);
        ImGui::TextWrapped("%s", snapshot.recovery.statusText.c_str());
    } else if (!snapshot.layout.directionalRadarAvailable) {
        ImGui::TextColored(kAmber, "[!] Surround format required");
        ImGui::TextWrapped("%s", snapshot.layout.statusText.c_str());
    }
    ImGui::EndChild();
}

void OverlayRenderer::DrawRecentEvents(const AppSnapshot& snapshot) {
    ImGui::TextUnformatted("RECENT EVENTS");
    const EventDisplaySettings& display = snapshot.settings.eventDisplay;
    if (!display.showTimeline) {
        ImGui::TextDisabled(
            "Timeline hidden. Re-enable it under Event Details.");
        return;
    }
    const int columnCount = 4 + (display.showConfidence ? 1 : 0) +
        (display.showMiniRadar ? 1 : 0);
    if (ImGui::BeginTable("RecentEventTimeline", columnCount,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                              ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, 180.0f))) {
        ImGui::TableSetupColumn("Time");
        ImGui::TableSetupColumn("Class");
        if (display.showConfidence) ImGui::TableSetupColumn("Confidence");
        ImGui::TableSetupColumn("Strongest direction");
        ImGui::TableSetupColumn("Peaks");
        if (display.showMiniRadar) ImGui::TableSetupColumn("Mini radar");
        ImGui::TableHeadersRow();
        size_t shown = 0;
        for (auto iterator = snapshot.recentEvents.rbegin();
             iterator != snapshot.recentEvents.rend() && shown < 20;
             ++iterator) {
            const RecentEventSnapshot& event = *iterator;
            if (event.confidence < display.minimumConfidence ||
                (event.suppressed && !display.showSuppressedEvents)) {
                continue;
            }
            ++shown;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%.2fs", event.timestampSeconds);
            ImGui::TableNextColumn();
            ImGui::TextColored(event.soundClass == SoundClass::Gunshot
                                   ? kAmber : kCyan,
                               "%s%s", ToString(event.soundClass),
                               event.suppressed ? " [suppressed]" : "");
            if (display.showConfidence) {
                ImGui::TableNextColumn();
                ImGui::Text("%.0f%%", event.confidence * 100.0f);
            }
            ImGui::TableNextColumn();
            if (event.peakCount == 0) ImGui::TextDisabled("Pending / unavailable");
            else ImGui::Text("%.0f deg", event.strongestAzimuthDegrees);
            ImGui::TableNextColumn();
            ImGui::Text("%u", event.peakCount);
            if (display.showMiniRadar) {
                ImGui::TableNextColumn();
                const ImVec2 cell = ImGui::GetCursorScreenPos();
                DrawMiniRadar(ImGui::GetWindowDrawList(),
                              {cell.x + 32.0f, cell.y + 16.0f}, 14.0f,
                              event.miniatureRadarDbfs,
                              snapshot.settings.radar.sensitivityDbfs);
                ImGui::Dummy(ImVec2(64.0f, 32.0f));
            }
        }
        if (shown == 0) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("No recognition-triggered radar events yet.");
        }
        ImGui::EndTable();
    }
}

void OverlayRenderer::DrawAudioSetup(const AppSnapshot& snapshot) {
    ImGui::Indent();
    ImGui::TextColored(snapshot.settings.onboarding.completed ? kCyan : kAmber,
                       "%s %s",
                       snapshot.settings.onboarding.completed ? "[+]" : "[ ]",
                       SetupStepText(snapshot.settings.onboarding.step));
    const char* preview = snapshot.capture.endpointName.empty()
        ? "Select a Windows playback endpoint" : snapshot.capture.endpointName.c_str();
    ImGui::SetNextItemWidth(std::min(620.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("Playback endpoint", preview)) {
        for (const AudioDeviceInfo& endpoint : snapshot.outputDevices) {
            const bool selected = endpoint.id == snapshot.capture.endpointId;
            const std::string label = endpoint.name + "  [" +
                std::to_string(endpoint.nativeChannels) + " ch / " +
                std::to_string(endpoint.nativeSampleRate) + " Hz / " +
                ToString(endpoint.layout.kind) + "]";
            if (ImGui::Selectable(label.c_str(), selected) && m_config.commands) {
                m_config.commands->Emplace<SelectEndpointCommand>(endpoint.id);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        if (snapshot.outputDevices.empty()) {
            ImGui::TextDisabled("No playback endpoints detected.");
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Windows sound settings") && m_config.commands) {
        m_config.commands->Emplace<OpenSoundSettingsCommand>();
    }

    ImGui::Text("Detected: %u Hz, %s, mask 0x%08X",
                snapshot.capture.sampleRate,
                ToString(snapshot.layout.detected.kind),
                snapshot.layout.detected.channelMask);
    if (snapshot.layout.directionalRadarAvailable) {
        ImGui::TextColored(kCyan, "[+] Native surround layout validated");
    } else {
        ImGui::TextColored(kAmber, "[!] %s", snapshot.layout.statusText.c_str());
        ImGui::TextWrapped("Stereo and 44.1 kHz remain usable for recognition and settings, but directional radar is disabled. EchoRadar never approximates surround direction from stereo.");
    }

    ImGui::TextUnformatted("Live labeled channel meters");
    const uint32_t channelCount = std::min<uint32_t>(
        snapshot.capture.levels.channelCount, kMaxAudioChannels);
    if (ImGui::BeginTable("ChannelMeters", std::max(1u, channelCount),
                          ImGuiTableFlags_SizingStretchSame |
                              ImGuiTableFlags_BordersInnerV)) {
        for (uint32_t channel = 0; channel < channelCount; ++channel) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(ToString(snapshot.layout.detected.roles[channel]));
            const float meter = std::clamp(
                snapshot.capture.levels.peak[channel], 0.0f, 1.0f);
            char id[32]{};
            std::snprintf(id, sizeof(id), "##channel_%u", channel);
            ImGui::ProgressBar(meter, ImVec2(-1.0f, 0.0f), "");
        }
        ImGui::EndTable();
    }

    if (!snapshot.settings.onboarding.completed && m_config.commands) {
        SetupEvent next = SetupEvent::Retry;
        const char* text = "Continue";
        bool enabled = true;
        switch (snapshot.settings.onboarding.step) {
        case SetupStep::SelectEndpoint:
            next = SetupEvent::EndpointSelected;
            enabled = !snapshot.capture.endpointId.empty();
            break;
        case SetupStep::ValidateFormat:
            next = snapshot.layout.directionalRadarAvailable
                ? SetupEvent::FormatSupported
                : (snapshot.capture.sampleRate != 48000
                    ? SetupEvent::FormatUnsupportedSampleRate
                    : (snapshot.layout.detected.kind == AudioChannelLayoutKind::Stereo
                        ? SetupEvent::FormatUnsupportedStereo
                        : SetupEvent::FormatUnsupportedChannelMask));
            text = snapshot.layout.directionalRadarAvailable
                ? "Format validated" : "Record unsupported format";
            break;
        case SetupStep::ConfirmChannels:
            next = SetupEvent::ChannelActivityConfirmed;
            text = "I can see channel activity";
            break;
        case SetupStep::PreviewHud:
            next = SetupEvent::HudPreviewConfirmed;
            text = "Finish setup";
            break;
        case SetupStep::Complete:
            enabled = false;
            break;
        }
        if (!enabled) ImGui::BeginDisabled();
        if (ImGui::Button(text)) {
            m_config.commands->Emplace<ApplySetupEventCommand>(next);
        }
        if (!enabled) ImGui::EndDisabled();
    }
    ImGui::Unindent();
}

void OverlayRenderer::DrawEventDetails(const AppSnapshot& snapshot) {
    ImGui::Indent();
    EventDisplaySettings display = snapshot.settings.eventDisplay;
    bool displayChanged = false;
    displayChanged |= ImGui::Checkbox("Show recent-event timeline",
                                      &display.showTimeline);
    displayChanged |= ImGui::Checkbox("Show confidence column",
                                      &display.showConfidence);
    displayChanged |= ImGui::Checkbox("Show miniature radar",
                                      &display.showMiniRadar);
    ImGui::BeginDisabled();
    ImGui::Checkbox("Show suppressed events", &display.showSuppressedEvents);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("recognizer safety policy excludes them");
    displayChanged |= ImGui::SliderFloat("Minimum event confidence",
                                         &display.minimumConfidence,
                                         0.0f, 1.0f, "%.2f");
    int maximumEvents = static_cast<int>(display.maximumRecentEvents);
    if (ImGui::SliderInt("Maximum retained events", &maximumEvents,
                         1, 500)) {
        display.maximumRecentEvents = static_cast<uint32_t>(maximumEvents);
        displayChanged = true;
    }
    if (displayChanged && m_config.commands) {
        m_config.commands->Emplace<UpdateEventDisplayCommand>(display);
    }
    ImGui::Separator();

    const RecentEventSnapshot* latest = nullptr;
    for (auto iterator = snapshot.recentEvents.rbegin();
         iterator != snapshot.recentEvents.rend(); ++iterator) {
        if (iterator->confidence >= display.minimumConfidence &&
            (!iterator->suppressed || display.showSuppressedEvents)) {
            latest = &*iterator;
            break;
        }
    }
    if (!latest) {
        ImGui::TextDisabled("No event radar result is available.");
        ImGui::Unindent();
        return;
    }
    const RecentEventSnapshot& event = *latest;
    ImGui::Text("Latest %s at %.2fs - recognition confidence %.0f%%",
                ToString(event.soundClass), event.timestampSeconds,
                event.confidence * 100.0f);
    if (ImGui::BeginTable("EventPeaks", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("Energy peak");
        ImGui::TableSetupColumn("Azimuth");
        ImGui::TableSetupColumn("Energy");
        ImGui::TableSetupColumn("Angular uncertainty");
        ImGui::TableHeadersRow();
        for (uint32_t index = 0; index < event.peakCount; ++index) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%u", index + 1);
            ImGui::TableNextColumn(); ImGui::Text("%.0f deg", event.peaks[index].azimuthDegrees);
            ImGui::TableNextColumn(); ImGui::Text("%.1f dBFS", event.peaks[index].energyDbfs);
            ImGui::TableNextColumn(); ImGui::Text("+/- %.0f deg", event.peaks[index].angularUncertaintyDegrees);
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Peaks are separated azimuthal energy maxima, not inferred physical sources. Elevation is not estimated.");
    ImGui::Unindent();
}

void OverlayRenderer::DrawHudEditor(const AppSnapshot& snapshot) {
    ImGui::Indent();
    const std::string displayId = snapshot.hud.displayId.empty()
        ? "default" : snapshot.hud.displayId;
    HudDisplaySettings hud;
    if (const HudDisplaySettings* found =
            FindHudDisplaySettings(snapshot.settings, displayId)) {
        hud = *found;
    } else if (const HudDisplaySettings* fallback =
                   FindHudDisplaySettings(snapshot.settings, "default")) {
        hud = *fallback;
        hud.displayId = displayId;
    } else {
        hud.displayId = displayId;
    }
    bool changed = false;
    ImGui::Text("Display: %s", displayId.c_str());
    changed |= ImGui::SliderFloat("HUD scale", &hud.scale, 0.25f, 4.0f, "%.2fx");
    changed |= ImGui::SliderFloat("HUD opacity", &hud.opacity, 0.05f, 1.0f, "%.2f");
    const char* anchors[]{"Top left", "Top center", "Top right", "Center left",
                          "Center", "Center right", "Bottom left", "Bottom center",
                          "Bottom right"};
    int anchor = static_cast<int>(hud.anchor);
    if (ImGui::Combo("Anchor", &anchor, anchors, IM_ARRAYSIZE(anchors))) {
        hud.anchor = static_cast<HudAnchor>(anchor);
        changed = true;
    }
    changed |= ImGui::DragFloat2("Offset", &hud.offsetX, 1.0f, -4000.0f,
                                 4000.0f, "%.0f px");
    changed |= ImGui::ColorEdit4("Sector color", &hud.sectorColor.red,
                                 ImGuiColorEditFlags_NoInputs);
    changed |= ImGui::ColorEdit4("Strongest arrow", &hud.strongestColor.red,
                                 ImGuiColorEditFlags_NoInputs);
    changed |= ImGui::Checkbox("Cardinal labels", &hud.showCardinalLabels);
    changed |= ImGui::Checkbox("Degree labels", &hud.showDegreeLabels);
    changed |= ImGui::SliderFloat("Marker persistence", &hud.persistenceSeconds,
                                  0.0f, 10.0f, "%.1f s");
    const char* backgrounds[]{"Transparent", "Tactical dark", "Light", "Checkerboard"};
    int background = static_cast<int>(hud.previewBackground);
    if (ImGui::Combo("Preview background", &background, backgrounds,
                     IM_ARRAYSIZE(backgrounds))) {
        hud.previewBackground = static_cast<HudPreviewBackground>(background);
        changed = true;
    }
    if (changed && m_config.commands) {
        m_config.commands->Emplace<UpdateHudDisplayCommand>(displayId, hud);
    }
    bool edit = snapshot.hud.state == HudUiState::Editing;
    if (ImGui::Checkbox("Unlock HUD for drag positioning", &edit) &&
        m_config.commands) {
        m_config.commands->Emplace<SetHudEditModeCommand>(displayId, edit);
    }
    ImGui::TextDisabled(edit
        ? "Edit mode is active: the topmost HUD accepts dragging. Turn it off to restore click-through."
        : "Normal mode is topmost and click-through.");
    ImGui::Unindent();
}

void OverlayRenderer::DrawAdvanced(const AppSnapshot& snapshot) {
    ImGui::Indent();
    RadarProcessorConfig tuning = snapshot.settings.radar;
    bool smoothingChanged = false;
    int attack = static_cast<int>(tuning.attackMilliseconds);
    int release = static_cast<int>(tuning.releaseMilliseconds);
    int hold = static_cast<int>(tuning.holdMilliseconds);
    smoothingChanged |= ImGui::SliderInt("Attack", &attack, 0, 2000, "%d ms");
    smoothingChanged |= ImGui::SliderInt("Release", &release, 0, 5000, "%d ms");
    smoothingChanged |= ImGui::SliderInt("Hold", &hold, 0, 2000, "%d ms");
    if (smoothingChanged && m_config.commands) {
        m_config.commands->Emplace<SetRadarSmoothingCommand>(
            static_cast<uint32_t>(attack), static_cast<uint32_t>(release),
            static_cast<uint32_t>(hold));
    }
    ImGui::TextDisabled("STFT: 2048 Hann / 480 sample (10 ms) hop / 24 sectors");

    if (tuning.preset == RadarPreset::Custom) {
        ImGui::TextUnformatted("Custom spectral emphasis (dB)");
        bool curveChanged = false;
        for (size_t index = 0; index < tuning.customCurveDb.size(); ++index) {
            char label[48]{};
            std::snprintf(label, sizeof(label), "%.0f Hz##curve_%zu",
                          kRadarCurveFrequenciesHz[index], index);
            curveChanged |= ImGui::SliderFloat(
                label, &tuning.customCurveDb[index], -24.0f, 24.0f, "%+.1f dB");
        }
        if (curveChanged && m_config.commands) {
            m_config.commands->Emplace<SetCustomRadarCurveCommand>(
                tuning.customCurveDb);
        }
    }
    ImGui::Separator();
    ImGui::Text("Recognition: %s", snapshot.model.statusText.c_str());
    if (snapshot.model.state == ModelUiState::Ready) {
        RecognitionRuntimeTuning recognition = snapshot.recognitionTuning;
        bool recognitionChanged = false;
        ImGui::TextUnformatted("Recognition thresholds");
        recognitionChanged |= ImGui::SliderFloat(
            "Gunshot threshold (quiet)", &recognition.quietThresholds[0],
            0.01f, 1.0f, "%.2f");
        recognitionChanged |= ImGui::SliderFloat(
            "Footstep threshold (quiet)", &recognition.quietThresholds[1],
            0.01f, 1.0f, "%.2f");
        recognitionChanged |= ImGui::SliderFloat(
            "Gunshot threshold (busy)", &recognition.busyThresholds[0],
            0.01f, 1.0f, "%.2f");
        recognitionChanged |= ImGui::SliderFloat(
            "Footstep threshold (busy)", &recognition.busyThresholds[1],
            0.01f, 1.0f, "%.2f");
        recognitionChanged |= ImGui::SliderFloat(
            "Scene activity cutoff", &recognition.sceneActivityCutoff,
            0.01f, 0.99f, "%.2f");
        recognitionChanged |= ImGui::SliderFloat(
            "Self-suppression threshold",
            &recognition.selfSuppressionThreshold, 0.01f, 1.0f, "%.2f");
        int gunshotSpacing = static_cast<int>(
            recognition.minimumSpacingMs[0]);
        int footstepSpacing = static_cast<int>(
            recognition.minimumSpacingMs[1]);
        if (ImGui::SliderInt("Gunshot minimum spacing", &gunshotSpacing,
                             1, 5000, "%d ms")) {
            recognition.minimumSpacingMs[0] =
                static_cast<uint32_t>(gunshotSpacing);
            recognitionChanged = true;
        }
        if (ImGui::SliderInt("Footstep minimum spacing", &footstepSpacing,
                             1, 5000, "%d ms")) {
            recognition.minimumSpacingMs[1] =
                static_cast<uint32_t>(footstepSpacing);
            recognitionChanged = true;
        }
        if (recognitionChanged && m_config.commands) {
            m_config.commands->Emplace<UpdateRecognitionTuningCommand>(
                recognition);
        }
    } else {
        ImGui::TextDisabled(
            "Load a recognition package with --model to edit thresholds.");
    }
    bool sessionLogging = snapshot.sessionLoggingActive;
    if (ImGui::Checkbox("Write schema-3 session log", &sessionLogging) &&
        m_config.commands) {
        m_config.commands->Emplace<SetSessionLoggingCommand>(sessionLogging);
    }
    if (!snapshot.sessionLoggingError.empty()) {
        ImGui::TextColored(kRed, "! %s",
                           snapshot.sessionLoggingError.c_str());
    }
    ImGui::Text("Stream generation: %llu",
                static_cast<unsigned long long>(snapshot.capture.streamGeneration));
    ImGui::Text("Capture drops: %llu frames",
                static_cast<unsigned long long>(snapshot.capture.droppedFrames));
    ImGui::Text("Backlog discarded: %llu frames",
                static_cast<unsigned long long>(
                    snapshot.capture.discardedBacklogFrames));
    ImGui::Text("Device restarts: %llu",
                static_cast<unsigned long long>(snapshot.capture.restartCount));
    ImGui::Text("Channel mask: 0x%08X", snapshot.layout.detected.channelMask);
    ImGui::Text("Radar confidence: %.3f", snapshot.radar.frame.confidence);
    if (m_config.commands && ImGui::Button("Save settings now")) {
        m_config.commands->Emplace<SaveSettingsCommand>();
    }
    ImGui::Unindent();
}

#else

void OverlayRenderer::DrawDashboard(const AppSnapshot&) {}
void OverlayRenderer::DrawHeader(const AppSnapshot&) {}
void OverlayRenderer::DrawInlineStatus(const AppSnapshot&) {}
void OverlayRenderer::DrawRadarWorkspace(const AppSnapshot&) {}
void OverlayRenderer::DrawLiveRadar(const AppSnapshot&, float) {}
void OverlayRenderer::DrawQuickControls(const AppSnapshot&, float) {}
void OverlayRenderer::DrawRecentEvents(const AppSnapshot&) {}
void OverlayRenderer::DrawAudioSetup(const AppSnapshot&) {}
void OverlayRenderer::DrawEventDetails(const AppSnapshot&) {}
void OverlayRenderer::DrawHudEditor(const AppSnapshot&) {}
void OverlayRenderer::DrawAdvanced(const AppSnapshot&) {}

#endif

} // namespace EchoRadar
