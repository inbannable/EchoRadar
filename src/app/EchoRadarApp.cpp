#include "EchoRadarApp.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <limits>
#include <iomanip>
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
    if (m_hud) m_hud->Shutdown();
}

bool EchoRadarApp::Initialise() {
    const std::filesystem::path settingsPath = m_cfg.settingsPath.empty()
        ? AppSettingsFile::DefaultPath() : m_cfg.settingsPath;
    m_settings = std::make_shared<RuntimeSettingsStore>(settingsPath);
    std::string settingsMessage;
    if (!m_settings->Load(&settingsMessage)) {
        std::cout << "[EchoRadar] " << settingsMessage << '\n';
    }
    m_calibration = std::make_shared<CalibrationController>(
        settingsPath.parent_path() / "direction-calibration.tsv");
    std::string calibrationMessage;
    if (!m_calibration->Load(&calibrationMessage)) {
        std::cout << "[EchoRadar] Direction calibration: " << calibrationMessage << '\n';
    }
    const AppSettings appSettings = m_settings->Snapshot();
    if (appSettings.sessionLogging) {
        std::error_code error;
        const auto logDirectory = settingsPath.parent_path() / "sessions";
        std::filesystem::create_directories(logDirectory, error);
        if (!error) {
            m_sessionLog.open(logDirectory / "latest.jsonl",
                              std::ios::binary | std::ios::app);
        }
    }

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
        overlayConfig.runtime_settings = m_settings;
        overlayConfig.calibration = m_calibration;
        m_overlay = std::make_unique<OverlayRenderer>(std::move(overlayConfig));
        if (!m_overlay->Initialise()) {
            std::cerr << "[EchoRadar] Event chart UI could not be initialized; continuing headless.\n";
            m_overlay.reset();
        }

        m_hud = std::make_unique<HudOverlayRenderer>(
            HudOverlayRenderer::Config{m_settings});
        if (!m_hud->Initialise()) {
            std::cerr << "[EchoRadar] Direction HUD could not be initialized; "
                         "the control window remains available.\n";
            m_hud.reset();
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
        if (m_hud && m_hud->IsRunning()) m_hud->Render();
        if (m_overlay && !m_overlay->IsRunning()) {
            Stop();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    if (m_dsp_thread.joinable()) m_dsp_thread.join();
    if (m_overlay) m_overlay->Shutdown();
    if (m_hud) m_hud->Shutdown();
}

void EchoRadarApp::Stop() {
    m_stop.store(true, std::memory_order_release);
}

void EchoRadarApp::HandleEvent(const V4SoundEvent& event) {
    if (m_overlay) m_overlay->PushV4Event(event);
    const AppSettings settings = m_settings ? m_settings->Snapshot() : AppSettings{};
    const bool enabled = event.soundClass == SoundClass::Gunshot
        ? settings.localization.localizeGunshots
        : settings.localization.localizeFootsteps;
    if (enabled) {
        m_pendingLocalizations.push_back({m_nextEventId++, event});
    }
    std::printf(
        "\n[V4 %s] source=%s scene=%s confidence=%.3f onset=%.3fs detected=%.3fs "
        "delivered=%.3fs stream=%llu\n",
        ToString(event.soundClass), ToString(event.sourceHint), ToString(event.sceneState),
        event.confidence, event.onsetSample / 48000.0, event.detectedSample / 48000.0,
        event.deliveredSample / 48000.0,
        static_cast<unsigned long long>(event.streamGeneration));
    std::fflush(stdout);
}

void EchoRadarApp::ProcessPendingLocalizations() {
    if (!m_settings) return;
    const AppSettings settings = m_settings->Snapshot();
    const uint64_t newest = m_audioHistory.GetNewestSampleExclusive();
    const uint64_t oldest = m_audioHistory.GetOldestSample();
    const uint64_t windowFrames = static_cast<uint64_t>(
        settings.localization.sampleWindowMs) * 48000u / 1000u;
    const uint64_t preFrames = static_cast<uint64_t>(
        settings.localization.preOnsetMs) * 48000u / 1000u;

    while (!m_pendingLocalizations.empty()) {
        const PendingLocalization pending = m_pendingLocalizations.front();
        const uint64_t startSample = pending.event.onsetSample > preFrames
            ? pending.event.onsetSample - preFrames : 0;
        const uint64_t endSample = startSample + windowFrames;
        if (endSample > newest) break;
        m_pendingLocalizations.pop_front();

        DirectionResult direction;
        direction.eventId = pending.eventId;
        if (startSample < oldest) {
            direction.status = DirectionStatus::AudioUnavailable;
        } else {
            std::vector<float> clip;
            if (m_audioHistory.ExtractWindow(
                    startSample, static_cast<size_t>(windowFrames), clip)) {
                StereoDirectionFeatures features;
                std::string featureError;
                if (m_directionEstimator.ExtractFeatures(clip, features, &featureError)) {
                    const DirectionCalibrationProfile calibration =
                        m_calibration ? m_calibration->ProfileSnapshot()
                                      : DirectionCalibrationProfile{};
                    const auto inferenceStart = std::chrono::steady_clock::now();
                    direction = m_directionEstimator.EstimateFeatures(
                        pending.eventId, pending.event.soundClass, features,
                        settings.audioProfile, settings.localization, &calibration);
                    direction.inferenceMilliseconds =
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - inferenceStart).count();
                    if (m_calibration && m_calibration->AcceptArmedSample(
                            pending.event.soundClass, features)) {
                        m_calibration->Save(nullptr);
                    }
                } else {
                    direction.status = DirectionStatus::AudioUnavailable;
                }
            } else {
                direction.status = DirectionStatus::AudioUnavailable;
            }
        }

        if (m_overlay) m_overlay->PushLocalizedEvent(pending.event, direction);
        if (m_hud && direction.status == DirectionStatus::Estimated) {
            m_hud->PushEvent(pending.event, direction);
        }
        LogLocalizedEvent(pending.event, direction);
        std::printf("[DIR %s] angle=%.0f confidence=%.3f uncertainty=%.0f "
                    "profile=%s status=%s\n",
                    ToString(pending.event.soundClass), direction.primaryAngleDegrees,
                    direction.confidence, direction.uncertaintyDegrees,
                    ToString(direction.profileSource), ToString(direction.status));
    }
}

void EchoRadarApp::LogLocalizedEvent(const V4SoundEvent& event,
                                     const DirectionResult& direction) {
    if (!m_settings || !m_settings->Snapshot().sessionLogging) return;
    if (!m_sessionLog) {
        std::error_code error;
        const auto logDirectory = m_settings->Path().parent_path() / "sessions";
        std::filesystem::create_directories(logDirectory, error);
        if (error) return;
        m_sessionLog.open(logDirectory / "latest.jsonl",
                          std::ios::binary | std::ios::app);
        if (!m_sessionLog) return;
    }
    m_sessionLog << std::fixed << std::setprecision(4)
                 << "{\"event_id\":" << direction.eventId
                 << ",\"stream_generation\":" << event.streamGeneration
                 << ",\"onset_sample\":" << event.onsetSample
                 << ",\"class\":\"" << ToString(event.soundClass) << "\""
                 << ",\"recognition_confidence\":" << event.confidence
                 << ",\"direction_degrees\":" << direction.primaryAngleDegrees
                 << ",\"direction_confidence\":" << direction.confidence
                 << ",\"uncertainty_degrees\":" << direction.uncertaintyDegrees
                 << ",\"profile_source\":\"" << ToString(direction.profileSource) << "\""
                 << ",\"status\":\"" << ToString(direction.status) << "\""
                 << ",\"inference_ms\":" << direction.inferenceMilliseconds
                 << "}\n";
    m_sessionLog.flush();
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
                if (m_settings && !status.endpointId.empty()) {
                    AppSettings settings = m_settings->Snapshot();
                    if (settings.audioProfile.outputEndpointId != status.endpointId) {
                        settings.audioProfile.outputEndpointId = status.endpointId;
                        m_settings->Update(settings, true, nullptr);
                    }
                }
            } else if (status.state == AudioCaptureState::Recovering) {
                std::cerr << "[EchoRadar] Audio recovering: " << status.lastError << '\n';
            }
        }
        if (read.discontinuity || read.streamGeneration != currentGeneration) {
            currentGeneration = read.streamGeneration;
            if (m_recognizer) m_recognizer->OnStreamReset(currentGeneration);
            m_audioHistory.Reset();
            m_pendingLocalizations.clear();
        }
        if (read.frames == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (m_recognizer) {
            m_audioHistory.PushInterleaved(samples.data(), read.frames, read.firstSample);
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
            ProcessPendingLocalizations();
        } else {
            m_audioHistory.PushInterleaved(samples.data(), read.frames, read.firstSample);
        }
    }
    if (m_recognizer) m_recognizer->Flush();
    m_audio->Stop();
}

} // namespace EchoRadar
