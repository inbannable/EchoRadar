#include "EchoRadarApp.h"

#include <audio/PcmWav.h>
#include <support/FlatJson.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <thread>
#include <vector>

namespace EchoRadar {

EchoRadarApp::EchoRadarApp() : EchoRadarApp(Config{}) {}

EchoRadarApp::EchoRadarApp(Config config) : m_config(std::move(config)) {}

EchoRadarApp::~EchoRadarApp() {
    Stop();
    if (m_dspThread.joinable()) m_dspThread.join();
    if (m_audio) m_audio->Stop();
    if (m_overlay) m_overlay->Shutdown();
    if (m_hud) m_hud->Shutdown();
}

bool EchoRadarApp::Initialise() {
    const std::filesystem::path settingsPath = m_config.settingsPath.empty()
        ? AppSettingsFile::DefaultPath() : m_config.settingsPath;
    m_settings = std::make_shared<RuntimeSettingsStore>(settingsPath);
    std::string settingsMessage;
    if (!m_settings->Load(&settingsMessage)) {
        std::cout << "[EchoRadar] " << settingsMessage << '\n';
    }

    const std::filesystem::path settingsDirectory =
        settingsPath.parent_path().empty()
            ? std::filesystem::current_path()
            : settingsPath.parent_path();
    const std::string sessionTag = "session-" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    m_clipDirectory = settingsDirectory / "sessions" / "clips" / sessionTag;
    {
        std::error_code error;
        std::filesystem::create_directories(m_clipDirectory, error);
        if (error) {
            std::cerr << "[EchoRadar] Scene clips could not be saved: "
                      << error.message() << '\n';
            m_clipDirectory.clear();
        }
    }

    if (m_settings->Snapshot().sessionLogging) {
        std::error_code error;
        const auto logDirectory = settingsDirectory / "sessions";
        std::filesystem::create_directories(logDirectory, error);
        if (!error) {
            m_sessionLog.open(logDirectory / "latest.jsonl",
                              std::ios::binary | std::ios::app);
        }
    }

    m_audio = std::make_unique<AudioCapture>();
    if (!m_audio->Start(m_config.audio)) {
        std::cerr << "[EchoRadar] Audio capture could not start: "
                  << m_audio->GetStatus().lastError << '\n';
        return false;
    }

    RecognitionModelPackage package;
    if (!RecognitionModelPackage::Load(
            m_config.modelDirectory, package, &m_recognitionError)) {
        std::cerr << "[EchoRadar] Recognition paused: "
                  << m_recognitionError << '\n';
    } else {
        m_modelVersion = package.modelVersion;
        m_peakLookaheadFrames = package.peakLookaheadFrames;
        m_runtimeTuning = std::make_shared<RecognitionRuntimeTuningStore>(
            RecognitionRuntimeTuning::FromPackage(package));
        m_model = std::make_shared<OnnxRecognitionModel>(
            package.modelPath, package.contextFrames,
            package.melBins, package.inputChannels);
        if (!m_model->IsLoaded()) {
            m_recognitionError = m_model->LoadError();
            std::cerr << "[EchoRadar] Recognition paused: "
                      << m_recognitionError << '\n';
        } else {
            const size_t planeSize =
                static_cast<size_t>(package.contextFrames) * package.melBins;
            std::vector<float> validationInput(
                static_cast<size_t>(package.inputChannels) * planeSize, 0.0f);
            std::fill(
                validationInput.begin() + static_cast<std::ptrdiff_t>(planeSize),
                validationInput.begin() + static_cast<std::ptrdiff_t>(2 * planeSize),
                -100.0f);
            RecognitionModelOutput validationOutput;
            if (!m_model->Predict(
                    validationInput, validationOutput, &m_recognitionError)) {
                std::cerr << "[EchoRadar] Recognition paused: model contract check failed: "
                          << m_recognitionError << '\n';
            } else {
                m_recognizer = std::make_unique<SoundRecognizer>(
                    m_model, package,
                    [this](const SoundEvent& event) { HandleEvent(event); },
                    m_runtimeTuning);
                std::cout << "[EchoRadar] Recognition model loaded: "
                          << package.modelVersion << '\n';
            }
        }
    }

    if (!m_config.directionModelDirectory.empty()) {
        DirectionModelPackage directionPackage;
        if (!DirectionModelPackage::Load(
                m_config.directionModelDirectory,
                directionPackage, &m_directionError)) {
            std::cerr << "[EchoRadar] Direction inference paused: "
                      << m_directionError << '\n';
        } else {
            m_directionModelVersion = directionPackage.modelVersion;
            m_directionEngine = std::make_unique<DirectionOnnxEngine>(
                std::move(directionPackage));
            if (!m_directionEngine->IsLoaded()) {
                m_directionError = m_directionEngine->LoadError();
                std::cerr << "[EchoRadar] Direction inference paused: "
                          << m_directionError << '\n';
                m_directionEngine.reset();
            } else {
                std::vector<float> validationInput(
                    static_cast<size_t>(
                        m_directionEngine->Package().contextSamples) * 2u,
                    0.0f);
                std::string validationError;
                const DirectionSceneResult validation =
                    m_directionEngine->Predict(
                        0, 0, 0, 0, validationInput,
                        DirectionSceneResult::kGunshotClassBit |
                            DirectionSceneResult::kFootstepClassBit,
                        &validationError);
                if (validation.status == DirectionStatus::InferenceFailed ||
                    validation.status == DirectionStatus::ModelUnavailable ||
                    !validationError.empty()) {
                    m_directionError = validationError.empty()
                        ? "Direction model startup contract check failed"
                        : validationError;
                    std::cerr << "[EchoRadar] Direction inference paused: "
                              << m_directionError << '\n';
                    m_directionEngine.reset();
                } else {
                    std::cout << "[EchoRadar] Direction model loaded: "
                              << m_directionModelVersion << '\n';
                }
            }
        }
    }

    if (m_config.showOverlay) {
        OverlayRenderer::Config overlayConfig;
        overlayConfig.sampleRate = 48000;
        overlayConfig.peakLookaheadFrames = m_peakLookaheadFrames;
        overlayConfig.modelVersion = m_modelVersion;
        overlayConfig.recognitionError = m_recognitionError;
        overlayConfig.directionModelVersion = m_directionModelVersion;
        overlayConfig.directionError = m_directionError;
        overlayConfig.runtimeTuning = m_runtimeTuning;
        overlayConfig.runtimeSettings = m_settings;
        m_overlay = std::make_unique<OverlayRenderer>(
            std::move(overlayConfig));
        if (!m_overlay->Initialise()) {
            std::cerr << "[EchoRadar] Control UI could not be initialized; "
                         "continuing headless.\n";
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
        std::cout << "[EchoRadar] Capturing system output: "
                  << status.endpointName << '\n';
    } else {
        std::cout << "[EchoRadar] Waiting for a Windows output endpoint: "
                  << status.lastError << '\n';
    }
    return true;
}

void EchoRadarApp::Run() {
    m_dspThread = std::thread(&EchoRadarApp::DSPLoop, this);
    while (!m_stop.load(std::memory_order_acquire)) {
        if (m_overlay && m_overlay->IsRunning()) m_overlay->Render();
        if (m_hud && m_hud->IsRunning()) m_hud->Render();
        if (m_overlay && !m_overlay->IsRunning()) {
            Stop();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    if (m_dspThread.joinable()) m_dspThread.join();
    if (m_overlay) m_overlay->Shutdown();
    if (m_hud) m_hud->Shutdown();
}

void EchoRadarApp::Stop() {
    m_stop.store(true, std::memory_order_release);
}

void EchoRadarApp::HandleEvent(const SoundEvent& event) {
    if (m_overlay) m_overlay->PushRecognitionEvent(event);
    const uint64_t eventId = m_nextEventId++;
    if (!m_config.directionModelDirectory.empty()) {
        m_sceneCoordinator.AddEvent(eventId, event);
    }
    std::printf(
        "\n[RECOGNITION %s] source=%s scene=%s confidence=%.3f "
        "onset=%.3fs detected=%.3fs delivered=%.3fs stream=%llu\n",
        ToString(event.soundClass), ToString(event.sourceHint),
        ToString(event.sceneState), event.confidence,
        event.onsetSample / 48000.0, event.detectedSample / 48000.0,
        event.deliveredSample / 48000.0,
        static_cast<unsigned long long>(event.streamGeneration));
    std::fflush(stdout);
}

void EchoRadarApp::ProcessPendingDirectionScenes() {
    if (!m_settings || m_config.directionModelDirectory.empty()) return;
    const AppSettings settings = m_settings->Snapshot();
    const uint64_t newest = m_audioHistory.GetNewestSampleExclusive();
    const uint64_t oldest = m_audioHistory.GetOldestSample();
    const uint8_t enabledClassMask = static_cast<uint8_t>(
        (settings.direction.enableGunshots
            ? DirectionSceneResult::kGunshotClassBit : 0u) |
        (settings.direction.enableFootsteps
            ? DirectionSceneResult::kFootstepClassBit : 0u));

    for (const DirectionSceneJob& scene :
         m_sceneCoordinator.TakeReady(newest, oldest)) {
        DirectionSceneResult result;
        result.sceneId = scene.sceneId;
        result.streamGeneration = scene.streamGeneration;
        result.anchorEventSample = scene.anchorEventSample;
        result.sceneStartSample = scene.sceneStartSample;
        result.sceneEndSample = scene.sceneEndSample;
        result.enabledClassMask = enabledClassMask;
        result.modelVersion = m_directionModelVersion;
        result.status = enabledClassMask == 0
            ? DirectionStatus::Disabled
            : DirectionStatus::AudioUnavailable;

        std::vector<float> clip;
        std::filesystem::path clipPath;
        if (scene.audioAvailable &&
            m_audioHistory.ExtractWindow(
                scene.sceneStartSample,
                static_cast<size_t>(
                    scene.sceneEndSample - scene.sceneStartSample),
                clip)) {
            clipPath = SaveSceneClip(scene, clip);
        }

        if (!m_directionEngine) {
            if (enabledClassMask != 0) {
                result.status = DirectionStatus::ModelUnavailable;
            }
        } else if (enabledClassMask == 0 || !clip.empty()) {
            std::string directionError;
            result = m_directionEngine->Predict(
                scene.sceneId, scene.streamGeneration,
                scene.anchorEventSample, scene.sceneStartSample,
                clip, enabledClassMask, &directionError);
            if (!directionError.empty()) {
                std::cerr << "[EchoRadar] Direction scene "
                          << scene.sceneId << " failed: "
                          << directionError << '\n';
            }
        }

        result.resultDeliverySample = newest;
        result.deliveryMilliseconds = newest >= scene.anchorEventSample
            ? static_cast<double>(newest - scene.anchorEventSample) *
                1000.0 / 48000.0
            : 0.0;

        if (m_overlay) {
            m_overlay->PushDirectionScene(scene.events, result, clipPath);
        }
        for (const DirectionSceneEvent& member : scene.events) {
            LogDirectionEvent(member, result, clipPath);
        }
        if (m_hud && !scene.events.empty() && result.sourceCount > 0 &&
            (result.status == DirectionStatus::Estimated ||
             result.status == DirectionStatus::LowConfidence)) {
            m_hud->PushScene(scene.events.front().event, result);
        }
        std::printf(
            "[DIRECTION scene=%llu] sources=%u status=%s inference=%.3fms\n",
            static_cast<unsigned long long>(result.sceneId),
            result.sourceCount, ToString(result.status),
            result.inferenceMilliseconds);
    }
}

std::filesystem::path EchoRadarApp::SaveSceneClip(
    const DirectionSceneJob& scene, const std::vector<float>& clip) {
    if (m_clipDirectory.empty() || clip.empty()) return {};
    const std::filesystem::path path =
        m_clipDirectory / ("scene-" + std::to_string(scene.sceneId) + ".wav");
    PcmAudio audio;
    audio.sampleRate = 48000;
    audio.channels = 2;
    audio.interleaved = clip;
    std::string error;
    if (!WritePcm16Wav(path, audio, &error)) {
        std::cerr << "[EchoRadar] Could not save direction scene audio "
                  << path << ": " << error << '\n';
        return {};
    }
    return path;
}

void EchoRadarApp::LogDirectionEvent(
    const DirectionSceneEvent& member,
    const DirectionSceneResult& scene,
    const std::filesystem::path& clipPath) {
    if (!m_settings || !m_settings->Snapshot().sessionLogging) return;
    if (!m_sessionLog) {
        std::error_code error;
        const auto directory = m_settings->Path().parent_path() / "sessions";
        std::filesystem::create_directories(directory, error);
        if (error) return;
        m_sessionLog.open(
            directory / "latest.jsonl", std::ios::binary | std::ios::app);
        if (!m_sessionLog) return;
    }

    const SoundEvent& event = member.event;
    m_sessionLog
        << std::fixed << std::setprecision(4)
        << "{\"schema_version\":2"
        << ",\"event_id\":" << member.eventId
        << ",\"stream_generation\":" << event.streamGeneration
        << ",\"class\":\"" << ToString(event.soundClass) << "\""
        << ",\"recognition_model_version\":\""
        << detail::JsonEscapeStr(event.modelVersion) << "\""
        << ",\"recognition_onset_sample\":" << event.onsetSample
        << ",\"recognition_end_sample\":" << event.endSample
        << ",\"recognition_detected_sample\":" << event.detectedSample
        << ",\"recognition_delivered_sample\":" << event.deliveredSample
        << ",\"recognition_confidence\":" << event.confidence
        << ",\"recognition_source_hint\":\""
        << ToString(event.sourceHint) << "\""
        << ",\"recognition_source_confidence\":" << event.sourceConfidence
        << ",\"recognition_scene_state\":\""
        << ToString(event.sceneState) << "\""
        << ",\"scene_id\":" << scene.sceneId
        << ",\"scene_start_sample\":" << scene.sceneStartSample
        << ",\"scene_end_sample\":" << scene.sceneEndSample
        << ",\"scene_anchor_onset_sample\":" << scene.anchorEventSample
        << ",\"scene_result_delivery_sample\":" << scene.resultDeliverySample
        << ",\"scene_delivery_ms\":" << scene.deliveryMilliseconds
        << ",\"direction_status\":\"" << ToString(scene.status) << "\""
        << ",\"direction_model_version\":\""
        << detail::JsonEscapeStr(scene.modelVersion) << "\""
        << ",\"direction_preprocessing_version\":\""
        << detail::JsonEscapeStr(scene.preprocessingVersion) << "\""
        << ",\"enabled_classes\":{\"gunshot\":"
        << ((scene.enabledClassMask &
             DirectionSceneResult::kGunshotClassBit) ? "true" : "false")
        << ",\"footstep\":"
        << ((scene.enabledClassMask &
             DirectionSceneResult::kFootstepClassBit) ? "true" : "false")
        << "}"
        << ",\"direction_inference_ms\":" << scene.inferenceMilliseconds
        << ",\"direction_feature_frames\":" << scene.featureFrames
        << ",\"direction_input_channels\":" << scene.inputChannels
        << ",\"direction_mel_bins\":" << scene.melBins
        << ",\"direction_sample_rate\":" << scene.sampleRate
        << ",\"direction_source_count\":" << scene.sourceCount
        << ",\"clip_path\":\""
        << detail::JsonEscapeStr(clipPath.string()) << "\""
        << ",\"sources\":[";

    for (uint32_t index = 0;
         index < std::min<uint32_t>(
             scene.sourceCount,
             static_cast<uint32_t>(
                 DirectionSceneResult::kMaximumSources));
         ++index) {
        if (index != 0) m_sessionLog << ',';
        const auto& source = scene.sources[index];
        m_sessionLog
            << "{\"azimuth_degrees\":" << source.azimuthDegrees
            << ",\"elevation_degrees\":" << source.elevationDegrees
            << ",\"confidence\":" << source.confidence
            << ",\"uncertainty_degrees\":" << source.uncertaintyDegrees
            << '}';
    }
    m_sessionLog << "]}\n";
    m_sessionLog.flush();
}

void EchoRadarApp::DSPLoop() {
    constexpr size_t kChunkFrames = 480;
    std::vector<float> samples(kChunkFrames * 2);
    uint64_t currentGeneration = std::numeric_limits<uint64_t>::max();
    AudioCaptureState previousState = AudioCaptureState::Stopped;

    while (!m_stop.load(std::memory_order_acquire)) {
        const AudioReadResult read =
            m_audio->Read(samples.data(), kChunkFrames);
        if (m_overlay) {
            m_overlay->PushAudioClock(
                read.firstSample + read.frames,
                read.streamGeneration, read.discontinuity);
            m_overlay->PushAudioLevels(m_audio->GetCurrentLevels());
        }

        const AudioCaptureStatus status = m_audio->GetStatus();
        if (status.state != previousState) {
            previousState = status.state;
            if (status.state == AudioCaptureState::Running) {
                std::cout << "[EchoRadar] Audio running on: "
                          << status.endpointName << '\n';
                if (m_settings && !status.endpointId.empty()) {
                    AppSettings settings = m_settings->Snapshot();
                    if (settings.audioProfile.outputEndpointId !=
                        status.endpointId) {
                        settings.audioProfile.outputEndpointId =
                            status.endpointId;
                        m_settings->Update(settings, true, nullptr);
                    }
                }
            } else if (status.state == AudioCaptureState::Recovering) {
                std::cerr << "[EchoRadar] Audio recovering: "
                          << status.lastError << '\n';
            }
        }

        if (read.discontinuity ||
            read.streamGeneration != currentGeneration) {
            currentGeneration = read.streamGeneration;
            if (m_recognizer) {
                m_recognizer->OnStreamReset(currentGeneration);
            }
            m_audioHistory.Reset();
            m_sceneCoordinator.Reset(currentGeneration);
        }
        if (read.frames == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        m_audioHistory.PushInterleaved(
            samples.data(), read.frames, read.firstSample);
        if (m_recognizer) {
            const AudioBlockView block{
                std::span<const float>(
                    samples.data(), read.frames * 2),
                read.frames,
                48000,
                2,
                read.firstSample,
                read.streamGeneration,
            };
            m_recognizer->OnAudio(block);
            if (m_overlay) {
                m_overlay->PushRecognitionScores(
                    m_recognizer->LastOutput(),
                    m_recognizer->LastSceneActivity(),
                    m_recognizer->LastError().empty() &&
                        m_recognizer->Stats().inferenceCount != 0);
            }
            if (!m_recognizer->LastError().empty()) {
                std::cerr << "[EchoRadar] Recognition paused after an inference error: "
                          << m_recognizer->LastError() << '\n';
                m_recognizer.reset();
            }
        }
        ProcessPendingDirectionScenes();
    }

    if (m_recognizer) m_recognizer->Flush();
    m_audio->Stop();
}

} // namespace EchoRadar
