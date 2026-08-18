#pragma once

#include <audio/AudioCapture.h>
#include <audio/AudioHistoryBuffer.h>
#include <direction/DirectionModelPackage.h>
#include <direction/DirectionOnnxEngine.h>
#include <direction/DirectionSceneCoordinator.h>
#include <overlay/HudOverlayRenderer.h>
#include <overlay/OverlayRenderer.h>
#include <recognition/OnnxRecognitionModel.h>
#include <recognition/SoundRecognizer.h>
#include <settings/AppSettings.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace EchoRadar {

class EchoRadarApp {
public:
    struct Config {
        AudioCaptureConfig audio;
        std::filesystem::path modelDirectory{"models/recognition-candidate"};
        std::filesystem::path directionModelDirectory;
        std::filesystem::path settingsPath;
        bool showOverlay{true};
    };

    EchoRadarApp();
    explicit EchoRadarApp(Config config);
    ~EchoRadarApp();

    EchoRadarApp(const EchoRadarApp&) = delete;
    EchoRadarApp& operator=(const EchoRadarApp&) = delete;

    bool Initialise();
    void Run();
    void Stop();

private:
    Config m_config;

    std::unique_ptr<AudioCapture> m_audio;
    std::shared_ptr<OnnxRecognitionModel> m_model;
    std::unique_ptr<SoundRecognizer> m_recognizer;
    std::unique_ptr<OverlayRenderer> m_overlay;
    std::unique_ptr<HudOverlayRenderer> m_hud;
    std::shared_ptr<RecognitionRuntimeTuningStore> m_runtimeTuning;
    std::shared_ptr<RuntimeSettingsStore> m_settings;
    AudioHistoryBuffer m_audioHistory{48000 * 3, 48000};
    std::unique_ptr<DirectionOnnxEngine> m_directionEngine;
    DirectionSceneCoordinator m_sceneCoordinator;

    uint64_t m_nextEventId{1};
    std::ofstream m_sessionLog;
    std::filesystem::path m_clipDirectory;
    std::string m_modelVersion;
    std::string m_directionModelVersion;
    std::string m_directionError;
    uint32_t m_peakLookaheadFrames{0};
    std::string m_recognitionError;

    std::atomic<bool> m_stop{false};
    std::thread m_dspThread;

    void DSPLoop();
    void HandleEvent(const SoundEvent& event);
    void ProcessPendingDirectionScenes();
    std::filesystem::path SaveSceneClip(const DirectionSceneJob& scene,
                                        const std::vector<float>& clip);
    void LogDirectionEvent(const DirectionSceneEvent& member,
                           const DirectionSceneResult& scene,
                           const std::filesystem::path& clipPath);
};

} // namespace EchoRadar
