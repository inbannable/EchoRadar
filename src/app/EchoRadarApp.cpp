#include "EchoRadarApp.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

namespace EchoRadar {

EchoRadarApp::EchoRadarApp() : EchoRadarApp(Config{}) {}

EchoRadarApp::EchoRadarApp(Config config) : m_cfg(std::move(config)) {}

EchoRadarApp::~EchoRadarApp() {
    Stop();
    if (m_dsp_thread.joinable()) m_dsp_thread.join();
    if (m_audio) m_audio->Stop();
    if (m_overlay) m_overlay->Shutdown();
}

bool EchoRadarApp::Initialise() {
    std::cout << "[EchoRadar] Starting built-in audio capture...\n";
    m_audio = std::make_unique<AudioCapture>();
    if (!m_audio->Start(m_cfg.audio)) {
        const auto status = m_audio->GetStatus();
        std::cerr << "[EchoRadar] Audio capture could not start: " << status.lastError << '\n';
        return false;
    }

    V4ModelPackage package;
    if (!V4ModelPackage::Load(m_cfg.modelDirectory, package, &m_recognitionError)) {
        std::cerr << "[EchoRadar] V4 recognition paused: " << m_recognitionError << '\n';
    } else {
        m_modelVersion = package.modelVersion;
        m_peakLookaheadFrames = package.peakLookaheadFrames;
        m_runtimeTuning = std::make_shared<V4RuntimeTuningStore>(
            V4RuntimeTuning::FromPackage(package));
        m_model = std::make_shared<V4OnnxModel>(
            package.modelPath, package.contextFrames, package.melBins, package.inputChannels);
        if (!m_model->IsLoaded()) {
            m_recognitionError = m_model->LoadError();
            std::cerr << "[EchoRadar] V4 recognition paused: " << m_recognitionError << '\n';
        } else {
            const size_t planeSize = static_cast<size_t>(package.contextFrames) * package.melBins;
            std::vector<float> validationInput(
                static_cast<size_t>(package.inputChannels) * planeSize, 0.0f);
            std::fill(validationInput.begin() + static_cast<std::ptrdiff_t>(planeSize),
                      validationInput.begin() + static_cast<std::ptrdiff_t>(2 * planeSize),
                      -100.0f);
            V4ModelOutput validationOutput;
            if (!m_model->Predict(validationInput, validationOutput, &m_recognitionError)) {
                std::cerr << "[EchoRadar] V4 recognition paused: model contract check failed: "
                          << m_recognitionError << '\n';
            } else {
                m_recognizer = std::make_unique<V4Recognizer>(
                    m_model, package,
                    [this](const V4SoundEvent& event) { HandleEvent(event); },
                    m_runtimeTuning);
                std::cout << "[EchoRadar] Experimental V4 model loaded: "
                          << package.modelVersion << '\n';
            }
        }
    }

    if (m_cfg.show_overlay) {
        OverlayRenderer::Config overlayConfig;
        overlayConfig.sample_rate = 48000;
        overlayConfig.peak_lookahead_frames = m_peakLookaheadFrames;
        overlayConfig.model_version = m_modelVersion;
        overlayConfig.recognition_error = m_recognitionError;
        overlayConfig.v4_tuning = m_runtimeTuning;
        m_overlay = std::make_unique<OverlayRenderer>(std::move(overlayConfig));
        if (!m_overlay->Initialise()) {
            std::cerr << "[EchoRadar] Event chart UI could not be initialized; continuing headless.\n";
            m_overlay.reset();
        }
    }
    const auto status = m_audio->GetStatus();
    if (status.state == AudioCaptureState::Running) {
        std::cout << "[EchoRadar] Capturing system output: " << status.endpointName << '\n';
    } else {
        std::cout << "[EchoRadar] Waiting for a Windows output endpoint: "
                  << status.lastError << '\n';
    }
    return true;
}

void EchoRadarApp::Run() {
    m_dsp_thread = std::thread(&EchoRadarApp::DSPLoop, this);
    while (!m_stop.load(std::memory_order_acquire)) {
        if (m_overlay && m_overlay->IsRunning()) m_overlay->Render();
        if (m_overlay && !m_overlay->IsRunning()) {
            Stop();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    if (m_dsp_thread.joinable()) m_dsp_thread.join();
    if (m_overlay) m_overlay->Shutdown();
}

void EchoRadarApp::Stop() {
    m_stop.store(true, std::memory_order_release);
}

void EchoRadarApp::HandleEvent(const V4SoundEvent& event) {
    if (m_overlay) m_overlay->PushV4Event(event);
    std::printf(
        "\n[V4 %s] source=%s scene=%s confidence=%.3f onset=%.3fs detected=%.3fs "
        "delivered=%.3fs stream=%llu\n",
        ToString(event.soundClass), ToString(event.sourceHint), ToString(event.sceneState),
        event.confidence, event.onsetSample / 48000.0, event.detectedSample / 48000.0,
        event.deliveredSample / 48000.0,
        static_cast<unsigned long long>(event.streamGeneration));
    std::fflush(stdout);
}

void EchoRadarApp::DSPLoop() {
    constexpr size_t kChunkFrames = 480;
    std::vector<float> samples(kChunkFrames * 2);
    uint64_t currentGeneration = std::numeric_limits<uint64_t>::max();
    AudioCaptureState previousState = AudioCaptureState::Stopped;
    while (!m_stop.load(std::memory_order_acquire)) {
        const AudioReadResult read = m_audio->Read(samples.data(), kChunkFrames);
        if (m_overlay) {
            m_overlay->PushAudioClock(read.firstSample + read.frames,
                                      read.streamGeneration, read.discontinuity);
            m_overlay->PushAudioLevels(m_audio->GetCurrentLevels());
        }
        const AudioCaptureStatus status = m_audio->GetStatus();
        if (status.state != previousState) {
            previousState = status.state;
            if (status.state == AudioCaptureState::Running) {
                std::cout << "[EchoRadar] Audio running on: " << status.endpointName << '\n';
            } else if (status.state == AudioCaptureState::Recovering) {
                std::cerr << "[EchoRadar] Audio recovering: " << status.lastError << '\n';
            }
        }
        if (read.discontinuity || read.streamGeneration != currentGeneration) {
            currentGeneration = read.streamGeneration;
            if (m_recognizer) m_recognizer->OnStreamReset(currentGeneration);
        }
        if (read.frames == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (m_recognizer) {
            const AudioBlockView block{
                std::span<const float>(samples.data(), read.frames * 2),
                read.frames,
                48000,
                2,
                read.firstSample,
                read.streamGeneration,
            };
            m_recognizer->OnAudio(block);
            if (m_overlay) {
                m_overlay->PushV4Scores(
                    m_recognizer->LastOutput(), m_recognizer->LastSceneActivity(),
                    m_recognizer->LastError().empty() &&
                        m_recognizer->Stats().inferenceCount != 0);
            }
            if (!m_recognizer->LastError().empty()) {
                std::cerr << "[EchoRadar] V4 recognition paused after an inference error: "
                          << m_recognizer->LastError() << '\n';
                m_recognizer.reset();
            }
        }
    }
    if (m_recognizer) m_recognizer->Flush();
    m_audio->Stop();
}

} // namespace EchoRadar
