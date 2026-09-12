#pragma once

#include "SessionLog.h"

#include <audio/AudioCapture.h>
#include <audio/AudioDeviceManager.h>
#include <audio/AudioHistoryBuffer.h>
#include <audio/Pcm16WavWriter.h>
#include <overlay/HudOverlayRenderer.h>
#include <overlay/OverlayRenderer.h>
#include <radar/RadarProcessor.h>
#include <recognition/OnnxRecognitionModel.h>
#include <recognition/SoundRecognizer.h>
#include <settings/AppSettings.h>
#include <ui/AppState.h>

#include <atomic>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace EchoRadar {

class EchoRadarApp {
public:
    struct Config {
        AudioCaptureConfig audio;
        std::filesystem::path modelDirectory;
        std::filesystem::path settingsPath;
        std::optional<RadarMode> radarMode;
        std::optional<RadarPreset> radarPreset;
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
    struct PendingRadarEvent {
        uint64_t eventId{0};
        SoundEvent event{};
    };

    Config m_config;
    std::unique_ptr<AudioCapture> m_audio;
    std::unique_ptr<AudioDeviceManager> m_deviceManager;
    std::shared_ptr<OnnxRecognitionModel> m_model;
    std::unique_ptr<SoundRecognizer> m_recognizer;
    std::shared_ptr<RecognitionRuntimeTuningStore> m_runtimeTuning;
    std::shared_ptr<RuntimeSettingsStore> m_settings;
    std::unique_ptr<DebouncedSettingsSaver> m_settingsSaver;
    std::unique_ptr<RadarProcessor> m_radar;
    std::unique_ptr<AudioHistoryBuffer> m_surroundHistory;
    std::unique_ptr<OverlayRenderer> m_overlay;
    std::unique_ptr<HudOverlayRenderer> m_hud;
    std::shared_ptr<LatestSnapshotPublisher> m_snapshots;
    std::shared_ptr<UiCommandQueue> m_commands;

    SessionLogWriter m_sessionLog;
    std::string m_sessionLoggingError;
    std::filesystem::path m_sessionDirectory;
    std::filesystem::path m_recordingPath;
    std::unique_ptr<Pcm16WavWriter> m_recordingWriter;
    bool m_recording{false};

    std::deque<PendingRadarEvent> m_pendingEvents;
    std::vector<RecentEventSnapshot> m_recentEvents;
    RadarFrame m_latestRadarFrame{};
    SignalActivity m_signalActivity;
    bool m_audioFresh{false};
    AudioChannelLayout m_currentLayout{};
    uint64_t m_currentGeneration{0};
    uint64_t m_currentAudioSample{0};
    uint64_t m_nextEventId{1};
    uint64_t m_recordingSequence{0};
    uint64_t m_snapshotRevision{0};
    AudioCaptureState m_lastLoggedCaptureState{AudioCaptureState::Stopped};
    uint64_t m_lastLoggedGeneration{0};
    std::string m_activeHudDisplayId{"default"};
    HudRect m_activeHudUsableBounds{0.0f, 0.0f, 1920.0f, 1080.0f};
    float m_activeHudDpiScale{1.0f};

    ModelUiState m_modelState{ModelUiState::Disabled};
    std::string m_modelVersion;
    std::string m_modelStatus;
    std::string m_recognitionError;
    uint32_t m_peakLookaheadFrames{0};

    std::atomic<bool> m_stop{false};
    std::thread m_dspThread;

    void DSPLoop();
    void HandleEvent(const SoundEvent& event);
    void ProcessPendingRadarEvents();
    void FinalizePendingRadarEvents(RadarRuntimeStatus status);
    void ProcessUiCommands();
    void PublishSnapshot(const AudioCaptureStatus& status,
                         const AudioLevels& levels);
    void ResetPipelines(const AudioReadResult& read);
    void ApplyRadarSettings(const AppSettings& settings);
    void CommitSettings(const AppSettings& settings);
    void StopSettingsSaver();
    void ClearLatestRadarFrame(RadarRuntimeStatus status);
    void RestartCapture(std::optional<std::string> endpointId = std::nullopt);
    void BeginRecording(const AudioChannelLayout& layout);
    void AppendRecording(std::span<const float> samples, size_t frameCount,
                         const AudioChannelLayout& layout);
    void FlushRecording();
    void RefreshDevices();
};

} // namespace EchoRadar
