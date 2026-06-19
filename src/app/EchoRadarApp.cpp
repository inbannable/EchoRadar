#include "EchoRadarApp.h"
#include <iostream>
#include <thread>
#include <chrono>

namespace EchoRadar {

EchoRadarApp::EchoRadarApp(Config cfg) : m_cfg(std::move(cfg)) {}

EchoRadarApp::~EchoRadarApp() {
    Stop();
    if (m_dsp_thread.joinable()) m_dsp_thread.join();
}

bool EchoRadarApp::Initialise() {
    std::cout << "[EchoRadar] Initialising subsystems...\n";

    m_ring      = std::make_unique<RingBuffer>(m_cfg.ring_buffer_capacity);
    m_stft      = std::make_unique<STFTProcessor>();
    m_gunshot   = std::make_unique<GunshotDetector>();
    m_footstep  = std::make_unique<FootstepDetector>();
    m_features  = std::make_unique<FeatureExtractor>();
    m_estimator = std::make_unique<KNNDirectionEstimator>();
    m_tracker   = std::make_unique<DirectionTracker>();

    m_audio = std::make_unique<AudioCapture>();
    bool ok = m_audio->Start(m_cfg.audio_device_name, [this](const AudioFrame& f) {
        m_ring->Push(f);
    });
    if (!ok) {
        std::cerr << "[EchoRadar] Failed to start audio capture.\n";
        return false;
    }

    if (m_cfg.show_overlay) {
        m_overlay = std::make_unique<OverlayRenderer>();
        m_overlay->Initialise(); // non-fatal if it fails on non-Windows
    }

    std::cout << "[EchoRadar] Ready.\n";
    return true;
}

void EchoRadarApp::Run() {
    m_stop = false;
    m_dsp_thread = std::thread(&EchoRadarApp::DSPLoop, this);

    // UI / overlay loop on main thread
    while (!m_stop) {
        if (m_overlay && m_overlay->IsRunning()) {
            m_overlay->Render();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 fps
    }

    if (m_dsp_thread.joinable()) m_dsp_thread.join();
}

void EchoRadarApp::Stop() {
    m_stop = true;
    if (m_audio) m_audio->Stop();
    if (m_overlay) m_overlay->Shutdown();
}

void EchoRadarApp::DSPLoop() {
    while (!m_stop) {
        AudioFrame frame;
        if (!m_ring->Pop(frame)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        auto [spec_l, spec_r] = m_stft->ProcessStereo(frame);

        m_gunshot ->Process(spec_l, [&](const GunshotEvent& ev) {
            auto dir = m_tracker->Update(m_estimator->Estimate(
                m_features->Extract(spec_l, spec_r, ev.timestamp)));
            if (m_overlay) m_overlay->PushGunshot(ev, dir);
        });

        m_footstep->Process(spec_l, [&](const FootstepEvent& ev) {
            auto dir = m_tracker->Update(m_estimator->Estimate(
                m_features->Extract(spec_l, spec_r, ev.timestamp)));
            if (m_overlay) m_overlay->PushFootstep(ev, dir);
        });
    }
}

} // namespace EchoRadar
