#pragma once
#include "../audio/AudioCapture.h"
#include "../audio/AudioHistoryBuffer.h"
#include "../localization/CalibrationController.h"
#include "../localization/DirectionModelPackage.h"
#include "../localization/DirectionOnnxEngine.h"
#include "../localization/DirectionSceneCoordinator.h"
#include "../localization/StereoDirectionEstimator.h"
#include "../overlay/HudOverlayRenderer.h"
#include "../overlay/OverlayRenderer.h"
#include "../recognition/V4OnnxModel.h"
#include "../recognition/V4Recognizer.h"
#include "../settings/AppSettings.h"
#include <atomic>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

namespace EchoRadar {

/// Top-level application class.
/// Wires all subsystems together and drives the processing loop.
class EchoRadarApp {
public:
    struct Config {
        AudioCaptureConfig audio;
        std::filesystem::path modelDirectory{"models/v4-candidate"};
        std::filesystem::path directionModelDirectory;
        std::filesystem::path settingsPath;
        bool        show_overlay{true};
        bool        legacyDirectionDiagnostic{false};
    };

    EchoRadarApp();
    explicit EchoRadarApp(Config cfg);
    ~EchoRadarApp();

    EchoRadarApp(const EchoRadarApp&)            = delete;
    EchoRadarApp& operator=(const EchoRadarApp&) = delete;

    /// Initialise all subsystems. Returns false on fatal error.
    bool Initialise();

    /// Run the application (blocks until Stop() is called or fatal error).
    void Run();

    /// Signal the application to exit cleanly.
    void Stop();

private:
    Config m_cfg;

    // Subsystems
    std::unique_ptr<AudioCapture> m_audio;
    std::shared_ptr<V4OnnxModel> m_model;
    std::unique_ptr<V4Recognizer> m_recognizer;
    std::unique_ptr<OverlayRenderer> m_overlay;
    std::unique_ptr<HudOverlayRenderer> m_hud;
    std::shared_ptr<V4RuntimeTuningStore> m_runtimeTuning;
    std::shared_ptr<RuntimeSettingsStore> m_settings;
    std::shared_ptr<CalibrationController> m_calibration;
    AudioHistoryBuffer m_audioHistory{48000 * 3, 48000};
    std::unique_ptr<DirectionOnnxEngine> m_directionEngine;
    DirectionSceneCoordinator m_sceneCoordinator;
    StereoDirectionEstimator m_directionEstimator;
    struct PendingLocalization {
        uint64_t eventId{0};
        V4SoundEvent event;
        bool enabled{true};
    };
    std::deque<PendingLocalization> m_pendingLocalizations;
    uint64_t m_nextEventId{1};
    std::ofstream m_sessionLog;
    std::filesystem::path m_clipDirectory;
    std::string m_clipSessionTag;
    std::string m_modelVersion;
    std::string m_directionModelVersion;
    std::string m_directionError;
    uint32_t m_peakLookaheadFrames{0};
    std::string m_recognitionError;

    std::atomic<bool> m_stop{false};
    std::thread       m_dsp_thread;

    void DSPLoop();
    void HandleEvent(const V4SoundEvent& event);
    void ProcessPendingLocalizations();
    std::filesystem::path SaveEventClip(uint64_t eventId,
                                         const V4SoundEvent& event,
                                         const std::vector<float>& clip);
    std::filesystem::path SaveSceneClip(const DirectionSceneJob& scene,
                                        const std::vector<float>& clip);
    void LogLocalizedEvent(const V4SoundEvent& event,
                           const DirectionResult& direction,
                           const std::filesystem::path& clipPath,
                           const DirectionSceneResult* scene = nullptr);
};

} // namespace EchoRadar
