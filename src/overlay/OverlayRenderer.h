#pragma once

#include <audio/AudioTypes.h>
#include <direction/DirectionSceneCoordinator.h>
#include <direction/DirectionTypes.h>
#include <recognition/RecognitionRuntimeConfig.h>
#include <recognition/RecognitionTypes.h>
#include <settings/AppSettings.h>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace EchoRadar {

class OverlayRenderer {
public:
    struct Config {
        int windowWidth{1280};
        int windowHeight{820};
        uint32_t sampleRate{48000};
        uint32_t peakLookaheadFrames{0};
        std::string modelVersion;
        std::string recognitionError;
        std::string directionModelVersion;
        std::string directionError;
        std::shared_ptr<RecognitionRuntimeTuningStore> runtimeTuning;
        std::shared_ptr<RuntimeSettingsStore> runtimeSettings;
    };

    OverlayRenderer();
    explicit OverlayRenderer(Config config);
    ~OverlayRenderer();

    OverlayRenderer(const OverlayRenderer&) = delete;
    OverlayRenderer& operator=(const OverlayRenderer&) = delete;

    bool Initialise();
    void Shutdown();

    void PushRecognitionEvent(const SoundEvent& event);
    void PushDirectionScene(std::vector<DirectionSceneEvent> events,
                            const DirectionSceneResult& direction,
                            std::filesystem::path clipPath = {});
    void PushAudioClock(uint64_t sample, uint64_t streamGeneration,
                        bool discontinuity = false);
    void PushAudioLevels(const AudioLevels& levels);
    void PushRecognitionScores(const RecognitionModelOutput& output,
                               float sceneActivity, bool hasOutput);
    void Render();

    bool IsRunning() const { return m_running; }

    struct PlatformImpl;

private:
    struct DirectionSceneRecord {
        std::vector<DirectionSceneEvent> events;
        DirectionSceneResult direction;
        std::filesystem::path clipPath;
    };

    Config m_config;
    bool m_running{false};
    std::unique_ptr<PlatformImpl> m_platform;

    mutable std::mutex m_dataMutex;
    std::deque<SoundEvent> m_recognitionEvents;
    std::deque<DirectionSceneRecord> m_directionScenes;
    uint64_t m_currentSample{0};
    uint64_t m_streamGeneration{0};
    AudioLevels m_audioLevels{};
    RecognitionModelOutput m_recognitionScores{};
    float m_sceneActivity{0.0f};
    bool m_haveRecognitionScores{false};
    float m_chartWindowSeconds{30.0f};
    float m_appliedUiScale{1.0f};
    std::filesystem::path m_playingClipPath;
    std::string m_clipPlaybackError;

    void DrawUi();
    void ApplyUiScale(float scale);
    void DrawLiveDiagnostics(const AudioLevels& levels,
                             const RecognitionModelOutput& scores,
                             float sceneActivity, bool haveScores);
    void DrawEventTimeline(const std::deque<SoundEvent>& events,
                           uint64_t currentSample,
                           uint64_t streamGeneration);
    void DrawRecentEvents(const std::deque<SoundEvent>& events,
                          uint64_t streamGeneration);
    void DrawTuneTable();
    void DrawDirectionPage(const std::deque<DirectionSceneRecord>& scenes);
    void DrawOverlayPage();
    void DrawAudioSystemPage();

#ifdef _WIN32
    void PlayClip(const std::filesystem::path& path);
    void StopClip();
#endif
};

} // namespace EchoRadar
