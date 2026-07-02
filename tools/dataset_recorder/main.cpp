#include <audio/AudioCapture.h>
#include <audio/AudioDeviceManager.h>
#include <audio/AudioHistoryBuffer.h>
#include <detector/GunshotEventDetector.h>
#include <dsp/STFTProcessor.h>
#include <features/FeatureExtractor.h>
#include <features/FeatureHistoryBuffer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace EchoRadar;
namespace fs = std::filesystem;

namespace {

constexpr size_t kReadChunkFrames = 480;
constexpr double kHistorySeconds = 3.0;
constexpr double kPreEventSeconds = 0.2;
constexpr double kPostEventSeconds = 0.2;
constexpr int kRecentEventsMax = 10;
constexpr float kMinTimeWindowSec = 0.4f;
constexpr float kMaxTimeWindowSec = 3.0f;

std::atomic<bool> g_running{true};

void OnSignal(int) {
    g_running.store(false, std::memory_order_relaxed);
}

std::string JsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
        case '\"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

struct DatasetRecord {
    std::string eventId;
    std::string eventType; // candidate / gunshot / ambient
    std::string datasetRoot;
    std::string labelFolder; // currently unknown
    std::string deviceName;
    uint64_t timestampMs{0};
    uint32_t sampleRate{48000};
    uint32_t fftSize{1024};
    uint32_t hopSize{512};
    float detectorScore{0.0f};
    float candidateScore{0.0f};
    float confidence{0.0f};
    float triggerThreshold{0.0f};
    uint64_t windowStartSample{0};
    std::vector<float> audioInterleaved;
    std::vector<FeatureHistoryEntry> features;
};

struct CompletedEvent {
    std::string eventId;
    std::string eventType;
    std::string folderPath;
    std::string audioPath;
    std::string csvPath;
    std::string jsonPath;
    uint64_t timestampMs{0};
    float candidateScore{0.0f};
    float confidence{0.0f};
    bool writeOk{false};
    size_t bytesWritten{0};

    std::vector<float> waveformLeft;
    std::vector<float> waveformRight;
    int specFrames{0};
    int specBins{0};
    float specMin{0.0f};
    float specMax{1.0f};
    std::vector<float> spectrogramLog; // frame-major flattened
};

std::vector<float> DownsampleWaveform(const std::vector<float>& values, size_t targetPoints) {
    if (values.size() <= targetPoints || targetPoints < 8) {
        return values;
    }
    std::vector<float> out;
    out.reserve(targetPoints);
    const double step = static_cast<double>(values.size()) / static_cast<double>(targetPoints);
    for (size_t i = 0; i < targetPoints; ++i) {
        const size_t idx = std::min(values.size() - 1, static_cast<size_t>(i * step));
        out.push_back(values[idx]);
    }
    return out;
}

bool WriteWavPcm16(const fs::path& path, const std::vector<float>& interleaved, uint32_t sampleRate) {
    if (interleaved.empty() || (interleaved.size() % 2) != 0) {
        return false;
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }

    const uint16_t channels = 2;
    const uint16_t bitsPerSample = 16;
    const uint32_t byteRate = sampleRate * channels * (bitsPerSample / 8);
    const uint16_t blockAlign = static_cast<uint16_t>(channels * (bitsPerSample / 8));
    const uint32_t dataBytes = static_cast<uint32_t>((interleaved.size() / 2) * blockAlign);
    const uint32_t riffSize = 36u + dataBytes;

    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riffSize), sizeof(riffSize));
    out.write("WAVE", 4);

    const uint32_t fmtChunkSize = 16;
    const uint16_t audioFormat = 1;
    out.write("fmt ", 4);
    out.write(reinterpret_cast<const char*>(&fmtChunkSize), sizeof(fmtChunkSize));
    out.write(reinterpret_cast<const char*>(&audioFormat), sizeof(audioFormat));
    out.write(reinterpret_cast<const char*>(&channels), sizeof(channels));
    out.write(reinterpret_cast<const char*>(&sampleRate), sizeof(sampleRate));
    out.write(reinterpret_cast<const char*>(&byteRate), sizeof(byteRate));
    out.write(reinterpret_cast<const char*>(&blockAlign), sizeof(blockAlign));
    out.write(reinterpret_cast<const char*>(&bitsPerSample), sizeof(bitsPerSample));

    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataBytes), sizeof(dataBytes));

    for (float value : interleaved) {
        const float clamped = std::clamp(value, -1.0f, 1.0f);
        const int16_t pcm = static_cast<int16_t>(clamped * 32767.0f);
        out.write(reinterpret_cast<const char*>(&pcm), sizeof(pcm));
    }
    return out.good();
}

bool WriteFeaturesCsv(const fs::path& path, const std::vector<FeatureHistoryEntry>& rows) {
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    out << "time_sec,frame_index,total_energy,log_energy,energy_rise,spectral_flux,low_band,mid_band,high_band,hf_ratio,centroid,flatness,energy_delta,transient,left_right_balance,candidate_score,confidence\n";
    out << std::fixed << std::setprecision(6);
    for (const FeatureHistoryEntry& e : rows) {
        out << e.timeSec << ','
            << e.frameIndex << ','
            << e.features.totalEnergy << ','
            << e.features.logEnergy << ','
            << e.features.energyRise << ','
            << e.features.spectralFlux << ','
            << e.features.lowBandEnergy << ','
            << e.features.midBandEnergy << ','
            << e.features.highBandEnergy << ','
            << e.features.hfEnergyRatio << ','
            << e.features.spectralCentroid << ','
            << e.features.spectralFlatness << ','
            << e.features.energyDelta << ','
            << e.features.transientScore << ','
            << e.features.leftRightBalance << ','
            << e.candidateScore << ','
            << e.confidence << '\n';
    }
    return out.good();
}

bool WriteMetadataJson(const fs::path& path, const DatasetRecord& record) {
    std::ofstream out(path);
    if (!out) {
        return false;
    }

    out << "{\n";
    out << "  \"event_id\": \"" << JsonEscape(record.eventId) << "\",\n";
    out << "  \"event_type\": \"" << JsonEscape(record.eventType) << "\",\n";
    out << "  \"label\": \"" << JsonEscape(record.labelFolder) << "\",\n";
    out << "  \"timestamp_ms\": " << record.timestampMs << ",\n";
    out << "  \"sample_rate\": " << record.sampleRate << ",\n";
    out << "  \"fft_size\": " << record.fftSize << ",\n";
    out << "  \"hop_size\": " << record.hopSize << ",\n";
    out << "  \"window_start_sample\": " << record.windowStartSample << ",\n";
    out << "  \"window_frames\": " << (record.audioInterleaved.size() / 2) << ",\n";
    out << "  \"feature_rows\": " << record.features.size() << ",\n";
    out << "  \"detector_score\": " << record.detectorScore << ",\n";
    out << "  \"candidate_score\": " << record.candidateScore << ",\n";
    out << "  \"confidence\": " << record.confidence << ",\n";
    out << "  \"trigger_threshold\": " << record.triggerThreshold << ",\n";
    out << "  \"device_name\": \"" << JsonEscape(record.deviceName) << "\"\n";
    out << "}\n";
    return out.good();
}

size_t FolderSizeBytes(const fs::path& root) {
    size_t total = 0;
    if (!fs::exists(root)) {
        return total;
    }
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            total += static_cast<size_t>(entry.file_size());
        }
    }
    return total;
}

void EnsureDatasetFolders(const fs::path& root) {
    fs::create_directories(root / "gunshot");
    fs::create_directories(root / "footstep");
    fs::create_directories(root / "reload");
    fs::create_directories(root / "switch");
    fs::create_directories(root / "ambient");
    fs::create_directories(root / "unknown");
}

bool WriteDatasetManifest(const fs::path& root) {
    std::ofstream out(root / "manifest.csv");
    if (!out) {
        return false;
    }
    out << "label,event_id,event_type,audio_path,csv_path,json_path\n";

    const fs::path unknownRoot = root / "unknown";
    if (!fs::exists(unknownRoot)) {
        return out.good();
    }

    for (const auto& entry : fs::directory_iterator(unknownRoot)) {
        if (!entry.is_directory()) {
            continue;
        }
        const fs::path dir = entry.path();
        const std::string id = dir.filename().string();
        out << "unknown," << id << ",,"
            << (dir / "audio.wav").string() << ','
            << (dir / "features.csv").string() << ','
            << (dir / "metadata.json").string() << '\n';
    }
    return out.good();
}

CompletedEvent BuildPreviewFromRecord(const DatasetRecord& record) {
    CompletedEvent preview{};
    preview.eventId = record.eventId;
    preview.eventType = record.eventType;
    preview.timestampMs = record.timestampMs;
    preview.candidateScore = record.candidateScore;
    preview.confidence = record.confidence;

    std::vector<float> left;
    std::vector<float> right;
    left.reserve(record.audioInterleaved.size() / 2);
    right.reserve(record.audioInterleaved.size() / 2);
    for (size_t i = 0; i + 1 < record.audioInterleaved.size(); i += 2) {
        left.push_back(record.audioInterleaved[i]);
        right.push_back(record.audioInterleaved[i + 1]);
    }
    preview.waveformLeft = DownsampleWaveform(left, 1600);
    preview.waveformRight = DownsampleWaveform(right, 1600);

    STFTProcessor stft({record.fftSize, record.hopSize, record.sampleRate});
    stft.PushInterleaved(record.audioInterleaved.data(), record.audioInterleaved.size() / 2);
    STFTFrame frame{};
    std::vector<float> flattened;
    int frames = 0;
    int bins = 0;
    float vMin = 1e9f;
    float vMax = -1e9f;
    while (stft.PopFrame(frame)) {
        if (bins == 0) {
            bins = static_cast<int>(frame.left.magnitudes.size());
        }
        if (bins <= 0) {
            continue;
        }
        ++frames;
        for (int b = 0; b < bins; ++b) {
            const float logMag = std::log10(1e-6f + frame.left.magnitudes[static_cast<size_t>(b)]);
            flattened.push_back(logMag);
            vMin = std::min(vMin, logMag);
            vMax = std::max(vMax, logMag);
        }
    }
    preview.specFrames = frames;
    preview.specBins = bins;
    preview.spectrogramLog = std::move(flattened);
    preview.specMin = (vMin < vMax) ? vMin : -6.0f;
    preview.specMax = (vMin < vMax) ? vMax : 0.0f;
    return preview;
}

class AsyncDatasetWriter {
public:
    AsyncDatasetWriter() {
        m_worker = std::thread([this]() { WorkerLoop(); });
    }

    ~AsyncDatasetWriter() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_cv.notify_all();
        if (m_worker.joinable()) {
            m_worker.join();
        }
    }

    void Enqueue(DatasetRecord&& record) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_requests.push(std::move(record));
        }
        m_cv.notify_one();
    }

    bool PopCompleted(CompletedEvent& outEvent) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_completed.empty()) {
            return false;
        }
        outEvent = std::move(m_completed.front());
        m_completed.pop();
        return true;
    }

    size_t QueueSize() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_requests.size();
    }

private:
    void WorkerLoop() {
        while (true) {
            DatasetRecord record{};
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this]() { return m_stopping || !m_requests.empty(); });
                if (m_stopping && m_requests.empty()) {
                    return;
                }
                record = std::move(m_requests.front());
                m_requests.pop();
            }

            CompletedEvent result = BuildPreviewFromRecord(record);
            const fs::path eventDir = fs::path(record.datasetRoot) / record.labelFolder / record.eventId;
            fs::create_directories(eventDir);

            const fs::path audioPath = eventDir / "audio.wav";
            const fs::path csvPath = eventDir / "features.csv";
            const fs::path jsonPath = eventDir / "metadata.json";

            const bool wavOk = WriteWavPcm16(audioPath, record.audioInterleaved, record.sampleRate);
            const bool csvOk = WriteFeaturesCsv(csvPath, record.features);
            const bool jsonOk = WriteMetadataJson(jsonPath, record);

            result.writeOk = wavOk && csvOk && jsonOk;
            result.folderPath = eventDir.string();
            result.audioPath = audioPath.string();
            result.csvPath = csvPath.string();
            result.jsonPath = jsonPath.string();
            if (result.writeOk) {
                const size_t bytes = static_cast<size_t>(fs::file_size(audioPath)) +
                                     static_cast<size_t>(fs::file_size(csvPath)) +
                                     static_cast<size_t>(fs::file_size(jsonPath));
                result.bytesWritten = bytes;
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_completed.push(std::move(result));
            }
        }
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::queue<DatasetRecord> m_requests;
    std::queue<CompletedEvent> m_completed;
    std::thread m_worker;
    bool m_stopping{false};
};

struct PendingCapture {
    std::string eventType;
    double timeSec{0.0};
    int frameIndex{0};
    float candidateScore{0.0f};
    float confidence{0.0f};
    float detectorScore{0.0f};
    float triggerThreshold{0.0f};
    double readyTimeSec{0.0};
};

const char* StateToString(DetectorState state) {
    switch (state) {
    case DetectorState::Idle: return "Idle";
    case DetectorState::InCandidate: return "InCandidate";
    case DetectorState::Cooldown: return "Cooldown";
    }
    return "Unknown";
}

int ParseCommonArgs(int argc,
                    char* argv[],
                    std::string& deviceArg,
                    bool& listOnly,
                    std::string& datasetRoot) {
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
        } else if (arg == "--dataset-root") {
            if (i + 1 < argc) {
                datasetRoot = argv[++i];
            } else {
                std::fprintf(stderr, "[Error] --dataset-root requires a path argument\n");
                return 1;
            }
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "Usage: dataset_recorder [options]\n\n"
                "Options:\n"
                "  --list-devices              List available input devices\n"
                "  --device <name>             Capture from a specific device\n"
                "  --dataset-root <path>       Dataset root folder (default: dataset)\n"
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

struct SharedState {
    std::mutex mutex;
    DetectorState detectorState{DetectorState::Idle};
    float currentScore{0.0f};
    float currentConfidence{0.0f};
    float triggerThreshold{0.0f};
    float releaseThreshold{0.0f};
    double currentTimeSec{0.0};
    uint64_t totalEvents{0};
    uint64_t savedEvents{0};
    uint64_t discardedEvents{0};
    size_t diskUsageBytes{0};
    size_t writerQueueSize{0};
    bool captureRunning{false};
    bool exportOk{false};
    std::deque<CompletedEvent> recentEvents;
    int selectedRecentIndex{0};
};

struct UiSnapshot {
    DetectorState detectorState{DetectorState::Idle};
    float currentScore{0.0f};
    float currentConfidence{0.0f};
    float triggerThreshold{0.0f};
    float releaseThreshold{0.0f};
    double currentTimeSec{0.0};
    uint64_t totalEvents{0};
    uint64_t savedEvents{0};
    uint64_t discardedEvents{0};
    size_t diskUsageBytes{0};
    size_t writerQueueSize{0};
    bool captureRunning{false};
    bool exportOk{false};
    std::deque<CompletedEvent> recentEvents;
    int selectedRecentIndex{0};
};

} // namespace

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d11.h>
#include <mmsystem.h>
#include <shellapi.h>
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

void RenderWaveform(const char* label, const std::vector<float>& samples, ImVec2 size) {
    if (samples.empty()) {
        ImGui::TextDisabled("%s: no samples", label);
        return;
    }
    ImGui::PlotLines(label,
                     samples.data(),
                     static_cast<int>(samples.size()),
                     0,
                     nullptr,
                     -1.0f,
                     1.0f,
                     size);
}

ImU32 HeatColor(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const int r = static_cast<int>(255.0f * t);
    const int g = static_cast<int>(255.0f * std::sqrt(t));
    const int b = static_cast<int>(255.0f * (1.0f - t));
    return IM_COL32(r, g, b, 255);
}

void RenderSpectrogram(const CompletedEvent& ev, const ImVec2& canvasSize) {
    if (ev.specFrames <= 0 || ev.specBins <= 0 || ev.spectrogramLog.empty()) {
        ImGui::TextDisabled("Spectrogram: waiting for saved event...");
        return;
    }

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("spectrogram_canvas", canvasSize);
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    draw->AddRectFilled(p0, p1, IM_COL32(20, 20, 24, 255), 2.0f);
    draw->AddRect(p0, p1, IM_COL32(80, 80, 90, 255), 2.0f);

    const int displayBins = std::min(ev.specBins, 160);
    const float dx = (p1.x - p0.x) / static_cast<float>(std::max(1, ev.specFrames));
    const float dy = (p1.y - p0.y) / static_cast<float>(std::max(1, displayBins));
    const float denom = std::max(1e-6f, ev.specMax - ev.specMin);

    for (int x = 0; x < ev.specFrames; ++x) {
        for (int b = 0; b < displayBins; ++b) {
            const int srcBin = static_cast<int>((static_cast<double>(b) / displayBins) * ev.specBins);
            const size_t idx = static_cast<size_t>(x * ev.specBins + std::clamp(srcBin, 0, ev.specBins - 1));
            const float value = ev.spectrogramLog[idx];
            const float t = (value - ev.specMin) / denom;
            const float x0 = p0.x + dx * static_cast<float>(x);
            const float y0 = p1.y - dy * static_cast<float>(b + 1);
            draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + dx + 1.0f, y0 + dy + 1.0f), HeatColor(t));
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, OnSignal);

    std::string deviceArg;
    bool listOnly = false;
    std::string datasetRootArg = "dataset";
    const int argParseResult = ParseCommonArgs(argc, argv, deviceArg, listOnly, datasetRootArg);
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

    const fs::path datasetRoot = fs::path(datasetRootArg);
    EnsureDatasetFolders(datasetRoot);

    AudioCapture capture;
    bool started = false;
    std::string selectedDeviceName = "default";

    std::printf("[EchoRadar Dataset Recorder]\n");
    if (!deviceArg.empty()) {
        const auto found = deviceManager.FindInputDeviceByName(deviceArg);
        if (found) {
            selectedDeviceName = found->name;
            std::printf("Using device: %s\n", selectedDeviceName.c_str());
        }
        started = capture.StartDeviceByName(deviceArg);
    }
    if (!started) {
        const auto def = deviceManager.GetDefaultInputDevice();
        selectedDeviceName = def ? def->name : "default";
        std::printf("Using device: %s\n", selectedDeviceName.c_str());
        started = capture.StartDefault();
    }
    if (!started) {
        std::fprintf(stderr, "[Error] Failed to start audio capture.\n");
        return 1;
    }

    AsyncDatasetWriter writer;
    SharedState shared{};
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.captureRunning = true;
    }

    std::atomic<bool> requestAmbientSnapshot{false};
    std::atomic<uint64_t> eventCounter{0};
    const auto recordingStart = std::chrono::steady_clock::now();

    std::thread dspThread([&]() {
        STFTProcessor stft;
        FeatureExtractor extractor;
        GunshotEventDetector detector;
        AudioHistoryBuffer audioHistory(static_cast<size_t>(48000 * kHistorySeconds), 48000);
        FeatureHistoryBuffer featureHistory(kHistorySeconds);
        std::vector<float> interleaved(kReadChunkFrames * 2, 0.0f);
        std::deque<PendingCapture> pending;
        uint64_t nextSample = 0;
        uint32_t fftSize = stft.GetConfig().fft_size;
        uint32_t hopSize = stft.GetConfig().hop_size;

        while (g_running.load(std::memory_order_relaxed) && capture.IsRunning()) {
            bool didWork = false;

            size_t pulled = 0;
            do {
                pulled = capture.ReadInterleaved(interleaved.data(), kReadChunkFrames);
                if (pulled > 0) {
                    stft.PushInterleaved(interleaved.data(), pulled);
                    audioHistory.PushInterleaved(interleaved.data(), pulled, nextSample);
                    nextSample += static_cast<uint64_t>(pulled);
                    didWork = true;
                }
            } while (pulled == kReadChunkFrames);

            STFTFrame frame{};
            while (stft.PopFrame(frame)) {
                didWork = true;
                fftSize = frame.fft_size;
                hopSize = frame.hop_size;
                const AudioFeatures features = extractor.Extract(frame);
                const double frameTimeSec = static_cast<double>(frame.start_sample) /
                                            static_cast<double>(frame.sample_rate);

                detector.PushFrame(features, frameTimeSec, static_cast<int>(frame.frame_index));
                featureHistory.Push(features,
                                   frameTimeSec,
                                   static_cast<int>(frame.frame_index),
                                   detector.GetLastScore(),
                                   detector.GetLastConfidence());

                if (requestAmbientSnapshot.exchange(false, std::memory_order_acq_rel)) {
                    PendingCapture ambient{};
                    ambient.eventType = "ambient";
                    ambient.timeSec = frameTimeSec;
                    ambient.frameIndex = static_cast<int>(frame.frame_index);
                    ambient.candidateScore = detector.GetLastScore();
                    ambient.confidence = detector.GetLastConfidence();
                    ambient.detectorScore = detector.GetLastScore();
                    ambient.triggerThreshold = detector.GetTriggerThreshold();
                    ambient.readyTimeSec = frameTimeSec + kPostEventSeconds;
                    pending.push_back(ambient);
                    std::lock_guard<std::mutex> lock(shared.mutex);
                    ++shared.totalEvents;
                }

                {
                    std::lock_guard<std::mutex> lock(shared.mutex);
                    shared.detectorState = detector.GetState();
                    shared.currentScore = detector.GetLastScore();
                    shared.currentConfidence = detector.GetLastConfidence();
                    shared.triggerThreshold = detector.GetTriggerThreshold();
                    shared.releaseThreshold = detector.GetReleaseThreshold();
                    shared.currentTimeSec = frameTimeSec;
                    shared.writerQueueSize = writer.QueueSize();
                }
            }

            CandidateDecision decision{};
            while (detector.PopDecision(decision)) {
                didWork = true;
                if (decision.type != CandidateDecisionType::Peak &&
                    decision.type != CandidateDecisionType::Accepted) {
                    continue;
                }
                PendingCapture item{};
                item.eventType = (decision.type == CandidateDecisionType::Accepted) ? "gunshot" : "candidate";
                item.timeSec = decision.timeSec;
                item.frameIndex = decision.frameIndex;
                item.candidateScore = decision.candidateScore;
                item.confidence = decision.confidence;
                item.detectorScore = detector.GetLastScore();
                item.triggerThreshold = detector.GetTriggerThreshold();
                item.readyTimeSec = decision.timeSec + kPostEventSeconds;
                pending.push_back(item);

                std::lock_guard<std::mutex> lock(shared.mutex);
                ++shared.totalEvents;
            }

            GunshotEvent ev{};
            while (detector.PopEvent(ev)) {
                didWork = true;
            }

            const double latestTimeSec = static_cast<double>(nextSample) / 48000.0;
            while (!pending.empty() && pending.front().readyTimeSec <= latestTimeSec) {
                PendingCapture item = pending.front();
                pending.pop_front();

                std::vector<float> audioWindow;
                uint64_t startSample = 0;
                const bool audioOk = audioHistory.ExtractWindowByTime(item.timeSec,
                                                                      kPreEventSeconds,
                                                                      kPostEventSeconds,
                                                                      audioWindow,
                                                                      startSample);
                const auto features = featureHistory.ExtractWindow(item.timeSec - kPreEventSeconds,
                                                                   item.timeSec + kPostEventSeconds);
                if (!audioOk || features.empty()) {
                    std::lock_guard<std::mutex> lock(shared.mutex);
                    ++shared.discardedEvents;
                    continue;
                }

                const uint64_t seq = eventCounter.fetch_add(1, std::memory_order_relaxed) + 1;
                char idBuf[32];
                std::snprintf(idBuf, sizeof(idBuf), "%06llu", static_cast<unsigned long long>(seq));

                DatasetRecord record{};
                record.eventId = idBuf;
                record.eventType = item.eventType;
                record.datasetRoot = datasetRoot.string();
                record.labelFolder = "unknown";
                record.deviceName = selectedDeviceName;
                record.timestampMs = static_cast<uint64_t>(std::llround(item.timeSec * 1000.0));
                record.sampleRate = 48000;
                record.fftSize = fftSize;
                record.hopSize = hopSize;
                record.detectorScore = item.detectorScore;
                record.candidateScore = item.candidateScore;
                record.confidence = item.confidence;
                record.triggerThreshold = item.triggerThreshold;
                record.windowStartSample = startSample;
                record.audioInterleaved = std::move(audioWindow);
                record.features = features;
                writer.Enqueue(std::move(record));
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
    wc.lpszClassName = L"EchoRadarDatasetRecorderClass";
    ::RegisterClassExW(&wc);

    HWND hwnd = ::CreateWindowW(wc.lpszClassName,
                                L"EchoRadar - Dataset Recorder",
                                WS_OVERLAPPEDWINDOW,
                                100,
                                100,
                                1420,
                                960,
                                nullptr,
                                nullptr,
                                wc.hInstance,
                                nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        g_running.store(false, std::memory_order_relaxed);
        if (dspThread.joinable()) {
            dspThread.join();
        }
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

    auto lastDiskRefresh = std::chrono::steady_clock::now();
    float displayWindowSec = 1.0f;
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

        CompletedEvent completed{};
        while (writer.PopCompleted(completed)) {
            std::lock_guard<std::mutex> lock(shared.mutex);
            if (completed.writeOk) {
                ++shared.savedEvents;
            } else {
                ++shared.discardedEvents;
            }
            shared.recentEvents.push_front(std::move(completed));
            while (shared.recentEvents.size() > static_cast<size_t>(kRecentEventsMax)) {
                shared.recentEvents.pop_back();
            }
            shared.selectedRecentIndex = std::clamp(shared.selectedRecentIndex, 0, static_cast<int>(shared.recentEvents.size()) - 1);
        }

        const auto now = std::chrono::steady_clock::now();
        if ((now - lastDiskRefresh) > std::chrono::seconds(1)) {
            std::lock_guard<std::mutex> lock(shared.mutex);
            shared.diskUsageBytes = FolderSizeBytes(datasetRoot);
            lastDiskRefresh = now;
        }

        UiSnapshot snapshot{};
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            snapshot.detectorState = shared.detectorState;
            snapshot.currentScore = shared.currentScore;
            snapshot.currentConfidence = shared.currentConfidence;
            snapshot.triggerThreshold = shared.triggerThreshold;
            snapshot.releaseThreshold = shared.releaseThreshold;
            snapshot.currentTimeSec = shared.currentTimeSec;
            snapshot.totalEvents = shared.totalEvents;
            snapshot.savedEvents = shared.savedEvents;
            snapshot.discardedEvents = shared.discardedEvents;
            snapshot.diskUsageBytes = shared.diskUsageBytes;
            snapshot.writerQueueSize = shared.writerQueueSize;
            snapshot.captureRunning = shared.captureRunning;
            snapshot.exportOk = shared.exportOk;
            snapshot.recentEvents = shared.recentEvents;
            snapshot.selectedRecentIndex = shared.selectedRecentIndex;
        }

        const double recordingSec = std::chrono::duration<double>(now - recordingStart).count();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowSize(ImVec2(1360, 900), ImGuiCond_FirstUseEver);
        ImGui::Begin("Dataset Recorder");

        ImGui::SeparatorText("Runtime");
        ImGui::Text("Capture: %s", snapshot.captureRunning ? "Running" : "Stopped");
        ImGui::Text("Detector State: %s", StateToString(snapshot.detectorState));
        ImGui::Text("Current Score: %.3f", snapshot.currentScore);
        ImGui::Text("Current Confidence: %.3f", snapshot.currentConfidence);
        ImGui::Text("Threshold: trigger=%.3f release=%.3f", snapshot.triggerThreshold, snapshot.releaseThreshold);
        ImGui::Text("Recording Time: %.1f s", recordingSec);

        ImGui::SeparatorText("Statistics");
        ImGui::Text("Total Events: %llu", static_cast<unsigned long long>(snapshot.totalEvents));
        ImGui::Text("Saved Events: %llu", static_cast<unsigned long long>(snapshot.savedEvents));
        ImGui::Text("Discarded Events: %llu", static_cast<unsigned long long>(snapshot.discardedEvents));
        ImGui::Text("Writer Queue: %zu", snapshot.writerQueueSize);
        ImGui::Text("Disk Usage: %.2f MB", static_cast<double>(snapshot.diskUsageBytes) / (1024.0 * 1024.0));
        ImGui::Text("Dataset Root: %s", datasetRoot.string().c_str());

        if (ImGui::Button("Save Ambient Snapshot (200ms pre/post)")) {
            requestAmbientSnapshot.store(true, std::memory_order_release);
        }
        ImGui::SameLine();
        if (ImGui::Button("Export Manifest")) {
            const bool ok = WriteDatasetManifest(datasetRoot);
            std::lock_guard<std::mutex> lock(shared.mutex);
            shared.exportOk = ok;
        }
        ImGui::SameLine();
        if (ImGui::Button("Open Dataset Folder")) {
            ShellExecuteA(nullptr, "open", datasetRoot.string().c_str(), nullptr, nullptr, SW_SHOWDEFAULT);
        }
        ImGui::SameLine();
        ImGui::TextColored(snapshot.exportOk ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                           snapshot.exportOk ? "Manifest exported" : "Manifest not exported yet");

        ImGui::SeparatorText("Recent Events");
        std::vector<const char*> labels;
        labels.reserve(snapshot.recentEvents.size());
        std::vector<std::string> ownedLabels;
        ownedLabels.reserve(snapshot.recentEvents.size());
        for (const auto& ev : snapshot.recentEvents) {
            std::ostringstream oss;
            oss << ev.eventId << " [" << ev.eventType << "]"
                << " score=" << std::fixed << std::setprecision(2) << ev.candidateScore
                << " conf=" << std::fixed << std::setprecision(2) << ev.confidence;
            ownedLabels.push_back(oss.str());
        }
        for (const auto& s : ownedLabels) {
            labels.push_back(s.c_str());
        }

        int selected = snapshot.selectedRecentIndex;
        if (!labels.empty()) {
            selected = std::clamp(selected, 0, static_cast<int>(labels.size()) - 1);
            ImGui::ListBox("Events", &selected, labels.data(), static_cast<int>(labels.size()), 8);
            {
                std::lock_guard<std::mutex> lock(shared.mutex);
                shared.selectedRecentIndex = selected;
            }
        } else {
            ImGui::TextDisabled("No saved events yet...");
        }

        CompletedEvent selectedEvent{};
        bool hasSelected = false;
        if (!snapshot.recentEvents.empty()) {
            const int idx = std::clamp(selected, 0, static_cast<int>(snapshot.recentEvents.size()) - 1);
            selectedEvent = snapshot.recentEvents[static_cast<size_t>(idx)];
            hasSelected = true;
        }

        if (hasSelected) {
            if (ImGui::Button("Replay Selected")) {
                PlaySoundA(selectedEvent.audioPath.c_str(), nullptr, SND_FILENAME | SND_ASYNC);
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop Replay")) {
                PlaySoundA(nullptr, nullptr, 0);
            }
            ImGui::SameLine();
            if (ImGui::Button("Open Event Folder")) {
                ShellExecuteA(nullptr, "open", selectedEvent.folderPath.c_str(), nullptr, nullptr, SW_SHOWDEFAULT);
            }

            ImGui::Text("Selected Event: %s", selectedEvent.eventId.c_str());
            ImGui::Text("Type: %s", selectedEvent.eventType.c_str());
            ImGui::Text("Score=%.3f  Confidence=%.3f", selectedEvent.candidateScore, selectedEvent.confidence);
            ImGui::Text("Write Status: %s  Bytes=%zu",
                        selectedEvent.writeOk ? "OK" : "FAILED",
                        selectedEvent.bytesWritten);

            ImGui::SetNextItemWidth(260.0f);
            ImGui::SliderFloat("Display Window (s)", &displayWindowSec, kMinTimeWindowSec, kMaxTimeWindowSec, "%.2f");

            ImGui::SeparatorText("Waveform (400 ms window)");
            RenderWaveform("Left", selectedEvent.waveformLeft, ImVec2(-1.0f, 120.0f));
            RenderWaveform("Right", selectedEvent.waveformRight, ImVec2(-1.0f, 120.0f));

            ImGui::SeparatorText("Spectrogram");
            RenderSpectrogram(selectedEvent, ImVec2(-1.0f, 240.0f));
        }

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
    std::string datasetRoot = "dataset";
    const int argParseResult = ParseCommonArgs(argc, argv, deviceArg, listOnly, datasetRoot);
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
                 "dataset_recorder GUI requires Windows (Dear ImGui + DirectX 11 backend).\n");
    return 1;
}

#endif
