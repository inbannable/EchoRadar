#pragma once
#include "../audio/AudioCapture.h"
#include "../overlay/OverlayRenderer.h"
#include "../recognition/V4OnnxModel.h"
#include "../recognition/V4Recognizer.h"
#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>

namespace EchoRadar {

/// Top-level application class.
/// Wires all subsystems together and drives the processing loop.
class EchoRadarApp {
public:
    struct Config {
        AudioCaptureConfig audio;
        std::filesystem::path modelDirectory{"models/v4-candidate"};
        bool        show_overlay{true};
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
    std::shared_ptr<V4RuntimeTuningStore> m_runtimeTuning;
    std::string m_modelVersion;
    uint32_t m_peakLookaheadFrames{0};
    std::string m_recognitionError;

    std::atomic<bool> m_stop{false};
    std::thread       m_dsp_thread;

    void DSPLoop();
    void HandleEvent(const V4SoundEvent& event);
};

} // namespace EchoRadar
