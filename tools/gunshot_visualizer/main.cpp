#include <audio/AudioCapture.h>
#include <audio/AudioDeviceManager.h>
#include <detector/GunshotEventDetector.h>
#include <dsp/STFTProcessor.h>
#include <features/FeatureExtractor.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace EchoRadar;

namespace {

#ifdef _WIN32
constexpr size_t kReadChunkFrames = 480;
constexpr int kDefaultHistoryLength = 1000;
constexpr float kDefaultTimeWindowSec = 8.0f;
constexpr float kMinTimeWindowSec = 5.0f;
constexpr float kMaxTimeWindowSec = 10.0f;
constexpr auto kEventHighlightDuration = std::chrono::milliseconds(500);
#endif

std::atomic<bool> g_running{true};

void OnSignal(int) {
    g_running.store(false, std::memory_order_relaxed);
}

#ifdef _WIN32
const char* StateToString(DetectorState state) {
    switch (state) {
    case DetectorState::Idle: return "Idle";
    case DetectorState::InCandidate: return "InCandidate";
    case DetectorState::Cooldown: return "Cooldown";
    }
    return "Unknown";
}

class FloatHistoryBuffer {
public:
    explicit FloatHistoryBuffer(size_t capacity = kDefaultHistoryLength) {
        Resize(capacity);
    }

    void Push(float value) {
        if (m_data.empty()) {
            return;
        }
        m_data[m_head] = value;
        m_head = (m_head + 1) % m_data.size();
        m_size = std::min(m_size + 1, m_data.size());
    }

    void Resize(size_t newCapacity) {
        const size_t safeCapacity = std::max<size_t>(32, newCapacity);
        std::vector<float> ordered;
        CopyOrdered(ordered);

        m_data.assign(safeCapacity, 0.0f);
        m_head = 0;
        m_size = 0;

        const size_t keep = std::min(safeCapacity, ordered.size());
        for (size_t i = ordered.size() - keep; i < ordered.size(); ++i) {
            Push(ordered[i]);
        }
    }

    void CopyOrdered(std::vector<float>& out) const {
        out.clear();
        out.reserve(m_size);
        if (m_size == 0 || m_data.empty()) {
            return;
        }

        const size_t cap = m_data.size();
        const size_t start = (m_head + cap - m_size) % cap;
        for (size_t i = 0; i < m_size; ++i) {
            out.push_back(m_data[(start + i) % cap]);
        }
    }

private:
    std::vector<float> m_data;
    size_t m_head{0};
    size_t m_size{0};
};

struct EventMarker {
    double peakTimeSec{0.0};
    float peakScore{0.0f};
};

struct SharedDashboardData {
    mutable std::mutex mutex;

    FloatHistoryBuffer scoreHistory{kDefaultHistoryLength};
    FloatHistoryBuffer thresholdHistory{kDefaultHistoryLength};
    FloatHistoryBuffer energyHistory{kDefaultHistoryLength};
    FloatHistoryBuffer energyRiseHistory{kDefaultHistoryLength};
    FloatHistoryBuffer transientHistory{kDefaultHistoryLength};
    FloatHistoryBuffer hfRatioHistory{kDefaultHistoryLength};
    FloatHistoryBuffer fluxHistory{kDefaultHistoryLength};

    std::deque<EventMarker> eventMarkers;
    std::optional<GunshotEvent> lastEvent;

    DetectorState state{DetectorState::Idle};
    float currentScore{0.0f};
    float currentThreshold{EventDetectorConfig{}.triggerThreshold};
    float releaseThreshold{EventDetectorConfig{}.releaseThreshold};
    uint64_t eventCount{0};
    size_t bufferedPcmFrames{0};
    double currentTimeSec{0.0};
    double frameStepSec{512.0 / 48000.0};
    int historyLength{kDefaultHistoryLength};
    bool autoScale{true};
    std::chrono::steady_clock::time_point highlightUntil{};

    void ResizeHistories(int newLength) {
        const int clamped = std::clamp(newLength, 100, 4000);
        historyLength = clamped;
        scoreHistory.Resize(static_cast<size_t>(clamped));
        thresholdHistory.Resize(static_cast<size_t>(clamped));
        energyHistory.Resize(static_cast<size_t>(clamped));
        energyRiseHistory.Resize(static_cast<size_t>(clamped));
        transientHistory.Resize(static_cast<size_t>(clamped));
        hfRatioHistory.Resize(static_cast<size_t>(clamped));
        fluxHistory.Resize(static_cast<size_t>(clamped));
    }
};

struct DashboardSnapshot {
    DetectorState state{DetectorState::Idle};
    float currentScore{0.0f};
    float currentThreshold{0.0f};
    float releaseThreshold{0.0f};
    uint64_t eventCount{0};
    size_t bufferedPcmFrames{0};
    double currentTimeSec{0.0};
    double frameStepSec{512.0 / 48000.0};
    int historyLength{kDefaultHistoryLength};
    bool autoScale{true};
    bool highlightActive{false};
    std::optional<GunshotEvent> lastEvent;
    std::vector<EventMarker> events;

    std::vector<float> score;
    std::vector<float> threshold;
    std::vector<float> energy;
    std::vector<float> energyRise;
    std::vector<float> transient;
    std::vector<float> hfRatio;
    std::vector<float> flux;
};

DashboardSnapshot BuildSnapshot(const SharedDashboardData& shared) {
    DashboardSnapshot snap{};
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(shared.mutex);

    snap.state = shared.state;
    snap.currentScore = shared.currentScore;
    snap.currentThreshold = shared.currentThreshold;
    snap.releaseThreshold = shared.releaseThreshold;
    snap.eventCount = shared.eventCount;
    snap.bufferedPcmFrames = shared.bufferedPcmFrames;
    snap.currentTimeSec = shared.currentTimeSec;
    snap.frameStepSec = shared.frameStepSec;
    snap.historyLength = shared.historyLength;
    snap.autoScale = shared.autoScale;
    snap.highlightActive = now < shared.highlightUntil;
    snap.lastEvent = shared.lastEvent;

    snap.events.assign(shared.eventMarkers.begin(), shared.eventMarkers.end());
    shared.scoreHistory.CopyOrdered(snap.score);
    shared.thresholdHistory.CopyOrdered(snap.threshold);
    shared.energyHistory.CopyOrdered(snap.energy);
    shared.energyRiseHistory.CopyOrdered(snap.energyRise);
    shared.transientHistory.CopyOrdered(snap.transient);
    shared.hfRatioHistory.CopyOrdered(snap.hfRatio);
    shared.fluxHistory.CopyOrdered(snap.flux);

    return snap;
}

std::vector<float> SliceTail(const std::vector<float>& values, size_t count) {
    if (values.size() <= count) {
        return values;
    }
    return std::vector<float>(values.end() - static_cast<std::ptrdiff_t>(count), values.end());
}

void ComputePlotRange(const std::vector<float>& values,
                      bool autoScale,
                      float fixedMin,
                      float fixedMax,
                      float& outMin,
                      float& outMax) {
    if (!autoScale || values.empty()) {
        outMin = fixedMin;
        outMax = fixedMax;
        return;
    }

    const auto [minIt, maxIt] = std::minmax_element(values.begin(), values.end());
    outMin = *minIt;
    outMax = *maxIt;
    if (outMin == outMax) {
        outMin -= 0.1f;
        outMax += 0.1f;
        return;
    }
    const float pad = (outMax - outMin) * 0.12f;
    outMin -= pad;
    outMax += pad;
}

#endif

int ParseCommonArgs(int argc,
                    char* argv[],
                    std::string& deviceArg,
                    bool& listOnly) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--list-devices" || arg == "-l") {
            listOnly = true;
        } else if (arg == "--device" || arg == "-d") {
            if (i + 1 < argc) {
                deviceArg = argv[++i];
            } else {
                std::fprintf(stderr, "[Error] --device requires a device name argument\n");
                return 1;
            }
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "Usage: gunshot_visualizer [options]\n\n"
                "Options:\n"
                "  --list-devices              List available input devices\n"
                "  --device <name>             Capture from a specific device (case-insensitive match)\n"
                "  --help                      Show this help message\n");
            return 0;
        }
    }
    return -1;
}

void PrintDeviceList(const std::vector<AudioDeviceInfo>& devices) {
    std::printf("[EchoRadar] Available input devices (%zu):\n", devices.size());
    for (size_t i = 0; i < devices.size(); ++i) {
        std::printf("  [%zu] %s%s\n",
                    i,
                    devices[i].name.c_str(),
                    devices[i].isDefault ? "  <default>" : "");
    }
}

} // namespace

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

namespace {

ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView != nullptr) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    const HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        featureLevelArray,
        2,
        D3D11_SDK_VERSION,
        &sd,
        &g_pSwapChain,
        &g_pd3dDevice,
        &featureLevel,
        &g_pd3dDeviceContext);
    if (result != S_OK) {
        return false;
    }

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain != nullptr) {
        g_pSwapChain->Release();
        g_pSwapChain = nullptr;
    }
    if (g_pd3dDeviceContext != nullptr) {
        g_pd3dDeviceContext->Release();
        g_pd3dDeviceContext = nullptr;
    }
    if (g_pd3dDevice != nullptr) {
        g_pd3dDevice->Release();
        g_pd3dDevice = nullptr;
    }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
        return true;
    }

    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0,
                                        static_cast<UINT>(LOWORD(lParam)),
                                        static_cast<UINT>(HIWORD(lParam)),
                                        DXGI_FORMAT_UNKNOWN,
                                        0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) {
            return 0;
        }
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

ImVec4 DetectorStateColor(DetectorState state, bool triggered) {
    if (triggered) {
        return ImVec4(1.00f, 0.22f, 0.22f, 1.00f); // Triggered -> red
    }
    switch (state) {
    case DetectorState::Idle:
        return ImVec4(0.25f, 0.90f, 0.35f, 1.00f); // Idle -> green
    case DetectorState::InCandidate:
        return ImVec4(1.00f, 0.88f, 0.20f, 1.00f); // Candidate -> yellow
    case DetectorState::Cooldown:
        return ImVec4(1.00f, 0.62f, 0.20f, 1.00f); // Cooldown -> orange
    }
    return ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
}

void RenderSeriesPlot(const char* label,
                      const std::vector<float>& series,
                      bool autoScale,
                      float fixedMin,
                      float fixedMax,
                      float height = 90.0f) {
    if (series.empty()) {
        ImGui::TextDisabled("%s: waiting for audio frames...", label);
        return;
    }

    float yMin = fixedMin;
    float yMax = fixedMax;
    ComputePlotRange(series, autoScale, fixedMin, fixedMax, yMin, yMax);
    ImGui::PlotLines(label,
                     series.data(),
                     static_cast<int>(series.size()),
                     0,
                     nullptr,
                     yMin,
                     yMax,
                     ImVec2(-1.0f, height));
}

void RenderScorePlot(const std::vector<float>& score,
                     const std::vector<float>& threshold,
                     bool autoScale) {
    if (score.empty()) {
        ImGui::TextDisabled("Score: waiting for audio frames...");
        return;
    }

    float yMin = 0.0f;
    float yMax = 1.0f;
    ComputePlotRange(score, autoScale, 0.0f, 1.0f, yMin, yMax);
    if (!threshold.empty()) {
        const auto [thMinIt, thMaxIt] = std::minmax_element(threshold.begin(), threshold.end());
        yMin = std::min(yMin, *thMinIt);
        yMax = std::max(yMax, *thMaxIt);
    }

    ImGui::PlotLines("Score",
                     score.data(),
                     static_cast<int>(score.size()),
                     0,
                     nullptr,
                     yMin,
                     yMax,
                     ImVec2(-1.0f, 150.0f));

    if (!threshold.empty() && yMax > yMin) {
        const float currentThreshold = threshold.back();
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const float normalized = (currentThreshold - yMin) / (yMax - yMin);
        const float y = max.y - normalized * (max.y - min.y);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), IM_COL32(255, 100, 100, 255), 2.0f);
    }
}

void RenderEventTimeline(const DashboardSnapshot& snapshot, float windowSec) {
    ImGui::SeparatorText("Event Timeline");
    const ImVec2 canvasSize(ImGui::GetContentRegionAvail().x, 90.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("timeline_canvas", canvasSize);
    const ImVec2 p1 = ImGui::GetItemRectMax();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p0, p1, IM_COL32(30, 30, 35, 255), 4.0f);
    draw->AddRect(p0, p1, IM_COL32(90, 90, 100, 255), 4.0f);

    const float left = p0.x + 10.0f;
    const float right = p1.x - 10.0f;
    const float centerY = (p0.y + p1.y) * 0.5f;
    draw->AddLine(ImVec2(left, centerY), ImVec2(right, centerY), IM_COL32(140, 140, 140, 255), 1.0f);

    const double tEnd = snapshot.currentTimeSec;
    const double tStart = tEnd - windowSec;
    for (const EventMarker& marker : snapshot.events) {
        if (marker.peakTimeSec < tStart || marker.peakTimeSec > tEnd) {
            continue;
        }
        const float t = static_cast<float>((marker.peakTimeSec - tStart) / windowSec);
        const float x = left + t * (right - left);
        draw->AddLine(ImVec2(x, centerY - 22.0f), ImVec2(x, centerY + 22.0f), IM_COL32(255, 90, 90, 255), 2.0f);

        char label[16];
        std::snprintf(label, sizeof(label), "%.2f", marker.peakScore);
        draw->AddText(ImVec2(x + 2.0f, centerY - 34.0f), IM_COL32(255, 190, 120, 255), label);
    }

    char startLabel[32];
    char endLabel[32];
    std::snprintf(startLabel, sizeof(startLabel), "%.2fs", tStart);
    std::snprintf(endLabel, sizeof(endLabel), "%.2fs", tEnd);
    draw->AddText(ImVec2(left, p1.y - 18.0f), IM_COL32(180, 180, 180, 255), startLabel);
    draw->AddText(ImVec2(right - 52.0f, p1.y - 18.0f), IM_COL32(180, 180, 180, 255), endLabel);
}

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, OnSignal);

    std::string deviceArg;
    bool listOnly = false;
    const int argParseResult = ParseCommonArgs(argc, argv, deviceArg, listOnly);
    if (argParseResult != -1) {
        return argParseResult;
    }

    AudioDeviceManager deviceManager;
    const auto devices = deviceManager.GetInputDevices();
    if (listOnly) {
        PrintDeviceList(devices);
        return 0;
    }

    if (devices.empty()) {
        std::fprintf(stderr, "[Error] No input devices available.\n");
        return 1;
    }

    AudioCapture capture;
    bool started = false;

    std::printf("[EchoRadar Gunshot Visualizer]\n");
    if (!deviceArg.empty()) {
        const auto found = deviceManager.FindInputDeviceByName(deviceArg);
        if (found) {
            std::printf("Using device: %s\n\n", found->name.c_str());
        }
        started = capture.StartDeviceByName(deviceArg);
    }
    if (!started) {
        const auto def = deviceManager.GetDefaultInputDevice();
        std::printf("Using device: %s\n\n", def ? def->name.c_str() : "default");
        started = capture.StartDefault();
    }
    if (!started) {
        std::fprintf(stderr, "[Error] Failed to start audio capture.\n");
        return 1;
    }

    SharedDashboardData shared;
    std::atomic<float> requestedThreshold{shared.currentThreshold};
    std::atomic<bool> thresholdDirty{false};

    std::thread dspThread([&]() {
        STFTProcessor stft;
        FeatureExtractor extractor;
        GunshotEventDetector detector;
        detector.SetTriggerThreshold(requestedThreshold.load(std::memory_order_relaxed));

        std::vector<float> interleaved(kReadChunkFrames * 2, 0.0f);
        while (g_running.load(std::memory_order_relaxed) && capture.IsRunning()) {
            if (thresholdDirty.exchange(false, std::memory_order_acq_rel)) {
                detector.SetTriggerThreshold(requestedThreshold.load(std::memory_order_relaxed));
            }

            bool didWork = false;

            size_t pulled = 0;
            do {
                pulled = capture.ReadInterleaved(interleaved.data(), kReadChunkFrames);
                if (pulled > 0) {
                    stft.PushInterleaved(interleaved.data(), pulled);
                    didWork = true;
                }
            } while (pulled == kReadChunkFrames);

            STFTFrame frame;
            while (stft.PopFrame(frame)) {
                didWork = true;

                const AudioFeatures features = extractor.Extract(frame);
                const double frameTimeSec = static_cast<double>(frame.start_sample) /
                                            static_cast<double>(frame.sample_rate);
                detector.PushFrame(features, frameTimeSec, static_cast<int>(frame.frame_index));

                std::lock_guard<std::mutex> lock(shared.mutex);
                shared.state = detector.GetState();
                shared.currentScore = detector.GetLastScore();
                shared.currentThreshold = detector.GetTriggerThreshold();
                shared.releaseThreshold = detector.GetReleaseThreshold();
                shared.currentTimeSec = frameTimeSec;
                shared.frameStepSec = static_cast<double>(frame.hop_size) /
                                      static_cast<double>(frame.sample_rate);

                shared.scoreHistory.Push(shared.currentScore);
                shared.thresholdHistory.Push(shared.currentThreshold);
                shared.energyHistory.Push(features.totalEnergy);
                shared.energyRiseHistory.Push(features.energyRise);
                shared.transientHistory.Push(features.transientScore);
                shared.hfRatioHistory.Push(features.hfEnergyRatio);
                shared.fluxHistory.Push(features.spectralFlux);
            }

            GunshotEvent ev;
            while (detector.PopEvent(ev)) {
                didWork = true;
                std::lock_guard<std::mutex> lock(shared.mutex);
                ++shared.eventCount;
                shared.lastEvent = ev;
                shared.highlightUntil = std::chrono::steady_clock::now() + kEventHighlightDuration;
                shared.eventMarkers.push_back(EventMarker{ev.peakTimeSec, ev.candidateScore});

                const double keepTimeSec = 30.0;
                const double currentTime = shared.currentTimeSec;
                while (!shared.eventMarkers.empty() &&
                       (currentTime - shared.eventMarkers.front().peakTimeSec) > keepTimeSec) {
                    shared.eventMarkers.pop_front();
                }
                while (shared.eventMarkers.size() > 512) {
                    shared.eventMarkers.pop_front();
                }
            }

            {
                std::lock_guard<std::mutex> lock(shared.mutex);
                shared.bufferedPcmFrames = capture.GetAvailableFrames();
            }

            if (!didWork) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        g_running.store(false, std::memory_order_relaxed);
    });

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"EchoRadarGunshotVisualizerClass";
    ::RegisterClassExW(&wc);

    HWND hwnd = ::CreateWindowW(wc.lpszClassName,
                                L"EchoRadar - Gunshot Detection Visualizer",
                                WS_OVERLAPPEDWINDOW,
                                100,
                                100,
                                1320,
                                920,
                                nullptr,
                                nullptr,
                                wc.hInstance,
                                nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        g_running.store(false, std::memory_order_relaxed);
        dspThread.join();
        capture.Stop();
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    float displayWindowSec = kDefaultTimeWindowSec;
    bool done = false;
    while (!done && g_running.load(std::memory_order_relaxed)) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
        }
        if (done) {
            break;
        }

        DashboardSnapshot snapshot = BuildSnapshot(shared);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowSize(ImVec2(1260, 860), ImGuiCond_FirstUseEver);
        ImGui::Begin("Gunshot Detection Debug Dashboard");

        int historyLengthUi = snapshot.historyLength;
        if (ImGui::SliderInt("History Length", &historyLengthUi, 200, 2000)) {
            std::lock_guard<std::mutex> lock(shared.mutex);
            shared.ResizeHistories(historyLengthUi);
        }

        float thresholdUi = snapshot.currentThreshold;
        const float thresholdMin = snapshot.releaseThreshold + 0.01f;
        if (ImGui::SliderFloat("Trigger Threshold", &thresholdUi, thresholdMin, 1.00f, "%.3f")) {
            requestedThreshold.store(thresholdUi, std::memory_order_relaxed);
            thresholdDirty.store(true, std::memory_order_release);
        }

        bool autoScaleUi = snapshot.autoScale;
        if (ImGui::Checkbox("Auto Scale", &autoScaleUi)) {
            std::lock_guard<std::mutex> lock(shared.mutex);
            shared.autoScale = autoScaleUi;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(230.0f);
        ImGui::SliderFloat("Display Window (s)", &displayWindowSec, kMinTimeWindowSec, kMaxTimeWindowSec, "%.1f s");

        ImGui::SeparatorText("Status");
        const bool triggered = snapshot.highlightActive;
        const ImVec4 stateColor = DetectorStateColor(snapshot.state, triggered);
        ImGui::Text("Detector State:");
        ImGui::SameLine();
        ImGui::TextColored(stateColor, "%s", triggered ? "Triggered" : StateToString(snapshot.state));

        ImGui::Text("Current Score: %.3f", snapshot.currentScore);
        ImGui::Text("Current Threshold: %.3f", snapshot.currentThreshold);
        ImGui::Text("Detected: %s", triggered ? "YES" : "NO");
        ImGui::Text("Events: %llu", static_cast<unsigned long long>(snapshot.eventCount));
        ImGui::Text("Buffered PCM: %zu frames", snapshot.bufferedPcmFrames);

        if (triggered) {
            ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f), "Gunshot Detected!");
        }

        if (snapshot.lastEvent.has_value()) {
            const GunshotEvent& ev = *snapshot.lastEvent;
            const double durationMs = (ev.endFrame >= ev.startFrame)
                ? static_cast<double>(ev.endFrame - ev.startFrame + 1) * snapshot.frameStepSec * 1000.0
                : 0.0;
            ImGui::SeparatorText("LAST EVENT");
            ImGui::Text("Peak Score: %.3f", ev.candidateScore);
            ImGui::Text("Duration: %.1f ms", durationMs);
            ImGui::Text("Peak Time: %.3f s", ev.peakTimeSec);
        }

        const size_t windowPoints = std::max<size_t>(
            1,
            static_cast<size_t>(displayWindowSec / std::max(1e-5, snapshot.frameStepSec)));

        const std::vector<float> scoreWindow = SliceTail(snapshot.score, windowPoints);
        const std::vector<float> thresholdWindow = SliceTail(snapshot.threshold, windowPoints);
        const std::vector<float> energyWindow = SliceTail(snapshot.energy, windowPoints);
        const std::vector<float> energyRiseWindow = SliceTail(snapshot.energyRise, windowPoints);
        const std::vector<float> transientWindow = SliceTail(snapshot.transient, windowPoints);
        const std::vector<float> hfWindow = SliceTail(snapshot.hfRatio, windowPoints);
        const std::vector<float> fluxWindow = SliceTail(snapshot.flux, windowPoints);

        ImGui::SeparatorText("Score Curve");
        RenderScorePlot(scoreWindow, thresholdWindow, snapshot.autoScale);
        ImGui::TextDisabled("Window: %.1f s | Samples: %zu", displayWindowSec, scoreWindow.size());
        ImGui::TextDisabled("Red line = Trigger Threshold");

        ImGui::SeparatorText("Feature Curves");
        RenderSeriesPlot("Energy", energyWindow, snapshot.autoScale, 0.0f, 1.0f);
        RenderSeriesPlot("Energy Rise", energyRiseWindow, snapshot.autoScale, 0.0f, 1.0f);
        RenderSeriesPlot("Transient Score", transientWindow, snapshot.autoScale, 0.0f, 1.0f);
        RenderSeriesPlot("HF Energy Ratio", hfWindow, snapshot.autoScale, 0.0f, 1.0f);
        RenderSeriesPlot("Spectral Flux", fluxWindow, snapshot.autoScale, 0.0f, 1.0f);

        RenderEventTimeline(snapshot, displayWindowSec);

        ImGui::End();

        ImGui::Render();
        const float clearColor[4] = {0.06f, 0.07f, 0.09f, 1.00f};
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    g_running.store(false, std::memory_order_relaxed);
    if (dspThread.joinable()) {
        dspThread.join();
    }
    capture.Stop();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

#else

int main(int argc, char* argv[]) {
    std::signal(SIGINT, OnSignal);

    std::string deviceArg;
    bool listOnly = false;
    const int argParseResult = ParseCommonArgs(argc, argv, deviceArg, listOnly);
    if (argParseResult != -1) {
        return argParseResult;
    }

    AudioDeviceManager deviceManager;
    const auto devices = deviceManager.GetInputDevices();
    if (listOnly) {
        PrintDeviceList(devices);
        return 0;
    }

    std::fprintf(stderr,
                 "gunshot_visualizer requires Windows (Dear ImGui + DirectX 11 backend).\n");
    return 1;
}

#endif
