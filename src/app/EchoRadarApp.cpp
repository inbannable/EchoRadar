#include "EchoRadarApp.h"

#include "../dataset/DatasetJson.h"
#include "../recognition/PcmWav.h"

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
    const auto settingsDirectory = settingsPath.parent_path().empty()
        ? std::filesystem::current_path() : settingsPath.parent_path();
    m_clipSessionTag = "session-" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    m_clipDirectory = settingsDirectory / "sessions" / "clips" / m_clipSessionTag;
    {
        std::error_code error;
        std::filesystem::create_directories(m_clipDirectory, error);
        if (error) {
            std::cerr << "[EchoRadar] Event audio clips could not be saved: "
                      << error.message() << '\n';
            m_clipDirectory.clear();
        }
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

    if (!m_cfg.directionModelDirectory.empty()) {
        DirectionModelPackage directionPackage;
        if (!DirectionModelPackage::Load(
                m_cfg.directionModelDirectory, directionPackage, &m_directionError)) {
            std::cerr << "[EchoRadar] 3D direction inference paused: "
                      << m_directionError << '\n';
        } else {
            m_directionModelVersion = directionPackage.modelVersion;
            m_directionEngine = std::make_unique<DirectionOnnxEngine>(
                std::move(directionPackage));
            if (!m_directionEngine->IsLoaded()) {
                m_directionError = m_directionEngine->LoadError();
                std::cerr << "[EchoRadar] 3D direction inference paused: "
                          << m_directionError << '\n';
            } else {
                std::vector<float> validationInput(
                    static_cast<size_t>(m_directionEngine->Package().contextSamples) * 2u,
                    0.0f);
                std::string validationError;
                const DirectionSceneResult validation = m_directionEngine->Predict(
                    0, 0, 0, 0, validationInput,
                    DirectionSceneResult::kGunshotClassBit |
                        DirectionSceneResult::kFootstepClassBit,
                    &validationError);
                if (validation.status == DirectionStatus::InferenceFailed ||
                    validation.status == DirectionStatus::ModelUnavailable ||
                    !validationError.empty()) {
                    m_directionError = validationError.empty()
                        ? "direction model startup contract check failed" : validationError;
                    std::cerr << "[EchoRadar] 3D direction inference paused: "
                              << m_directionError << '\n';
                    m_directionEngine.reset();
                } else {
                    std::cout << "[EchoRadar] Scene-level 3D direction model loaded: "
                              << m_directionModelVersion << '\n';
                }
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
        overlayConfig.clip_directory = m_clipDirectory;
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
    const uint64_t eventId = m_nextEventId++;
    if (!m_cfg.directionModelDirectory.empty()) {
        m_sceneCoordinator.AddEvent(eventId, event);
    }
    const AppSettings settings = m_settings ? m_settings->Snapshot() : AppSettings{};
    const bool enabled = event.soundClass == SoundClass::Gunshot
        ? settings.localization.localizeGunshots
        : settings.localization.localizeFootsteps;
    // Keep the clip job even when direction display is disabled. The same
    // audio window is useful for listening and recognition debugging.
    if (m_cfg.legacyDirectionDiagnostic) {
        m_pendingLocalizations.push_back({eventId, event, enabled});
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

    if (!m_cfg.directionModelDirectory.empty()) {
        const uint8_t enabledClassMask = static_cast<uint8_t>(
            (settings.localization.localizeGunshots
                ? DirectionSceneResult::kGunshotClassBit : 0u) |
            (settings.localization.localizeFootsteps
                ? DirectionSceneResult::kFootstepClassBit : 0u));
        for (const DirectionSceneJob& scene : m_sceneCoordinator.TakeReady(newest, oldest)) {
            DirectionSceneResult sceneResult;
            sceneResult.sceneId = scene.sceneId;
            sceneResult.streamGeneration = scene.streamGeneration;
            sceneResult.anchorEventSample = scene.anchorEventSample;
            sceneResult.sceneStartSample = scene.sceneStartSample;
            sceneResult.sceneEndSample = scene.sceneEndSample;
            sceneResult.enabledClassMask = enabledClassMask;
            sceneResult.modelVersion = m_directionModelVersion;
            sceneResult.status = enabledClassMask == 0
                ? DirectionStatus::Disabled : DirectionStatus::AudioUnavailable;
            std::vector<float> clip;
            std::filesystem::path clipPath;
            if (scene.audioAvailable && m_audioHistory.ExtractWindow(
                    scene.sceneStartSample,
                    static_cast<size_t>(scene.sceneEndSample - scene.sceneStartSample),
                    clip)) {
                clipPath = SaveSceneClip(scene, clip);
            }
            if (!m_directionEngine) {
                if (enabledClassMask != 0) sceneResult.status = DirectionStatus::ModelUnavailable;
            } else if (enabledClassMask == 0 || !clip.empty()) {
                std::string directionError;
                sceneResult = m_directionEngine->Predict(
                    scene.sceneId, scene.streamGeneration, scene.anchorEventSample,
                    scene.sceneStartSample, clip, enabledClassMask, &directionError);
                if (!directionError.empty()) {
                    std::cerr << "[EchoRadar] Direction scene " << scene.sceneId
                              << " failed: " << directionError << '\n';
                }
            }
            sceneResult.resultDeliverySample = newest;
            sceneResult.deliveryMilliseconds = newest >= scene.anchorEventSample
                ? static_cast<double>(newest - scene.anchorEventSample) * 1000.0 / 48000.0
                : 0.0;

            for (const DirectionSceneEvent& member : scene.events) {
                const DirectionResult compatibility = sceneResult.LegacyResult(member.eventId);
                if (m_overlay) {
                    m_overlay->PushLocalizedEvent(member.event, compatibility, clipPath);
                }
                LogLocalizedEvent(member.event, compatibility, clipPath, &sceneResult);
            }
            if (m_hud && sceneResult.sourceCount > 0 &&
                (sceneResult.status == DirectionStatus::Estimated ||
                 sceneResult.status == DirectionStatus::LowConfidence)) {
                m_hud->PushScene(scene.events.front().event, sceneResult);
            }
            std::printf("[DIR scene=%llu] sources=%u status=%s inference=%.3fms\n",
                        static_cast<unsigned long long>(sceneResult.sceneId),
                        sceneResult.sourceCount, ToString(sceneResult.status),
                        sceneResult.inferenceMilliseconds);
        }
    }

    const uint64_t preFrames = static_cast<uint64_t>(
        settings.localization.preOnsetMs) * 48000u / 1000u;

    while (!m_pendingLocalizations.empty()) {
        const PendingLocalization pending = m_pendingLocalizations.front();
        const auto& peakTuning = settings.localization.PeakWindowFor(
            pending.event.soundClass);
        const uint64_t startSample = pending.event.onsetSample > preFrames
            ? pending.event.onsetSample - preFrames : 0;
        const uint64_t fallbackEnd = startSample + static_cast<uint64_t>(
            settings.localization.sampleWindowMs) * 48000u / 1000u;
        const uint64_t eventEnd = std::max(pending.event.endSample,
                                           pending.event.onsetSample);
        const uint64_t endSample = std::max(
            fallbackEnd,
            eventEnd + static_cast<uint64_t>(peakTuning.afterPeakMs) * 48000u / 1000u);
        const uint64_t windowFrames = endSample - startSample;
        if (endSample > newest) break;
        m_pendingLocalizations.pop_front();

        DirectionResult direction;
        direction.eventId = pending.eventId;
        direction.status = pending.enabled
            ? DirectionStatus::AudioUnavailable : DirectionStatus::Disabled;
        std::filesystem::path clipPath;
        if (startSample >= oldest) {
            std::vector<float> clip;
            if (m_audioHistory.ExtractWindow(
                    startSample, static_cast<size_t>(windowFrames), clip)) {
                PeakWindowSelection selection;
                std::vector<float> selectedClip;
                std::string selectionError;
                const bool peakAccepted = m_directionEstimator.SelectPeakWindow(
                    clip, peakTuning, selection, selectedClip, &selectionError);
                // Persist the exact peak-centered PCM sent to the feature
                // extractor. Rejected broad windows remain available for
                // diagnosing the rejection.
                clipPath = SaveEventClip(pending.eventId, pending.event,
                                         peakAccepted ? selectedClip : clip);
                direction.featureSchemaVersion = StereoDirectionFeatures::kSchemaVersion;
                direction.mapperVersion = DeterministicDirectionMapper::kVersion;
                direction.peakSample = startSample + selection.peakFrame;
                direction.clipStartSample = startSample + selection.startFrame;
                direction.clipEndSample = startSample + selection.endFrame;
                direction.peakToNoiseDb = selection.peakToNoiseDb;
                direction.activeFrameFraction = selection.activeFrameFraction;
                if (pending.enabled && peakAccepted) {
                    StereoDirectionFeatures features;
                    std::string featureError;
                    if (m_directionEstimator.ExtractFeatures(selectedClip, features, &featureError)) {
                        features.peakSample = direction.peakSample;
                        features.clipStartSample = direction.clipStartSample;
                        features.clipEndSample = direction.clipEndSample;
                        features.peakToNoiseDb = selection.peakToNoiseDb;
                        features.activeFrameFraction = selection.activeFrameFraction;
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
                } else if (pending.enabled) {
                    direction.status = DirectionStatus::LowConfidence;
                }
            }
        }

        if (m_overlay) {
            m_overlay->PushLocalizedEvent(pending.event, direction, clipPath);
        }
        if (m_hud && (direction.status == DirectionStatus::Estimated ||
                      direction.status == DirectionStatus::LowConfidence)) {
            m_hud->PushEvent(pending.event, direction);
        }
        LogLocalizedEvent(pending.event, direction, clipPath, nullptr);
        std::printf("[DIR %s] angle=%.0f confidence=%.3f uncertainty=%.0f "
                    "profile=%s status=%s\n",
                    ToString(pending.event.soundClass), direction.primaryAngleDegrees,
                    direction.confidence, direction.uncertaintyDegrees,
                    ToString(direction.profileSource), ToString(direction.status));
    }
}

std::filesystem::path EchoRadarApp::SaveEventClip(
    uint64_t eventId, const V4SoundEvent& event, const std::vector<float>& clip) {
    if (m_clipDirectory.empty() || clip.empty()) return {};

    const std::filesystem::path path = m_clipDirectory /
        ("event-" + std::to_string(eventId) + "-" +
         ToString(event.soundClass) + ".wav");
    PcmAudio audio;
    audio.sampleRate = 48000;
    audio.channels = 2;
    audio.interleaved = clip;
    std::string error;
    if (!WritePcm16Wav(path, audio, &error)) {
        std::cerr << "[EchoRadar] Could not save event audio clip "
                  << path << ": " << error << '\n';
        return {};
    }
    return path;
}

std::filesystem::path EchoRadarApp::SaveSceneClip(
    const DirectionSceneJob& scene, const std::vector<float>& clip) {
    if (m_clipDirectory.empty() || clip.empty()) return {};
    const std::filesystem::path path = m_clipDirectory /
        ("scene-" + std::to_string(scene.sceneId) + ".wav");
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

void EchoRadarApp::LogLocalizedEvent(const V4SoundEvent& event,
                                     const DirectionResult& direction,
                                     const std::filesystem::path& clipPath,
                                     const DirectionSceneResult* scene) {
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
                 << ",\"peak_sample\":" << direction.peakSample
                 << ",\"selected_clip_start_sample\":" << direction.clipStartSample
                 << ",\"selected_clip_end_sample\":" << direction.clipEndSample
                 << ",\"peak_to_noise_db\":" << direction.peakToNoiseDb
                 << ",\"active_frame_fraction\":" << direction.activeFrameFraction
                 << ",\"gcc_quality\":" << direction.gccQuality
                 << ",\"feature_schema\":" << direction.featureSchemaVersion
                 << ",\"mapper_version\":" << direction.mapperVersion
                 << ",\"clip_path\":\""
                 << detail::JsonEscapeStr(clipPath.string()) << "\"";
    if (scene) {
        m_sessionLog
            << ",\"scene_id\":" << scene->sceneId
            << ",\"scene_start_sample\":" << scene->sceneStartSample
            << ",\"scene_end_sample\":" << scene->sceneEndSample
            << ",\"scene_anchor_onset_sample\":" << scene->anchorEventSample
            << ",\"scene_result_delivery_sample\":" << scene->resultDeliverySample
            << ",\"scene_delivery_ms\":" << scene->deliveryMilliseconds
            << ",\"direction_source_count\":" << scene->sourceCount
            << ",\"enabled_class_mask\":{\"gunshot\":"
            << ((scene->enabledClassMask & DirectionSceneResult::kGunshotClassBit)
                    ? "true" : "false")
            << ",\"footstep\":"
            << ((scene->enabledClassMask & DirectionSceneResult::kFootstepClassBit)
                    ? "true" : "false") << "}"
            << ",\"direction_model_version\":\""
            << detail::JsonEscapeStr(scene->modelVersion) << "\""
            << ",\"direction_preprocessing_version\":\""
            << detail::JsonEscapeStr(scene->preprocessingVersion) << "\""
            << ",\"direction_feature_frames\":" << scene->featureFrames
            << ",\"direction_sources\":[";
        for (uint32_t index = 0; index < std::min<uint32_t>(
                 scene->sourceCount,
                 static_cast<uint32_t>(DirectionSceneResult::kMaximumSources)); ++index) {
            if (index != 0) m_sessionLog << ',';
            const auto& source = scene->sources[index];
            m_sessionLog
                << "{\"azimuth_degrees\":" << source.azimuthDegrees
                << ",\"elevation_degrees\":" << source.elevationDegrees
                << ",\"confidence\":" << source.confidence
                << ",\"uncertainty_degrees\":" << source.uncertaintyDegrees
                << '}';
        }
        m_sessionLog << ']';
    }
    m_sessionLog << "}\n";
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
            m_sceneCoordinator.Reset(currentGeneration);
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
