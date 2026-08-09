#pragma once
#include "../common/Types.h"
#include "../recognition/V4RecognitionTypes.h"
#include "../recognition/V4RuntimeConfig.h"
#include "../localization/CalibrationController.h"
#include "../localization/LocalizationTypes.h"
#include "../settings/AppSettings.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace EchoRadar {

/// Renders the EchoRadar V4 event chart and runtime tune table using Dear ImGui
/// + DirectX 11 in a regular interactive Windows window.
///
/// Only compiled on Windows (_WIN32).
class OverlayRenderer {
public:
    struct Config {
        int   window_width{1280};
        int   window_height{820};
        float radar_radius{150.0f};
        float opacity{0.98f};
        uint32_t sample_rate{48000};
        uint32_t peak_lookahead_frames{0};
        std::string model_version;
        std::string recognition_error;
        std::shared_ptr<V4RuntimeTuningStore> v4_tuning;
        std::shared_ptr<RuntimeSettingsStore> runtime_settings;
        std::shared_ptr<CalibrationController> calibration;
    };

    OverlayRenderer();
    explicit OverlayRenderer(Config cfg);
    ~OverlayRenderer();

    OverlayRenderer(const OverlayRenderer&)            = delete;
    OverlayRenderer& operator=(const OverlayRenderer&) = delete;

    /// Initialise the event chart window and DX11 device.
    bool Initialise();
    void Shutdown();

    /// Push a new set of events to display (called from processing thread).
    void PushFootstep(const FootstepEvent&     ev, const DirectionEstimate& dir);
    void PushGunshot (const GunshotEvent&      ev, const DirectionEstimate& dir);
    void PushV4Event(const V4SoundEvent& event);
    void PushLocalizedEvent(const V4SoundEvent& event,
                            const DirectionResult& direction);
    void PushAudioClock(uint64_t sample, uint64_t streamGeneration,
                        bool discontinuity = false);
    void PushAudioLevels(const AudioLevels& levels);
    void PushV4Scores(const V4ModelOutput& output, float sceneActivity,
                      bool hasOutput);

    /// Main render tick – call from the main/UI thread.
    void Render();

    bool IsRunning() const { return m_running; }

    // Platform details stay opaque to callers but are public so the Win32
    // window procedure can safely reference the forward-declared type.
    struct PlatformImpl;

private:
    Config m_cfg;
    bool   m_running{false};

    struct ActiveMarker {
        DirectionEstimate dir;
        bool              is_gunshot{false};
        float             ttl{2.0f};  // seconds remaining
    };
    // Platform handles – opaque here; defined in .cpp
    std::unique_ptr<PlatformImpl> m_platform;

    mutable std::mutex m_dataMutex;
    std::deque<V4SoundEvent> m_v4Events;
    struct LocalizedRecord {
        V4SoundEvent event;
        DirectionResult direction;
    };
    std::deque<LocalizedRecord> m_localizedEvents;
    uint64_t m_currentSample{0};
    uint64_t m_streamGeneration{0};
    AudioLevels m_audioLevels{};
    V4ModelOutput m_v4Scores{};
    float m_sceneActivity{0.0f};
    bool m_haveV4Scores{false};
    float m_chartWindowSeconds{30.0f};
    std::vector<ActiveMarker> m_markers;

    void DrawUi();
    void DrawLiveDiagnostics(const AudioLevels& levels,
                             const V4ModelOutput& scores,
                             float sceneActivity, bool haveScores);
    void DrawEventTimeline(const std::deque<V4SoundEvent>& events,
                           uint64_t currentSample, uint64_t streamGeneration);
    void DrawRecentEvents(const std::deque<V4SoundEvent>& events,
                          uint64_t streamGeneration);
    void DrawTuneTable();
    void DrawDirectionPage(const std::deque<LocalizedRecord>& events);
    void DrawCalibrationPage();
    void DrawOverlayPage();
    void DrawAudioSystemPage();
};

} // namespace EchoRadar
