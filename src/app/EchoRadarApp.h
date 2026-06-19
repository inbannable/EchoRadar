#pragma once
#include "../audio/AudioCapture.h"
#include "../dsp/RingBuffer.h"
#include "../dsp/STFTProcessor.h"
#include "../events/GunshotDetector.h"
#include "../events/FootstepDetector.h"
#include "../features/FeatureExtractor.h"
#include "../localization/KNNDirectionEstimator.h"
#include "../tracking/DirectionTracker.h"
#include "../overlay/OverlayRenderer.h"
#include <atomic>
#include <memory>
#include <thread>

namespace EchoRadar {

/// Top-level application class.
/// Wires all subsystems together and drives the processing loop.
class EchoRadarApp {
public:
    struct Config {
        std::string audio_device_name;          // empty = default device
        std::string knn_model_path{"model.bin"};
        bool        show_overlay{true};
        uint32_t    ring_buffer_capacity{512};
    };

    explicit EchoRadarApp(Config cfg = {});
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
    std::unique_ptr<AudioCapture>          m_audio;
    std::unique_ptr<RingBuffer>            m_ring;
    std::unique_ptr<STFTProcessor>         m_stft;
    std::unique_ptr<GunshotDetector>       m_gunshot;
    std::unique_ptr<FootstepDetector>      m_footstep;
    std::unique_ptr<FeatureExtractor>      m_features;
    std::unique_ptr<KNNDirectionEstimator> m_estimator;
    std::unique_ptr<DirectionTracker>      m_tracker;
    std::unique_ptr<OverlayRenderer>       m_overlay;

    std::atomic<bool> m_stop{false};
    std::thread       m_dsp_thread;

    void DSPLoop();
};

} // namespace EchoRadar
