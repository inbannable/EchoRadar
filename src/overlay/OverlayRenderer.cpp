#include "OverlayRenderer.h"

#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <limits>
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
#include <mmsystem.h>

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
    ImGuiContext* imguiContext{nullptr};
    ImGuiStyle baseStyle{};
};

namespace {

constexpr wchar_t kWindowClassName[] = L"EchoRadarEventChartWindow";

void CreateRenderTarget(OverlayRenderer::PlatformImpl& platform) {
    if (platform.swapChain == nullptr || platform.device == nullptr) return;
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(platform.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        platform.device->CreateRenderTargetView(backBuffer, nullptr, &platform.renderTarget);
        backBuffer->Release();
    }
}

void CleanupRenderTarget(OverlayRenderer::PlatformImpl& platform) {
    if (platform.renderTarget != nullptr) {
        platform.renderTarget->Release();
        platform.renderTarget = nullptr;
    }
}

bool CreateDevice(OverlayRenderer::PlatformImpl& platform) {
    DXGI_SWAP_CHAIN_DESC swapChainDescription{};
    swapChainDescription.BufferCount = 2;
    swapChainDescription.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDescription.BufferDesc.RefreshRate.Numerator = 60;
    swapChainDescription.BufferDesc.RefreshRate.Denominator = 1;
    swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDescription.OutputWindow = platform.window;
    swapChainDescription.SampleDesc.Count = 1;
    swapChainDescription.Windowed = TRUE;
    swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selectedFeatureLevel{};
    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &swapChainDescription,
        &platform.swapChain,
        &platform.device,
        &selectedFeatureLevel,
        &platform.deviceContext);
    if (FAILED(result)) {
        result = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &swapChainDescription,
            &platform.swapChain,
            &platform.device,
            &selectedFeatureLevel,
            &platform.deviceContext);
    }
    if (FAILED(result)) return false;
    CreateRenderTarget(platform);
    return platform.renderTarget != nullptr;
}

void CleanupDevice(OverlayRenderer::PlatformImpl& platform) {
    CleanupRenderTarget(platform);
    if (platform.swapChain != nullptr) {
        platform.swapChain->Release();
        platform.swapChain = nullptr;
    }
    if (platform.deviceContext != nullptr) {
        platform.deviceContext->Release();
        platform.deviceContext = nullptr;
    }
    if (platform.device != nullptr) {
        platform.device->Release();
        platform.device = nullptr;
    }
}

LRESULT WINAPI OverlayWindowProc(HWND window, UINT message,
                                 WPARAM wParam, LPARAM lParam) {
    auto* platform = reinterpret_cast<OverlayRenderer::PlatformImpl*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        platform = static_cast<OverlayRenderer::PlatformImpl*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(platform));
    }

    if (platform != nullptr && platform->imguiContext != nullptr) {
        ImGui::SetCurrentContext(platform->imguiContext);
        if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) {
            return true;
        }
    }

    switch (message) {
    case WM_SIZE:
        if (platform != nullptr && platform->device != nullptr &&
            wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget(*platform);
            platform->swapChain->ResizeBuffers(
                0, static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)),
                DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget(*platform);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

ImU32 EventColor(SoundClass soundClass) {
    return soundClass == SoundClass::Gunshot
        ? IM_COL32(255, 111, 96, 235)
        : IM_COL32(82, 190, 255, 235);
}

const char* EventShortName(SoundClass soundClass) {
    return soundClass == SoundClass::Gunshot ? "GUNSHOT" : "FOOTSTEP";
}

void DrawText(ImDrawList* drawList, ImVec2 position, ImU32 color,
              const char* format, ...) {
    char buffer[128]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    drawList->AddText(position, color, buffer);
}

} // namespace
#else
struct OverlayRenderer::PlatformImpl {};
#endif

OverlayRenderer::OverlayRenderer() : OverlayRenderer(Config{}) {}

OverlayRenderer::OverlayRenderer(Config cfg) : m_config(std::move(cfg)) {}

OverlayRenderer::~OverlayRenderer() {
    Shutdown();
}

bool OverlayRenderer::Initialise() {
#ifdef _WIN32
    if (m_running) return true;

    m_platform = std::make_unique<PlatformImpl>();
    m_platform->instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = OverlayWindowProc;
    windowClass.hInstance = m_platform->instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClassName;
    if (RegisterClassExW(&windowClass) != 0) {
        m_platform->classRegistered = true;
    } else if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        m_platform.reset();
        return false;
    }

    RECT windowRect{0, 0, m_config.windowWidth, m_config.windowHeight};
    AdjustWindowRectEx(&windowRect, WS_OVERLAPPEDWINDOW, FALSE, 0);
    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;
    const int x = std::max(0, (GetSystemMetrics(SM_CXSCREEN) - width) / 2);
    const int y = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - height) / 2);
    m_platform->window = CreateWindowExW(
        WS_EX_APPWINDOW,
        kWindowClassName,
        L"EchoRadar - Control",
        WS_OVERLAPPEDWINDOW,
        x, y, width, height,
        nullptr,
        nullptr,
        m_platform->instance,
        m_platform.get());
    if (m_platform->window == nullptr || !CreateDevice(*m_platform)) {
        Shutdown();
        return false;
    }

    IMGUI_CHECKVERSION();
    m_platform->imguiContext = ImGui::CreateContext();
    ImGui::SetCurrentContext(m_platform->imguiContext);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.035f, 0.047f, 0.070f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.055f, 0.070f, 0.100f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.11f, 0.14f, 0.19f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.21f, 0.29f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.12f, 0.25f, 0.36f, 1.0f);
    m_platform->baseStyle = style;
    ImGui_ImplWin32_Init(m_platform->window);
    ImGui_ImplDX11_Init(m_platform->device, m_platform->deviceContext);
    m_platform->imguiInitialized = true;

    ShowWindow(m_platform->window, SW_SHOWDEFAULT);
    UpdateWindow(m_platform->window);
    m_running = true;
    return true;
#else
    std::cout << "[OverlayRenderer] Event chart UI is only supported on Windows.\n";
    return false;
#endif
}

void OverlayRenderer::Shutdown() {
#ifdef _WIN32
    StopClip();
    if (m_platform) {
        if (m_platform->imguiInitialized) {
            ImGui::SetCurrentContext(m_platform->imguiContext);
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext(m_platform->imguiContext);
            m_platform->imguiContext = nullptr;
            m_platform->imguiInitialized = false;
        }
        CleanupDevice(*m_platform);
        if (m_platform->window != nullptr) {
            DestroyWindow(m_platform->window);
            m_platform->window = nullptr;
        }
        if (m_platform->classRegistered) {
            UnregisterClassW(kWindowClassName, m_platform->instance);
            m_platform->classRegistered = false;
        }
        m_platform.reset();
    }
#endif
    m_running = false;
}

void OverlayRenderer::PushRecognitionEvent(const SoundEvent& event) {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    if (event.streamGeneration != m_streamGeneration) {
        m_recognitionEvents.clear();
        m_streamGeneration = event.streamGeneration;
    }
    m_recognitionEvents.push_back(event);
    constexpr size_t kMaximumChartEvents = 1024;
    while (m_recognitionEvents.size() > kMaximumChartEvents) {
        m_recognitionEvents.pop_front();
    }
}

void OverlayRenderer::PushDirectionScene(
    std::vector<DirectionSceneEvent> events,
    const DirectionSceneResult& direction,
    std::filesystem::path clipPath) {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    m_directionScenes.push_back(
        {std::move(events), direction, std::move(clipPath)});
    constexpr size_t kMaximumDirectionScenes = 256;
    while (m_directionScenes.size() > kMaximumDirectionScenes) {
        m_directionScenes.pop_front();
    }
}

void OverlayRenderer::PushAudioClock(uint64_t sample, uint64_t streamGeneration,
                                      bool discontinuity) {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    if (discontinuity || streamGeneration != m_streamGeneration) {
        m_recognitionEvents.clear();
        m_directionScenes.clear();
        m_streamGeneration = streamGeneration;
        m_currentSample = 0;
        m_audioLevels = {};
        m_recognitionScores = {};
        m_sceneActivity = 0.0f;
        m_haveRecognitionScores = false;
    }
    m_currentSample = std::max(m_currentSample, sample);
}

void OverlayRenderer::PushAudioLevels(const AudioLevels& levels) {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    m_audioLevels = levels;
}

void OverlayRenderer::PushRecognitionScores(const RecognitionModelOutput& output, float sceneActivity,
                                    bool hasOutput) {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    m_recognitionScores = output;
    m_sceneActivity = std::clamp(sceneActivity, 0.0f, 1.0f);
    m_haveRecognitionScores = hasOutput;
}

void OverlayRenderer::Render() {
#ifdef _WIN32
    if (!m_running || !m_platform || !m_platform->imguiInitialized) return;

    ImGui::SetCurrentContext(m_platform->imguiContext);

    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
        if (message.message == WM_QUIT) {
            m_running = false;
            return;
        }
    }

    const AppSettings appSettings = m_config.runtimeSettings
        ? m_config.runtimeSettings->Snapshot() : AppSettings{};
    ApplyUiScale(appSettings.uiScale);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawUi();
    ImGui::Render();

    const float clearColor[4] = {0.025f, 0.033f, 0.052f, 1.0f};
    m_platform->deviceContext->OMSetRenderTargets(1, &m_platform->renderTarget, nullptr);
    m_platform->deviceContext->ClearRenderTargetView(m_platform->renderTarget, clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    m_platform->swapChain->Present(1, 0);
#endif
}

#ifdef _WIN32

void OverlayRenderer::ApplyUiScale(float scale) {
    if (!m_platform) return;
    if (!std::isfinite(scale)) scale = AppSettings::kDefaultUiScale;
    scale = std::clamp(scale, AppSettings::kMinUiScale, AppSettings::kMaxUiScale);
    if (std::abs(scale - m_appliedUiScale) < 0.001f) return;

    ImGuiStyle scaledStyle = m_platform->baseStyle;
    scaledStyle.ScaleAllSizes(scale);
    ImGui::GetStyle() = scaledStyle;
    ImGui::GetIO().FontGlobalScale = scale;
    m_appliedUiScale = scale;
}

void OverlayRenderer::DrawUi() {
    std::deque<SoundEvent> events;
    std::deque<DirectionSceneRecord> directionScenes;
    uint64_t currentSample = 0;
    uint64_t streamGeneration = 0;
    AudioLevels audioLevels;
    RecognitionModelOutput recognitionScores;
    float sceneActivity = 0.0f;
    bool haveRecognitionScores = false;
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        events = m_recognitionEvents;
        directionScenes = m_directionScenes;
        currentSample = m_currentSample;
        streamGeneration = m_streamGeneration;
        audioLevels = m_audioLevels;
        recognitionScores = m_recognitionScores;
        sceneActivity = m_sceneActivity;
        haveRecognitionScores = m_haveRecognitionScores;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    constexpr ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("EchoRadar", nullptr, windowFlags);

    ImGui::TextColored(ImVec4(0.36f, 0.82f, 1.0f, 1.0f), "ECHO RADAR");
    ImGui::SameLine();
    ImGui::TextDisabled("EVENT MONITOR");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 92.0f);
    if (m_running) {
        ImGui::TextColored(ImVec4(0.30f, 0.92f, 0.52f, 1.0f), "● LIVE");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "● STOPPED");
    }
    ImGui::Separator();

    const char* modelText = m_config.modelVersion.empty()
        ? "Recognition model unavailable" : m_config.modelVersion.c_str();
    ImGui::Text("Model: %s", modelText);
    ImGui::SameLine();
    ImGui::TextDisabled("| stream %llu | audio %.2fs | %zu events",
                        static_cast<unsigned long long>(streamGeneration),
                        m_config.sampleRate == 0
                            ? 0.0
                            : static_cast<double>(currentSample) / m_config.sampleRate,
                        events.size());
    if (!m_config.recognitionError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.58f, 0.32f, 1.0f),
                           "Recognition paused: %s", m_config.recognitionError.c_str());
    }

    if (ImGui::BeginTabBar("EchoRadarPages")) {
        if (ImGui::BeginTabItem("Live")) {
            DrawLiveDiagnostics(audioLevels, recognitionScores, sceneActivity, haveRecognitionScores);
            if (ImGui::Button("Clear chart")) {
                std::lock_guard<std::mutex> lock(m_dataMutex);
                m_recognitionEvents.clear();
                m_directionScenes.clear();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(180.0f);
            ImGui::SliderFloat("Timeline span", &m_chartWindowSeconds,
                               5.0f, 120.0f, "%.0f s");
            DrawEventTimeline(events, currentSample, streamGeneration);
            if (ImGui::BeginTable("LiveLowerPanels", 2,
                                  ImGuiTableFlags_Resizable |
                                      ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("Recognition events",
                                        ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Direction estimates",
                                        ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                DrawRecentEvents(events, streamGeneration);
                ImGui::TableNextColumn();
                DrawDirectionPage(directionScenes);
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Recognition")) {
            ImGui::TextDisabled("Recognition event policy; changes apply on the next audio block.");
            DrawTuneTable();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Direction")) {
            DrawDirectionPage(directionScenes);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Overlay")) {
            DrawOverlayPage();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Audio & System")) {
            DrawAudioSystemPage();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void OverlayRenderer::DrawLiveDiagnostics(const AudioLevels& levels,
                                           const RecognitionModelOutput& scores,
                                           float sceneActivity, bool haveScores) {
    const auto dbFs = [](float amplitude) {
        if (!std::isfinite(amplitude) || amplitude <= 1.0e-6f) return -120.0f;
        return std::max(-120.0f, 20.0f * std::log10(amplitude));
    };
    const float leftRmsDb = dbFs(levels.leftRms);
    const float rightRmsDb = dbFs(levels.rightRms);
    const float leftPeakDb = dbFs(levels.leftPeak);
    const float rightPeakDb = dbFs(levels.rightPeak);
    const float cutoff = m_config.runtimeTuning
        ? m_config.runtimeTuning->Snapshot().sceneActivityCutoff : 0.5f;

    if (ImGui::BeginTable("LiveDiagnostics", 2,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                          ImVec2(0.0f, 126.0f))) {
        ImGui::TableSetupColumn("Live audio level", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Live recognition scores", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Current sound-level score");
        ImGui::SameLine();
        ImGui::TextDisabled("(scene activity)");
        if (haveScores) {
            char sceneOverlay[16]{};
            std::snprintf(sceneOverlay, sizeof(sceneOverlay), "%.3f", sceneActivity);
            ImGui::ProgressBar(sceneActivity, ImVec2(-FLT_MIN, 0.0f),
                               sceneOverlay);
            ImGui::Text("Scene: %s  |  cutoff %.3f",
                        sceneActivity >= cutoff ? "BUSY" : "QUIET", cutoff);
        } else {
            ImGui::TextDisabled("Waiting for recognition feature/inference output...");
        }
        ImGui::Text("RMS   L %6.1f dBFS   R %6.1f dBFS", leftRmsDb, rightRmsDb);
        ImGui::Text("Peak  L %6.1f dBFS   R %6.1f dBFS", leftPeakDb, rightPeakDb);

        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Current onset scores");
        if (haveScores) {
            const float gunshot = std::clamp(scores.onsetProbabilities[0], 0.0f, 1.0f);
            const float footstep = std::clamp(scores.onsetProbabilities[1], 0.0f, 1.0f);
            char gunshotOverlay[16]{};
            char footstepOverlay[16]{};
            std::snprintf(gunshotOverlay, sizeof(gunshotOverlay), "%.3f", gunshot);
            std::snprintf(footstepOverlay, sizeof(footstepOverlay), "%.3f", footstep);
            ImGui::TextUnformatted("Gunshot");
            ImGui::SameLine();
            ImGui::ProgressBar(gunshot, ImVec2(-FLT_MIN, 0.0f), gunshotOverlay);
            ImGui::TextUnformatted("Footstep");
            ImGui::SameLine();
            ImGui::ProgressBar(footstep, ImVec2(-FLT_MIN, 0.0f), footstepOverlay);
            ImGui::TextDisabled("Scores are live model output before peak/event gating.");
        } else {
            ImGui::TextDisabled("Recognition model scores are unavailable.");
        }
        ImGui::EndTable();
    }
}

void OverlayRenderer::DrawEventTimeline(const std::deque<SoundEvent>& events,
                                         uint64_t currentSample,
                                         uint64_t streamGeneration) {
    const float height = 260.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(220.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 size(width, height);
    ImGui::InvisibleButton("##event_timeline", size);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 panelColor = IM_COL32(13, 20, 32, 255);
    const ImU32 gridColor = IM_COL32(51, 69, 91, 150);
    const ImU32 textColor = IM_COL32(177, 194, 214, 255);
    drawList->AddRectFilled(origin,
                            ImVec2(origin.x + size.x, origin.y + size.y),
                            panelColor, 7.0f);

    const double sampleRate = m_config.sampleRate == 0 ? 48000.0 : m_config.sampleRate;
    const double nowSeconds = static_cast<double>(currentSample) / sampleRate;
    const double startSeconds = std::max(0.0, nowSeconds - m_chartWindowSeconds);
    const float left = origin.x + 92.0f;
    const float right = origin.x + size.x - 16.0f;
    const float top = origin.y + 40.0f;
    const float bottom = origin.y + size.y - 32.0f;
    const float plotWidth = std::max(1.0f, right - left);
    const float laneHeight = (bottom - top) / 2.0f;

    drawList->AddText(ImVec2(origin.x + 14.0f, top + laneHeight * 0.5f - 8.0f),
                      IM_COL32(255, 145, 126, 255), "GUNSHOT");
    drawList->AddText(ImVec2(origin.x + 14.0f, top + laneHeight * 1.5f - 8.0f),
                      IM_COL32(104, 203, 255, 255), "FOOTSTEP");
    drawList->AddLine(ImVec2(left, top + laneHeight), ImVec2(right, top + laneHeight),
                      gridColor, 1.0f);

    constexpr int kGridLines = 6;
    for (int index = 0; index <= kGridLines; ++index) {
        const float fraction = static_cast<float>(index) / kGridLines;
        const float x = left + fraction * plotWidth;
        drawList->AddLine(ImVec2(x, top), ImVec2(x, bottom), gridColor, 1.0f);
        const double time = startSeconds + fraction * m_chartWindowSeconds;
        const double relative = time - nowSeconds;
        DrawText(drawList, ImVec2(x - 18.0f, bottom + 9.0f), textColor, "%+.1fs", relative);
    }

    const auto xForTime = [&](double time) {
        return left + static_cast<float>((time - startSeconds) / m_chartWindowSeconds) * plotWidth;
    };
    for (const auto& event : events) {
        if (event.streamGeneration != streamGeneration) continue;
        const double eventStart = static_cast<double>(event.onsetSample) / sampleRate;
        const double eventEnd = static_cast<double>(event.endSample) / sampleRate;
        if (eventEnd < startSeconds || eventStart > nowSeconds) continue;

        const float x0 = std::clamp(xForTime(eventStart), left, right);
        const float x1 = std::clamp(std::max(xForTime(eventEnd), x0 + 5.0f), left, right);
        const bool gunshot = event.soundClass == SoundClass::Gunshot;
        const float laneTop = top + (gunshot ? 5.0f : laneHeight + 5.0f);
        const float laneBottom = laneTop + laneHeight - 10.0f;
        const ImU32 color = EventColor(event.soundClass);
        drawList->AddRectFilled(ImVec2(x0, laneTop), ImVec2(x1, laneBottom), color, 3.0f);
        drawList->AddCircleFilled(ImVec2(x0, (laneTop + laneBottom) * 0.5f), 4.5f, color);
        if (x1 - x0 > 42.0f) {
            DrawText(drawList, ImVec2(x0 + 9.0f, laneTop + 7.0f),
                     IM_COL32(255, 255, 255, 235), "%.2f", event.confidence);
        }
    }

    if (events.empty()) {
        drawList->AddText(ImVec2(left + 18.0f, top + laneHeight - 18.0f),
                          IM_COL32(129, 148, 174, 255),
                          "Waiting for recognition events...");
    }
}

void OverlayRenderer::DrawRecentEvents(const std::deque<SoundEvent>& events,
                                        uint64_t streamGeneration) {
    if (ImGui::BeginChild("RecentEvents", ImVec2(0.0f, 210.0f), true)) {
        if (ImGui::BeginTable("EventTable", 6,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_ScrollY,
                              ImVec2(0.0f, 0.0f))) {
            ImGui::TableSetupColumn("Time");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Conf.");
            ImGui::TableSetupColumn("Source");
            ImGui::TableSetupColumn("Scene");
            ImGui::TableSetupColumn("Latency");
            ImGui::TableHeadersRow();
            size_t shown = 0;
            const double sampleRate = m_config.sampleRate == 0 ? 48000.0 : m_config.sampleRate;
            for (auto it = events.rbegin(); it != events.rend() && shown < 16; ++it) {
                const SoundEvent& event = *it;
                if (event.streamGeneration != streamGeneration) continue;
                ++shown;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%.2fs", static_cast<double>(event.onsetSample) / sampleRate);
                ImGui::TableNextColumn();
                ImGui::TextColored(event.soundClass == SoundClass::Gunshot
                                       ? ImVec4(1.0f, 0.45f, 0.38f, 1.0f)
                                       : ImVec4(0.35f, 0.78f, 1.0f, 1.0f),
                                   "%s", EventShortName(event.soundClass));
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", event.confidence);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(ToString(event.sourceHint));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(ToString(event.sceneState));
                ImGui::TableNextColumn();
                const int latencyMs = event.deliveredSample >= event.onsetSample
                    ? static_cast<int>((event.deliveredSample - event.onsetSample) * 1000u /
                                       static_cast<uint64_t>(sampleRate))
                    : 0;
                ImGui::Text("%d ms", latencyMs);
            }
            if (shown == 0) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("No events in this stream yet.");
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
    }
}

void OverlayRenderer::DrawTuneTable() {
    if (!m_config.runtimeTuning) {
        ImGui::BeginChild("RecognitionTuningUnavailable", ImVec2(0.0f, 210.0f), true);
        ImGui::TextDisabled("No recognition runtime policy is available.");
        ImGui::EndChild();
        return;
    }

    if (ImGui::Button("Reset to package defaults")) m_config.runtimeTuning->Reset();
    ImGui::SameLine();
    ImGui::TextDisabled("changes apply live on the next audio block");

    RecognitionRuntimeTuning updated = m_config.runtimeTuning->Snapshot();
    bool changed = false;
    if (ImGui::BeginTable("RecognitionTuneTable", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Setting");
        ImGui::TableSetupColumn("Gunshot");
        ImGui::TableSetupColumn("Footstep");
        ImGui::TableSetupColumn("Effect");
        ImGui::TableHeadersRow();

        const auto thresholdRow = [&](const char* name, const char* suffix,
                                      std::array<float, 2>& values,
                                      const char* effect) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name);
            for (size_t index = 0; index < values.size(); ++index) {
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                char id[48]{};
                std::snprintf(id, sizeof(id), "##recognition_%s_%zu", suffix, index);
                changed |= ImGui::SliderFloat(id, &values[index], 0.01f, 1.0f, "%.3f");
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", effect);
        };
        thresholdRow("Quiet threshold", "quiet", updated.quietThresholds,
                     "lower = more alerts");
        thresholdRow("Busy threshold", "busy", updated.busyThresholds,
                     "lower = more alerts");

        const auto millisecondsRow = [&](const char* name, const char* suffix,
                                         std::array<uint32_t, 2>& values,
                                         int minimum, int maximum,
                                         const char* effect) {
            bool rowChanged = false;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name);
            for (size_t index = 0; index < values.size(); ++index) {
                int value = static_cast<int>(values[index]);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                char id[48]{};
                std::snprintf(id, sizeof(id), "##recognition_%s_%zu", suffix, index);
                if (ImGui::SliderInt(id, &value, minimum, maximum, "%d ms")) {
                    values[index] = static_cast<uint32_t>(value);
                    rowChanged = true;
                    changed = true;
                }
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", effect);
            return rowChanged;
        };
        millisecondsRow("Minimum spacing", "spacing", updated.minimumSpacingMs,
                        1, 1000, "debounce");
        std::array<uint32_t, 2> onsetOffsetMs{};
        for (size_t index = 0; index < onsetOffsetMs.size(); ++index) {
            onsetOffsetMs[index] = static_cast<uint32_t>(
                (static_cast<uint64_t>(updated.onsetOffsetSamples[index]) * 1000u + 24000u) /
                48000u);
        }
        const bool onsetOffsetChanged = millisecondsRow(
            "Onset offset", "offset", onsetOffsetMs, 0, 50, "timing correction");
        if (onsetOffsetChanged) {
            for (size_t index = 0; index < onsetOffsetMs.size(); ++index) {
                updated.onsetOffsetSamples[index] = static_cast<uint32_t>(
                    static_cast<uint64_t>(onsetOffsetMs[index]) * 48000u / 1000u);
            }
        }

        const auto sharedFloatRow = [&](const char* name, const char* id,
                                        float& value, float minimum, float maximum,
                                        const char* effect) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= ImGui::SliderFloat(id, &value, minimum, maximum, "%.3f");
            ImGui::TableNextColumn();
            ImGui::TextDisabled("shared");
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", effect);
        };
        sharedFloatRow("Scene activity cutoff", "##recognition_scene", updated.sceneActivityCutoff,
                       0.01f, 0.99f, "quiet / busy split");
        sharedFloatRow("Self suppression", "##recognition_self", updated.selfSuppressionThreshold,
                       0.01f, 1.0f, "hide local audio");

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Pulse width");
        ImGui::TableNextColumn();
        int pulseMs = static_cast<int>(updated.pulseMs);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##recognition_pulse", &pulseMs, 1, 500, "%d ms")) {
            updated.pulseMs = static_cast<uint32_t>(pulseMs);
            changed = true;
        }
        ImGui::TableNextColumn();
        ImGui::TextDisabled("shared");
        ImGui::TableNextColumn();
        ImGui::TextDisabled("chart bar length");
        ImGui::EndTable();
    }

    if (changed) m_config.runtimeTuning->Update(updated);
    ImGui::TextDisabled("Locked model shape: 1024 FFT / 240 hop / 64 mel / 128-frame context / %u-frame peak lookahead",
                        m_config.peakLookaheadFrames);
}

void OverlayRenderer::DrawDirectionPage(
    const std::deque<DirectionSceneRecord>& scenes) {
    if (!m_config.runtimeSettings) {
        ImGui::TextDisabled("Direction settings are unavailable.");
        return;
    }

    AppSettings settings = m_config.runtimeSettings->Snapshot();
    bool changed = false;
    ImGui::Text("Direction model: %s",
                m_config.directionModelVersion.empty()
                    ? "unavailable"
                    : m_config.directionModelVersion.c_str());
    if (!m_config.directionError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.58f, 0.32f, 1.0f),
                           "Direction paused: %s",
                           m_config.directionError.c_str());
    }

    if (ImGui::BeginTable("DirectionSettings", 2,
                          ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Borders |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Class");
        ImGui::TableSetupColumn("Enabled");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Footsteps");
        ImGui::TableNextColumn();
        changed |= ImGui::Checkbox(
            "##direction_footsteps",
            &settings.direction.enableFootsteps);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Gunshots");
        ImGui::TableNextColumn();
        changed |= ImGui::Checkbox(
            "##direction_gunshots",
            &settings.direction.enableGunshots);
        ImGui::EndTable();
    }
    if (changed) {
        m_config.runtimeSettings->Update(settings, true, nullptr);
    }

    ImGui::Spacing();
    if (!m_playingClipPath.empty()) {
        if (ImGui::SmallButton("Stop playback")) StopClip();
        ImGui::SameLine();
        ImGui::TextDisabled("Last played: %s",
                            m_playingClipPath.filename().string().c_str());
    }
    if (!m_clipPlaybackError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                           "Clip playback: %s",
                           m_clipPlaybackError.c_str());
    }

    ImGui::TextUnformatted("Recent direction scenes");
    if (ImGui::BeginTable(
            "DirectionScenes", 9,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                ImGuiTableFlags_ScrollY,
            ImVec2(0.0f, 260.0f))) {
        ImGui::TableSetupColumn("Scene");
        ImGui::TableSetupColumn("Source");
        ImGui::TableSetupColumn("Events");
        ImGui::TableSetupColumn("Azimuth");
        ImGui::TableSetupColumn("Elevation");
        ImGui::TableSetupColumn("Conf.");
        ImGui::TableSetupColumn("Arc");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("Audio");
        ImGui::TableHeadersRow();

        size_t shownScenes = 0;
        for (auto sceneIterator = scenes.rbegin();
             sceneIterator != scenes.rend() && shownScenes < 24;
             ++sceneIterator, ++shownScenes) {
            const auto& record = *sceneIterator;
            std::string eventSummary;
            for (size_t index = 0; index < record.events.size(); ++index) {
                if (index != 0) eventSummary += ", ";
                eventSummary += std::to_string(record.events[index].eventId);
                eventSummary += ':';
                eventSummary += ToString(
                    record.events[index].event.soundClass);
            }

            const uint32_t rows = std::max<uint32_t>(
                1, std::min<uint32_t>(
                    record.direction.sourceCount,
                    static_cast<uint32_t>(
                        DirectionSceneResult::kMaximumSources)));
            for (uint32_t sourceIndex = 0;
                 sourceIndex < rows; ++sourceIndex) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%llu",
                            static_cast<unsigned long long>(
                                record.direction.sceneId));
                ImGui::TableNextColumn();
                if (record.direction.sourceCount == 0) {
                    ImGui::TextDisabled("-");
                } else {
                    ImGui::Text("%u/%u", sourceIndex + 1,
                                record.direction.sourceCount);
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(eventSummary.c_str());
                ImGui::TableNextColumn();
                if (record.direction.sourceCount == 0) {
                    ImGui::TextDisabled("-");
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("-");
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("-");
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("-");
                } else {
                    const auto& source =
                        record.direction.sources[sourceIndex];
                    ImGui::Text("%.0f deg", source.azimuthDegrees);
                    ImGui::TableNextColumn();
                    ImGui::Text("%+.0f deg", source.elevationDegrees);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", source.confidence);
                    ImGui::TableNextColumn();
                    ImGui::Text("+/-%.0f",
                                source.uncertaintyDegrees);
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    ToString(record.direction.status));
                ImGui::TableNextColumn();
                if (sourceIndex != 0) {
                    ImGui::TextDisabled("shared");
                } else if (record.clipPath.empty()) {
                    ImGui::TextDisabled("n/a");
                } else {
                    const std::string buttonId =
                        "Play##scene_" +
                        std::to_string(record.direction.sceneId);
                    if (ImGui::SmallButton(buttonId.c_str())) {
                        PlayClip(record.clipPath);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "%s", record.clipPath.string().c_str());
                    }
                }
            }
        }
        if (shownScenes == 0) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled(
                "Waiting for direction scenes...");
        }
        ImGui::EndTable();
    }
}

#ifdef _WIN32

void OverlayRenderer::PlayClip(const std::filesystem::path& path) {
    m_clipPlaybackError.clear();
    if (path.empty()) {
        m_clipPlaybackError = "No audio clip was saved for this event.";
        return;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        m_clipPlaybackError = "The saved WAV file is no longer available.";
        return;
    }
    const std::wstring widePath = path.wstring();
    if (!PlaySoundW(widePath.c_str(), nullptr,
                    SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) {
        m_clipPlaybackError = "Windows could not start WAV playback.";
        return;
    }
    m_playingClipPath = path;
}

void OverlayRenderer::StopClip() {
    PlaySoundW(nullptr, nullptr, 0);
    m_playingClipPath.clear();
}

#endif

void OverlayRenderer::DrawOverlayPage() {
    if (!m_config.runtimeSettings) return;
    AppSettings settings = m_config.runtimeSettings->Snapshot();
    bool changed = false;
    ImGui::TextWrapped("The direction HUD is a separate click-through topmost window. "
                       "CS2 must use Fullscreen Windowed/Borderless mode.");
    const char* visibilityNames[]{"Off", "CS2 only", "Always visible"};
    int visibility = static_cast<int>(settings.overlay.visibility);
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::Combo("Visibility", &visibility, visibilityNames, 3)) {
        settings.overlay.visibility = static_cast<OverlaySettings::Visibility>(visibility);
        changed = true;
    }
    changed |= ImGui::SliderFloat("Radius", &settings.overlay.radiusPixels,
                                  40.0f, 400.0f, "%.0f px");
    changed |= ImGui::SliderFloat("Thickness", &settings.overlay.thicknessPixels,
                                  2.0f, 32.0f, "%.1f px");
    changed |= ImGui::SliderFloat("Opacity", &settings.overlay.opacity,
                                  0.05f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Horizontal offset", &settings.overlay.offsetX,
                                  -800.0f, 800.0f, "%.0f px");
    changed |= ImGui::SliderFloat("Vertical offset", &settings.overlay.offsetY,
                                  -800.0f, 800.0f, "%.0f px");
    changed |= ImGui::SliderFloat("Footstep lifetime",
                                  &settings.overlay.footstepLifetimeSeconds,
                                  0.1f, 5.0f, "%.1f s");
    changed |= ImGui::SliderFloat("Gunshot lifetime",
                                  &settings.overlay.gunshotLifetimeSeconds,
                                  0.1f, 5.0f, "%.1f s");
    changed |= ImGui::Checkbox("Center dot", &settings.overlay.showCenterDot);
    ImGui::TextDisabled("Global HUD hotkey: Ctrl+Alt+O");
    if (changed) m_config.runtimeSettings->Update(settings, true, nullptr);
}

void OverlayRenderer::DrawAudioSystemPage() {
    ImGui::Text("Recognition model: %s",
                m_config.modelVersion.empty() ? "unavailable" : m_config.modelVersion.c_str());
    if (m_config.runtimeSettings) {
        AppSettings settings = m_config.runtimeSettings->Snapshot();
        ImGui::TextUnformatted("Interface");
        ImGui::SetNextItemWidth(280.0f);
        float uiScalePercent = settings.uiScale * 100.0f;
        if (ImGui::SliderFloat("UI scale", &uiScalePercent,
                              AppSettings::kMinUiScale * 100.0f,
                              AppSettings::kMaxUiScale * 100.0f, "%.0f%%")) {
            settings.uiScale = std::clamp(
                uiScalePercent / 100.0f, AppSettings::kMinUiScale, AppSettings::kMaxUiScale);
            m_config.runtimeSettings->Update(settings, true, nullptr);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset UI scale")) {
            settings.uiScale = AppSettings::kDefaultUiScale;
            m_config.runtimeSettings->Update(settings, true, nullptr);
        }
        ImGui::TextDisabled("Changes apply immediately and are saved to settings.json.");
        ImGui::Separator();
        ImGui::TextWrapped("Settings: %s",
                           m_config.runtimeSettings->Path().string().c_str());
        if (ImGui::Checkbox("Write schema-2 direction JSONL session log",
                            &settings.sessionLogging)) {
            m_config.runtimeSettings->Update(settings, true, nullptr);
        }
        if (ImGui::Button("Save settings now")) {
            m_config.runtimeSettings->Save(nullptr);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset app settings")) {
            m_config.runtimeSettings->Reset(true);
        }
    }
    ImGui::Separator();
    ImGui::TextWrapped("Capture contract: Windows system-output loopback, 48 kHz, "
                       "interleaved stereo. Disable third-party mono conversion or "
                       "untracked spatial processing for repeatable model input.");
}

#else

void OverlayRenderer::DrawUi() {}

void OverlayRenderer::DrawEventTimeline(const std::deque<SoundEvent>&,
                                         uint64_t, uint64_t) {}

void OverlayRenderer::DrawRecentEvents(const std::deque<SoundEvent>&,
                                        uint64_t) {}

void OverlayRenderer::DrawTuneTable() {}

void OverlayRenderer::DrawDirectionPage(const std::deque<DirectionSceneRecord>&) {}

void OverlayRenderer::DrawOverlayPage() {}

void OverlayRenderer::DrawAudioSystemPage() {}

#endif

} // namespace EchoRadar
