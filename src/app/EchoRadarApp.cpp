#include "EchoRadarApp.h"

#include <recognition/RecognitionModelPackage.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace EchoRadar {
namespace {

constexpr size_t kChunkFrames = RadarProcessor::kHopSize;
constexpr size_t kHistoryFrames = 48000 * 3;
constexpr size_t kEventPreFrames = 2048;
constexpr size_t kEventWindowFrames = 8192;

std::string SessionTag() {
    return "session-" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

bool IsEventMode(RadarMode mode) {
    return mode == RadarMode::Events || mode == RadarMode::Combined;
}

std::optional<AudioChannelLayout> NativeLayout(
    const AudioCaptureStatus& status) {
    return MakeAudioChannelLayout(
        status.nativeChannels, status.nativeChannelMask);
}

bool IsNativeRadarFormat(const AudioCaptureStatus& status,
                         const AudioChannelLayout& layout) {
    return IsNativeDirectionalRadarFormat(status, layout);
}

HudDisplaySettings EffectiveHudSettings(
    const AppSettings& settings, std::string_view displayId) {
    const std::string effectiveId = displayId.empty()
        ? "default" : std::string(displayId);
    HudDisplaySettings hud;
    if (const HudDisplaySettings* exact =
            FindHudDisplaySettings(settings, effectiveId)) {
        hud = *exact;
    } else if (const HudDisplaySettings* fallback =
                   FindHudDisplaySettings(settings, "default")) {
        hud = *fallback;
    }
    hud.displayId = effectiveId;
    return hud;
}

} // namespace

EchoRadarApp::EchoRadarApp() : EchoRadarApp(Config{}) {}
EchoRadarApp::EchoRadarApp(Config config) : m_config(std::move(config)) {}

EchoRadarApp::~EchoRadarApp() {
    Stop();
    if (m_dspThread.joinable()) m_dspThread.join();
    StopSettingsSaver();
    FlushRecording();
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

    AppSettings settings = m_settings->Snapshot();
    if (m_config.radarMode) settings.radar.mode = *m_config.radarMode;
    if (m_config.radarPreset) settings.radar.preset = *m_config.radarPreset;
    m_settings->Update(settings, false, nullptr);
    m_settingsSaver = std::make_unique<DebouncedSettingsSaver>(m_settings);
    if (m_config.audio.endpointId.empty() &&
        !settings.audioProfile.outputEndpointId.empty()) {
        m_config.audio.selection = AudioEndpointSelection::Fixed;
        m_config.audio.endpointId = settings.audioProfile.outputEndpointId;
    }

    const std::filesystem::path settingsDirectory =
        settingsPath.parent_path().empty()
            ? std::filesystem::current_path() : settingsPath.parent_path();
    m_sessionDirectory = settingsDirectory / "sessions" / SessionTag();
    std::error_code filesystemError;
    std::filesystem::create_directories(m_sessionDirectory, filesystemError);
    if (filesystemError) {
        m_sessionLoggingError = "Could not create the v2 session directory: " +
            filesystemError.message();
        std::cerr << "[EchoRadar] " << m_sessionLoggingError << '\n';
        m_sessionDirectory.clear();
        if (settings.sessionLogging) {
            settings.sessionLogging = false;
            m_settings->Update(settings, false, nullptr);
            m_settingsSaver->RequestSave();
        }
    } else if (settings.sessionLogging) {
        std::string logError;
        if (!m_sessionLog.Open(m_sessionDirectory / "session.jsonl", &logError)) {
            m_sessionLoggingError = logError;
            std::cerr << "[EchoRadar] " << m_sessionLoggingError << '\n';
            settings.sessionLogging = false;
            m_settings->Update(settings, false, nullptr);
            m_settingsSaver->RequestSave();
        }
    }

    m_radar = std::make_unique<RadarProcessor>(settings.radar);
    m_snapshots = std::make_shared<LatestSnapshotPublisher>();
    m_commands = std::make_shared<UiCommandQueue>();
    m_deviceManager = std::make_unique<AudioDeviceManager>();
    RefreshDevices();

    m_audio = std::make_unique<AudioCapture>();
    if (!m_audio->Start(m_config.audio)) {
        std::cerr << "[EchoRadar] Capture is unavailable: "
                  << m_audio->GetStatus().lastError
                  << ". Dashboard and settings remain available.\n";
    }

    if (m_config.modelDirectory.empty()) {
        m_modelState = ModelUiState::Disabled;
        m_modelStatus = "Recognition disabled; continuous radar needs no model";
    } else {
        m_modelState = ModelUiState::Loading;
        RecognitionModelPackage package;
        if (!RecognitionModelPackage::Load(
                m_config.modelDirectory, package, &m_recognitionError)) {
            m_modelState = std::filesystem::exists(m_config.modelDirectory)
                ? ModelUiState::Malformed : ModelUiState::Missing;
            m_modelStatus = m_recognitionError;
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
                m_modelState = ModelUiState::Malformed;
                m_recognitionError = m_model->LoadError();
                m_modelStatus = m_recognitionError;
            } else {
                const size_t planeSize =
                    static_cast<size_t>(package.contextFrames) * package.melBins;
                std::vector<float> validationInput(
                    static_cast<size_t>(package.inputChannels) * planeSize, 0.0f);
                if (validationInput.size() >= planeSize * 2u) {
                    std::fill(validationInput.begin() +
                                  static_cast<std::ptrdiff_t>(planeSize),
                              validationInput.begin() +
                                  static_cast<std::ptrdiff_t>(planeSize * 2u),
                              -100.0f);
                }
                RecognitionModelOutput validationOutput;
                if (!m_model->Predict(validationInput, validationOutput,
                                      &m_recognitionError)) {
                    m_modelState = ModelUiState::Malformed;
                    m_modelStatus = "Model contract failed: " + m_recognitionError;
                } else {
                    m_recognizer = std::make_unique<SoundRecognizer>(
                        m_model, package,
                        [this](const SoundEvent& event) { HandleEvent(event); },
                        m_runtimeTuning);
                    m_modelState = ModelUiState::Ready;
                    m_modelStatus = "Recognition ready";
                    std::cout << "[EchoRadar] Recognition model loaded: "
                              << m_modelVersion << '\n';
                }
            }
        }
    }

    PublishSnapshot(m_audio->GetStatus(), m_audio->GetCurrentLevels());
    if (m_config.showOverlay) {
        OverlayRenderer::Config dashboardConfig;
        dashboardConfig.windowX = static_cast<int>(settings.dashboard.windowX);
        dashboardConfig.windowY = static_cast<int>(settings.dashboard.windowY);
        dashboardConfig.windowWidth = static_cast<int>(settings.dashboard.windowWidth);
        dashboardConfig.windowHeight = static_cast<int>(settings.dashboard.windowHeight);
        dashboardConfig.maximized = settings.dashboard.maximized;
        dashboardConfig.snapshots = m_snapshots;
        dashboardConfig.commands = m_commands;
        m_overlay = std::make_unique<OverlayRenderer>(std::move(dashboardConfig));
        if (!m_overlay->Initialise()) {
            std::cerr << "[EchoRadar] Dashboard initialization failed; continuing headless.\n";
            m_overlay.reset();
        }

        HudOverlayRenderer::Config hudConfig;
        hudConfig.snapshots = m_snapshots;
        hudConfig.commands = m_commands;
        m_hud = std::make_unique<HudOverlayRenderer>(std::move(hudConfig));
        if (!m_hud->Initialise()) {
            std::cerr << "[EchoRadar] HUD initialization failed; the dashboard remains available.\n";
            m_hud.reset();
        }
    }
    return true;
}

void EchoRadarApp::Run() {
    m_stop.store(false, std::memory_order_release);
    m_dspThread = std::thread(&EchoRadarApp::DSPLoop, this);
    while (!m_stop.load(std::memory_order_acquire)) {
        if (m_overlay && m_overlay->IsRunning()) m_overlay->Render();
        if (m_hud && m_hud->IsRunning()) m_hud->Render();
        if (m_overlay && !m_overlay->IsRunning()) {
            Stop();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    if (m_overlay) m_overlay->FlushPendingPlacement();
    if (m_dspThread.joinable()) m_dspThread.join();
    while (m_commands && !m_commands->Empty()) ProcessUiCommands();
    StopSettingsSaver();
    if (m_overlay) m_overlay->Shutdown();
    if (m_hud) m_hud->Shutdown();
}

void EchoRadarApp::Stop() {
    m_stop.store(true, std::memory_order_release);
}

void EchoRadarApp::HandleEvent(const SoundEvent& event) {
    const uint64_t eventId = m_nextEventId++;
    m_pendingEvents.push_back({eventId, event});
    RecentEventSnapshot recent;
    recent.eventId = eventId;
    recent.streamGeneration = event.streamGeneration;
    recent.timestampSeconds = static_cast<double>(event.onsetSample) / 48000.0;
    recent.soundClass = event.soundClass;
    recent.confidence = event.confidence;
    recent.suppressed = event.suppressed;
    recent.miniatureRadarDbfs.fill(kRadarSilenceDbfs);
    m_recentEvents.push_back(recent);
    const uint32_t maximum = m_settings
        ? m_settings->Snapshot().eventDisplay.maximumRecentEvents : 100u;
    if (m_recentEvents.size() > maximum) {
        m_recentEvents.erase(
            m_recentEvents.begin(),
            m_recentEvents.begin() + static_cast<std::ptrdiff_t>(
                m_recentEvents.size() - maximum));
    }
    std::printf("[EVENT %llu %s] confidence=%.3f onset=%.3fs\n",
                static_cast<unsigned long long>(eventId),
                ToString(event.soundClass), event.confidence,
                recent.timestampSeconds);
}

void EchoRadarApp::ProcessPendingRadarEvents() {
    if (!m_radar || !m_surroundHistory || m_pendingEvents.empty()) return;
    const AppSettings settings = m_settings->Snapshot();
    if (!IsEventMode(settings.radar.mode)) {
        FinalizePendingRadarEvents(RadarRuntimeStatus::MalformedInput);
        return;
    }

    const uint64_t oldest = m_surroundHistory->GetOldestSample();
    const uint64_t newest = m_surroundHistory->GetNewestSampleExclusive();
    while (!m_pendingEvents.empty()) {
        const PendingRadarEvent& pending = m_pendingEvents.front();
        const uint64_t start = pending.event.onsetSample > kEventPreFrames
            ? pending.event.onsetSample - kEventPreFrames : 0;
        const uint64_t end = start + kEventWindowFrames;
        if (pending.event.streamGeneration == m_currentGeneration && newest < end) {
            break;
        }

        RadarEventResult result;
        result.recognitionEvent = pending.event;
        result.preset = settings.radar.preset;
        std::vector<float> window;
        if (pending.event.streamGeneration != m_currentGeneration || start < oldest ||
            !m_surroundHistory->ExtractWindow(start, kEventWindowFrames, window)) {
            result.status = RadarRuntimeStatus::MalformedInput;
        } else {
            const SurroundAudioBlockView block{
                std::span<const float>(window.data(), window.size()),
                kEventWindowFrames,
                48000,
                m_currentLayout,
                start,
                m_currentGeneration,
                false,
            };
            result = m_radar->AnalyzeEventWindow(pending.event, block);
        }

        const auto recent = std::find_if(
            m_recentEvents.begin(), m_recentEvents.end(),
            [&](const RecentEventSnapshot& item) {
                return item.eventId == pending.eventId;
            });
        if (recent != m_recentEvents.end()) {
            recent->peakCount = result.peakCount;
            recent->peaks = result.peaks;
            recent->miniatureRadarDbfs.fill(kRadarSilenceDbfs);
            if (result.peakCount > 0) {
                recent->strongestAzimuthDegrees = result.peaks[0].azimuthDegrees;
            }
            for (uint32_t index = 0; index < result.peakCount; ++index) {
                const uint32_t sector = static_cast<uint32_t>(std::lround(
                    result.peaks[index].azimuthDegrees / kRadarSectorWidthDegrees)) %
                    static_cast<uint32_t>(kRadarSectorCount);
                recent->miniatureRadarDbfs[sector] = result.peaks[index].energyDbfs;
            }
        }
        m_sessionLog.WriteEventDirection(pending.eventId, result);
        std::printf("[RADAR EVENT %llu] peaks=%u status=%s\n",
                    static_cast<unsigned long long>(pending.eventId),
                    result.peakCount, ToString(result.status));
        m_pendingEvents.pop_front();
    }
}

void EchoRadarApp::FinalizePendingRadarEvents(RadarRuntimeStatus status) {
    const RadarPreset preset = m_settings
        ? m_settings->Snapshot().radar.preset : RadarPreset::All;
    while (!m_pendingEvents.empty()) {
        const PendingRadarEvent pending = m_pendingEvents.front();
        RadarEventResult result;
        result.recognitionEvent = pending.event;
        result.preset = preset;
        result.status = status;
        m_sessionLog.WriteEventDirection(pending.eventId, result);
        std::printf("[RADAR EVENT %llu] peaks=0 status=%s\n",
                    static_cast<unsigned long long>(pending.eventId),
                    ToString(status));
        m_pendingEvents.pop_front();
    }
}

void EchoRadarApp::ResetPipelines(const AudioReadResult& read) {
    FinalizePendingRadarEvents(RadarRuntimeStatus::MalformedInput);
    m_currentGeneration = read.streamGeneration;
    m_currentAudioSample = read.firstSample;
    m_currentLayout = read.layout;
    if (m_recognizer) m_recognizer->OnStreamReset(m_currentGeneration);
    if (m_radar) m_radar->Reset(m_currentGeneration);
    m_surroundHistory = read.layout.channelCount == 0
        ? nullptr
        : std::make_unique<AudioHistoryBuffer>(
              kHistoryFrames, 48000, read.layout.channelCount);
    m_latestRadarFrame = {};
    m_latestRadarFrame.layout = read.layout;
    m_latestRadarFrame.streamGeneration = read.streamGeneration;
    m_latestRadarFrame.status = read.layout.IsDirectional()
        ? RadarRuntimeStatus::WaitingForAudio
        : RadarRuntimeStatus::UnsupportedLayout;
    if (m_recording) {
        FlushRecording();
        BeginRecording(read.layout);
    }
}

void EchoRadarApp::ApplyRadarSettings(const AppSettings& settings) {
    if (m_radar) m_radar->SetConfig(settings.radar);
}

void EchoRadarApp::CommitSettings(const AppSettings& settings) {
    if (!m_settings) return;
    m_settings->Update(settings, false, nullptr);
    if (m_settingsSaver) {
        m_settingsSaver->RequestSave();
    }
}

void EchoRadarApp::StopSettingsSaver() {
    if (!m_settingsSaver) return;
    std::string error;
    if (!m_settingsSaver->Stop(&error) && !error.empty()) {
        std::cerr << "[EchoRadar] Could not persist settings during shutdown: "
                  << error << '\n';
    }
}

void EchoRadarApp::ClearLatestRadarFrame(RadarRuntimeStatus status) {
    m_latestRadarFrame = {};
    m_latestRadarFrame.layout = m_currentLayout;
    m_latestRadarFrame.streamGeneration = m_currentGeneration;
    m_latestRadarFrame.status = status;
}

void EchoRadarApp::RestartCapture(std::optional<std::string> endpointId) {
    if (!m_audio) return;
    if (endpointId) {
        if (endpointId->empty()) {
            m_config.audio.selection = AudioEndpointSelection::FollowDefault;
            m_config.audio.endpointId.clear();
            AppSettings settings = m_settings->Snapshot();
            settings.audioProfile.outputEndpointId.clear();
            CommitSettings(settings);
        } else {
            m_config.audio.selection = AudioEndpointSelection::Fixed;
            m_config.audio.endpointId = *endpointId;
            AppSettings settings = m_settings->Snapshot();
            settings.audioProfile.outputEndpointId = *endpointId;
            CommitSettings(settings);
        }
    }
    ClearLatestRadarFrame(RadarRuntimeStatus::WaitingForAudio);
    m_audio->Stop();
    m_audio->Start(m_config.audio);
    RefreshDevices();
}

void EchoRadarApp::ProcessUiCommands() {
    if (!m_commands || !m_settings) return;
    for (UiCommand& command : m_commands->Drain(128)) {
        std::visit([&](auto& value) {
            using Command = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Command, StartCaptureCommand> ||
                          std::is_same_v<Command, RetryCaptureCommand>) {
                RestartCapture();
            } else if constexpr (std::is_same_v<Command, StopCaptureCommand>) {
                ClearLatestRadarFrame(RadarRuntimeStatus::WaitingForAudio);
                if (m_audio) m_audio->Stop();
            } else if constexpr (std::is_same_v<Command, SelectEndpointCommand>) {
                RestartCapture(value.endpointId);
            } else if constexpr (std::is_same_v<Command, OpenSoundSettingsCommand>) {
#ifdef _WIN32
                ShellExecuteW(nullptr, L"open", L"ms-settings:sound",
                              nullptr, nullptr, SW_SHOWNORMAL);
#endif
            } else if constexpr (std::is_same_v<Command, SaveSettingsCommand>) {
                if (m_settingsSaver) {
                    m_settingsSaver->RequestSaveNow();
                } else {
                    m_settings->Save(nullptr);
                }
            } else if constexpr (std::is_same_v<Command, DismissErrorCommand>) {
                m_recognitionError.clear();
            } else if constexpr (std::is_same_v<Command, SetRadarModeCommand>) {
                AppSettings settings = m_settings->Snapshot();
                const bool eventPipelineChanged =
                    IsEventMode(settings.radar.mode) != IsEventMode(value.mode);
                settings.radar.mode = value.mode;
                CommitSettings(settings);
                ApplyRadarSettings(settings);
                if (eventPipelineChanged) {
                    FinalizePendingRadarEvents(RadarRuntimeStatus::MalformedInput);
                    if (m_recognizer) {
                        m_recognizer->OnStreamReset(m_currentGeneration);
                    }
                }
                ClearLatestRadarFrame(RadarRuntimeStatus::WaitingForAudio);
            } else if constexpr (std::is_same_v<Command, SetRadarPresetCommand>) {
                AppSettings settings = m_settings->Snapshot();
                settings.radar.preset = value.preset;
                CommitSettings(settings);
                ApplyRadarSettings(settings);
            } else if constexpr (std::is_same_v<Command, SetRadarSensitivityCommand>) {
                AppSettings settings = m_settings->Snapshot();
                settings.radar.sensitivityDbfs = value.sensitivityDbfs;
                CommitSettings(settings);
                ApplyRadarSettings(settings);
            } else if constexpr (std::is_same_v<Command, SetRadarSmoothingCommand>) {
                AppSettings settings = m_settings->Snapshot();
                settings.radar.attackMilliseconds = value.attackMilliseconds;
                settings.radar.releaseMilliseconds = value.releaseMilliseconds;
                settings.radar.holdMilliseconds = value.holdMilliseconds;
                CommitSettings(settings);
                ApplyRadarSettings(settings);
            } else if constexpr (std::is_same_v<Command, SetCustomRadarCurveCommand>) {
                AppSettings settings = m_settings->Snapshot();
                settings.radar.customCurveDb = value.curveDb;
                CommitSettings(settings);
                ApplyRadarSettings(settings);
            } else if constexpr (std::is_same_v<Command, SetHudVisibleCommand>) {
                AppSettings settings = m_settings->Snapshot();
                HudDisplaySettings hud = EffectiveHudSettings(
                    settings, m_activeHudDisplayId);
                hud.visible = value.visible;
                if (UpdateHudDisplaySettings(
                        settings, m_activeHudDisplayId, hud)) {
                    CommitSettings(settings);
                }
            } else if constexpr (
                    std::is_same_v<Command, SetActiveHudDisplayCommand>) {
                const std::string displayId = value.displayId.empty()
                    ? "default" : value.displayId;
                if (displayId != m_activeHudDisplayId) {
                    AppSettings settings = m_settings->Snapshot();
                    bool settingsChanged = false;
                    if (HudDisplaySettings* previous = FindHudDisplaySettings(
                            settings, m_activeHudDisplayId);
                        previous && previous->editMode) {
                        previous->editMode = false;
                        settingsChanged = true;
                    }
                    m_activeHudDisplayId = displayId;
                    if (settingsChanged) CommitSettings(settings);
                }
                if (std::isfinite(value.usableBounds.x) &&
                    std::isfinite(value.usableBounds.y) &&
                    std::isfinite(value.usableBounds.width) &&
                    std::isfinite(value.usableBounds.height) &&
                    value.usableBounds.width > 0.0f &&
                    value.usableBounds.height > 0.0f) {
                    m_activeHudUsableBounds = value.usableBounds;
                }
                if (std::isfinite(value.dpiScale) && value.dpiScale > 0.0f) {
                    m_activeHudDpiScale = std::clamp(value.dpiScale, 0.5f, 4.0f);
                }
            } else if constexpr (std::is_same_v<Command, SetHudEditModeCommand>) {
                AppSettings settings = m_settings->Snapshot();
                const std::string displayId = value.displayId.empty()
                    ? m_activeHudDisplayId : value.displayId;
                HudDisplaySettings hud = EffectiveHudSettings(
                    settings, displayId);
                hud.editMode = value.enabled;
                if (UpdateHudDisplaySettings(settings, displayId, hud)) {
                    CommitSettings(settings);
                }
            } else if constexpr (std::is_same_v<Command, UpdateHudDisplayCommand>) {
                AppSettings settings = m_settings->Snapshot();
                const std::string displayId = value.displayId.empty()
                    ? m_activeHudDisplayId : value.displayId;
                HudDisplaySettings hud = value.settings;
                hud.displayId = displayId;
                if (UpdateHudDisplaySettings(settings, displayId, hud)) {
                    CommitSettings(settings);
                }
            } else if constexpr (std::is_same_v<Command, SetHudOffsetCommand>) {
                AppSettings settings = m_settings->Snapshot();
                const std::string displayId = value.displayId.empty()
                    ? m_activeHudDisplayId : value.displayId;
                HudDisplaySettings hud = EffectiveHudSettings(
                    settings, displayId);
                hud.offsetX = value.offsetX;
                hud.offsetY = value.offsetY;
                if (UpdateHudDisplaySettings(settings, displayId, hud)) {
                    CommitSettings(settings);
                }
            } else if constexpr (
                    std::is_same_v<Command, UpdateDashboardPlacementCommand>) {
                AppSettings settings = m_settings->Snapshot();
                settings.dashboard.windowX = value.windowX;
                settings.dashboard.windowY = value.windowY;
                settings.dashboard.windowWidth = value.windowWidth;
                settings.dashboard.windowHeight = value.windowHeight;
                settings.dashboard.maximized = value.maximized;
                CommitSettings(settings);
            } else if constexpr (
                    std::is_same_v<Command,
                                   SetDashboardSectionCollapsedCommand>) {
                AppSettings settings = m_settings->Snapshot();
                switch (value.section) {
                case DashboardSection::AudioSetup:
                    settings.dashboard.audioSetupCollapsed = value.collapsed;
                    break;
                case DashboardSection::EventDetails:
                    settings.dashboard.eventDetailsCollapsed = value.collapsed;
                    break;
                case DashboardSection::HudEditor:
                    settings.dashboard.hudEditorCollapsed = value.collapsed;
                    break;
                case DashboardSection::Advanced:
                    settings.dashboard.advancedCollapsed = value.collapsed;
                    break;
                }
                CommitSettings(settings);
            } else if constexpr (std::is_same_v<Command, UpdateEventDisplayCommand>) {
                AppSettings settings = m_settings->Snapshot();
                settings.eventDisplay = value.settings;
                CommitSettings(settings);
                const size_t maximum = std::clamp<size_t>(
                    value.settings.maximumRecentEvents, 1u, 500u);
                if (m_recentEvents.size() > maximum) {
                    m_recentEvents.erase(
                        m_recentEvents.begin(),
                        m_recentEvents.begin() +
                            static_cast<std::ptrdiff_t>(
                                m_recentEvents.size() - maximum));
                }
            } else if constexpr (std::is_same_v<Command, ApplySetupEventCommand>) {
                AppSettings settings = m_settings->Snapshot();
                settings.onboarding = ApplySetupEvent(
                    settings.onboarding, value.event).state;
                CommitSettings(settings);
            } else if constexpr (std::is_same_v<Command, SetRecordingCommand>) {
                if (value.enabled && !m_recording) {
                    m_recording = true;
                    BeginRecording(m_currentLayout);
                } else if (!value.enabled && m_recording) {
                    m_recording = false;
                    FlushRecording();
                }
            } else if constexpr (
                    std::is_same_v<Command, SetSessionLoggingCommand>) {
                AppSettings settings = m_settings->Snapshot();
                if (!value.enabled) {
                    m_sessionLog.Close();
                    m_sessionLoggingError.clear();
                    settings.sessionLogging = false;
                    CommitSettings(settings);
                } else if (m_sessionDirectory.empty()) {
                    m_sessionLoggingError =
                        "Session logging is unavailable because the session directory could not be created.";
                    settings.sessionLogging = false;
                    CommitSettings(settings);
                } else {
                    std::string error;
                    const bool opened = m_sessionLog.IsHealthy() ||
                        m_sessionLog.Open(
                            m_sessionDirectory / "session.jsonl", &error);
                    if (!opened) {
                        m_sessionLoggingError = error;
                        settings.sessionLogging = false;
                        CommitSettings(settings);
                        std::cerr << "[EchoRadar] " << error << '\n';
                    } else {
                        m_sessionLoggingError.clear();
                        settings.sessionLogging = true;
                        CommitSettings(settings);
                    }
                    if (opened && m_audio) {
                        const AudioCaptureStatus current = m_audio->GetStatus();
                        m_sessionLog.WriteStreamStatus(
                            current, "session logging enabled");
                    }
                }
            } else if constexpr (
                    std::is_same_v<Command, UpdateRecognitionTuningCommand>) {
                if (m_runtimeTuning) {
                    m_runtimeTuning->Update(value.tuning);
                }
            }
        }, command);
    }
}

void EchoRadarApp::BeginRecording(const AudioChannelLayout& layout) {
    m_recordingWriter.reset();
    m_recordingPath.clear();
    if (!m_recording) return;
    if (m_sessionDirectory.empty()) {
        std::cerr << "[EchoRadar] Recording is unavailable because the session "
                     "directory could not be created.\n";
        m_recording = false;
        return;
    }
    // Recording can be armed while capture is still starting; the first valid
    // block will call this method again with its exact native channel layout.
    if (!layout.IsValid()) return;
    m_recordingPath = m_sessionDirectory /
        ("capture-" + std::to_string(m_currentGeneration) + "-" +
         std::to_string(++m_recordingSequence) + ".wav");
    auto writer = std::make_unique<Pcm16WavWriter>();
    const Pcm16WavWriterConfig config{
        48000,
        static_cast<uint16_t>(layout.channelCount),
        layout.channelMask,
        0,
    };
    std::string error;
    if (!writer->Open(m_recordingPath, config, &error)) {
        std::cerr << "[EchoRadar] Could not start recording: "
                  << error << '\n';
        m_recordingPath.clear();
        m_recording = false;
        return;
    }
    m_recordingWriter = std::move(writer);
}

void EchoRadarApp::AppendRecording(std::span<const float> samples,
                                   size_t frameCount,
    const AudioChannelLayout& layout) {
    if (!m_recording) return;
    if (!m_recordingWriter) BeginRecording(layout);
    if (!m_recordingWriter || !m_recording) return;
    if (m_recordingWriter->Channels() != layout.channelCount ||
        m_recordingWriter->ChannelMask() != layout.channelMask) {
        FlushRecording();
        BeginRecording(layout);
    }
    if (!m_recordingWriter || !m_recording) return;
    if (layout.channelCount == 0 ||
        frameCount > samples.size() / layout.channelCount) {
        std::cerr << "[EchoRadar] Recording input is not frame-aligned.\n";
        m_recording = false;
        FlushRecording();
        return;
    }
    const size_t sampleCount = frameCount * layout.channelCount;
    std::string error;
    if (!m_recordingWriter->AppendFrames(samples.first(sampleCount),
                                          frameCount, &error)) {
        std::cerr << "[EchoRadar] Could not append recording: "
                  << error << '\n';
        m_recording = false;
        FlushRecording();
    }
}

void EchoRadarApp::FlushRecording() {
    if (!m_recordingWriter) {
        m_recordingPath.clear();
        return;
    }
    const uint64_t frameCount = m_recordingWriter->FrameCount();
    std::string error;
    if (!m_recordingWriter->Close(&error)) {
        std::cerr << "[EchoRadar] Could not finalize recording: "
                  << error << '\n';
    } else {
        std::cout << "[EchoRadar] Wrote " << m_recordingPath.string()
                  << " (" << frameCount << " frames)\n";
    }
    m_recordingWriter.reset();
    m_recordingPath.clear();
}

void EchoRadarApp::RefreshDevices() {
    if (m_deviceManager) m_deviceManager->Refresh();
}

void EchoRadarApp::PublishSnapshot(const AudioCaptureStatus& status,
                                   const AudioLevels& levels) {
    if (!m_snapshots || !m_settings) return;
    AppSnapshot snapshot;
    snapshot.revision = ++m_snapshotRevision;
    snapshot.timestampSeconds = static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.0;
    snapshot.capture.state = status.state;
    snapshot.capture.endpointId = status.endpointId;
    snapshot.capture.endpointName = status.endpointName;
    // The dashboard validates the endpoint format, not miniaudio's resampled
    // processing format. A native 44.1 kHz endpoint must never appear eligible
    // merely because the capture client requested 48 kHz output.
    snapshot.capture.sampleRate = status.nativeSampleRate;
    snapshot.capture.levels = levels;
    snapshot.capture.streamGeneration = status.streamGeneration;
    snapshot.capture.streamSample = m_currentAudioSample;
    snapshot.capture.droppedFrames = status.droppedFrames;
    snapshot.capture.discardedBacklogFrames = status.discardedBacklogFrames;
    snapshot.capture.restartCount = status.restartCount;
    const auto nativeLayout = NativeLayout(status);
    snapshot.layout.detected = nativeLayout.value_or(status.layout);
    snapshot.layout.directionalRadarAvailable =
        IsNativeRadarFormat(status, status.layout);
    if (status.nativeSampleRate == 0) {
        snapshot.layout.statusText =
            "Waiting for the endpoint's native audio format.";
    } else if (status.nativeSampleRate != 48000) {
        snapshot.layout.statusText =
            "Directional radar requires a native 48 kHz endpoint.";
    } else if (!nativeLayout) {
        snapshot.layout.statusText =
            "The endpoint has no validated native Windows speaker mask.";
    } else if (nativeLayout->kind == AudioChannelLayoutKind::Stereo) {
        snapshot.layout.statusText =
            "Stereo is available to recognition but unsupported for directional radar.";
    } else if (!nativeLayout->IsDirectional()) {
        snapshot.layout.statusText =
            "Select a native 5.1 or 7.1 layout with a valid Windows speaker mask.";
    } else if (status.layout != *nativeLayout) {
        snapshot.layout.statusText =
            "Capture channel conversion is active; native roles are required for radar.";
    } else {
        snapshot.layout.statusText = "Native surround layout is ready.";
    }

    snapshot.settings = m_settings->Snapshot();
    snapshot.radar.mode = snapshot.settings.radar.mode;
    snapshot.radar.preset = snapshot.settings.radar.preset;
    if (status.state == AudioCaptureState::Running) {
        snapshot.radar.frame = m_latestRadarFrame;
        snapshot.radar.state = m_latestRadarFrame.status;
    } else {
        snapshot.radar.frame = {};
        snapshot.radar.frame.layout = snapshot.layout.detected;
        snapshot.radar.frame.sampleRate = status.nativeSampleRate;
        snapshot.radar.frame.streamGeneration = status.streamGeneration;
        snapshot.radar.frame.status = RadarRuntimeStatus::WaitingForAudio;
        snapshot.radar.state = RadarRuntimeStatus::WaitingForAudio;
    }
    snapshot.model.state = m_modelState;
    snapshot.model.name = m_config.modelDirectory.filename().string();
    snapshot.model.version = m_modelVersion;
    snapshot.model.statusText = m_modelStatus;
    if (m_runtimeTuning) {
        snapshot.recognitionTuning = m_runtimeTuning->Snapshot();
    }
    snapshot.recentEvents = m_recentEvents;
    snapshot.recording = m_recording;
    snapshot.sessionLoggingActive = m_sessionLog.IsHealthy();
    snapshot.sessionLoggingError = m_sessionLoggingError;
    if (m_deviceManager) snapshot.outputDevices = m_deviceManager->GetOutputDevices();

    snapshot.hud.displayId = m_activeHudDisplayId;
    const HudDisplaySettings hud = EffectiveHudSettings(
        snapshot.settings, snapshot.hud.displayId);
    snapshot.hud.state = !hud.visible ? HudUiState::Hidden
        : (hud.editMode ? HudUiState::Editing : HudUiState::Visible);
    snapshot.hud.clickThrough = !hud.editMode;
    snapshot.hud.bounds = ResolveHudBounds(
        hud, m_activeHudUsableBounds,
        300.0f * m_activeHudDpiScale,
        300.0f * m_activeHudDpiScale);

    if (status.state == AudioCaptureState::Recovering) {
        snapshot.error = {InlineErrorKind::RecoveringDevice,
                          "capture-recovering",
                          "The playback endpoint is recovering.",
                          status.lastError, true};
        snapshot.recovery.state = RecoveryUiState::RestartingCapture;
        snapshot.recovery.attempt = static_cast<uint32_t>(status.restartCount + 1u);
        snapshot.recovery.statusText = status.lastError;
    } else if (status.state == AudioCaptureState::Failed) {
        snapshot.error = {InlineErrorKind::CaptureFailure,
                          "capture-failed",
                          "System-output capture failed.",
                          status.lastError, true};
    } else if (!snapshot.layout.directionalRadarAvailable) {
        snapshot.error = {
            status.nativeSampleRate != 48000
                ? InlineErrorKind::UnsupportedSampleRate
                : InlineErrorKind::UnsupportedLayout,
            "unsupported-audio",
            snapshot.layout.statusText,
            "Choose a 48 kHz 5.1/7.1 format in Windows sound settings.",
            false};
    } else if (status.state == AudioCaptureState::Running &&
               snapshot.radar.state == RadarRuntimeStatus::Silent) {
        snapshot.error = {InlineErrorKind::SilentInput,
                          "silent-input",
                          "No directional energy is above the current sensitivity.",
                          "Play surround content or lower sensitivity.", false};
    } else if (IsEventMode(snapshot.settings.radar.mode) &&
               (m_modelState == ModelUiState::Missing ||
                m_modelState == ModelUiState::Disabled)) {
        snapshot.error = {InlineErrorKind::MissingModel,
                          "model-missing",
                          "Event radar needs a recognition model; continuous radar remains available.",
                          "Start with --model <package-directory>.", false};
    } else if (m_modelState == ModelUiState::Malformed ||
               m_modelState == ModelUiState::Failed) {
        snapshot.error = {InlineErrorKind::MalformedModel,
                          "model-invalid",
                          "Recognition is paused because the model package is invalid.",
                          m_recognitionError, false};
    }
    m_snapshots->Publish(std::move(snapshot));
}

void EchoRadarApp::DSPLoop() {
    std::vector<float> surround(kChunkFrames * kMaxAudioChannels, 0.0f);
    std::vector<float> stereo(kChunkFrames * 2u, 0.0f);
    auto nextSnapshot = std::chrono::steady_clock::now();

    while (!m_stop.load(std::memory_order_acquire)) {
        ProcessUiCommands();
        m_audio->Poll();
        const AudioReadResult read = m_audio->Read(surround.data(), kChunkFrames);
        const AudioCaptureStatus status = m_audio->GetStatus();
        const bool captureRunning =
            status.state == AudioCaptureState::Running;
        const bool directionalRadarAvailable =
            IsNativeRadarFormat(status, read.layout);
        const bool eventModeEnabled =
            IsEventMode(m_settings->Snapshot().radar.mode);
        if (!captureRunning) {
            ClearLatestRadarFrame(RadarRuntimeStatus::WaitingForAudio);
        }
        if (status.state != m_lastLoggedCaptureState ||
            status.streamGeneration != m_lastLoggedGeneration) {
            m_lastLoggedCaptureState = status.state;
            m_lastLoggedGeneration = status.streamGeneration;
            m_sessionLog.WriteStreamStatus(status, status.lastError);
        }

        if (read.discontinuity || read.layoutChanged ||
            read.streamGeneration != m_currentGeneration ||
            read.layout != m_currentLayout) {
            ResetPipelines(read);
        }
        if (read.frames != 0) {
            m_currentAudioSample = read.firstSample + read.frames;
            const size_t sampleCount = read.frames * read.layout.channelCount;
            const SurroundAudioBlockView surroundBlock{
                std::span<const float>(surround.data(), sampleCount),
                read.frames,
                status.sampleRate,
                read.layout,
                read.firstSample,
                read.streamGeneration,
                read.discontinuity,
            };
            if (captureRunning && directionalRadarAvailable &&
                m_surroundHistory) {
                m_surroundHistory->PushInterleaved(
                    surround.data(), read.frames, read.firstSample);
            }
            if (captureRunning && directionalRadarAvailable && m_radar) {
                m_radar->PushAudio(surroundBlock);
                RadarFrame frame;
                while (m_radar->PopFrame(frame)) {
                    m_latestRadarFrame = frame;
                    m_sessionLog.WriteRadarFrame(frame);
                }
            } else if (captureRunning) {
                m_latestRadarFrame = {};
                m_latestRadarFrame.layout = read.layout;
                m_latestRadarFrame.sampleRate = status.nativeSampleRate;
                m_latestRadarFrame.streamGeneration = read.streamGeneration;
                m_latestRadarFrame.status = status.nativeSampleRate == 48000
                    ? RadarRuntimeStatus::UnsupportedLayout
                    : RadarRuntimeStatus::UnsupportedSampleRate;
            }
            if (captureRunning) {
                AppendRecording(
                    surroundBlock.interleaved, read.frames, read.layout);
            }

            if (captureRunning && eventModeEnabled && m_recognizer &&
                DownmixForRecognition(
                    surroundBlock.interleaved, read.frames, read.layout,
                    std::span<float>(stereo.data(), read.frames * 2u))) {
                const AudioBlockView stereoBlock{
                    std::span<const float>(stereo.data(), read.frames * 2u),
                    read.frames,
                    48000,
                    2,
                    read.firstSample,
                    read.streamGeneration,
                };
                m_recognizer->OnAudio(stereoBlock);
                if (!m_recognizer->LastError().empty()) {
                    m_recognitionError = m_recognizer->LastError();
                    m_modelState = ModelUiState::Failed;
                    m_modelStatus = m_recognitionError;
                    m_recognizer.reset();
                }
            }
            if (captureRunning && eventModeEnabled &&
                directionalRadarAvailable) {
                ProcessPendingRadarEvents();
            } else if (captureRunning && eventModeEnabled &&
                       !m_pendingEvents.empty()) {
                // Recognition remains available on a stereo/resampled stream,
                // but v2 must not manufacture directional event results from it.
                FinalizePendingRadarEvents(
                    status.nativeSampleRate == 48000
                        ? RadarRuntimeStatus::UnsupportedLayout
                        : RadarRuntimeStatus::UnsupportedSampleRate);
            } else if (!eventModeEnabled && !m_pendingEvents.empty()) {
                FinalizePendingRadarEvents(RadarRuntimeStatus::MalformedInput);
            }
        }
        if (!captureRunning && !m_pendingEvents.empty()) {
            FinalizePendingRadarEvents(RadarRuntimeStatus::WaitingForAudio);
        }

        if (m_settings->Snapshot().sessionLogging &&
            !m_sessionLog.IsHealthy()) {
            m_sessionLoggingError =
                "The schema-3 session log stopped accepting writes; logging was disabled.";
            AppSettings settings = m_settings->Snapshot();
            settings.sessionLogging = false;
            CommitSettings(settings);
            m_sessionLog.Close();
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= nextSnapshot) {
            PublishSnapshot(status, m_audio->GetCurrentLevels());
            nextSnapshot = now + std::chrono::milliseconds(33);
        }
        if (read.frames == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    // A renderer can enqueue its last edit in the same iteration that closes
    // the app. Apply every remaining value command before the save worker is
    // flushed so the debounce never loses a shutdown-edge change.
    while (m_commands && !m_commands->Empty()) {
        ProcessUiCommands();
    }
    if (m_recognizer) m_recognizer->Flush();
    ProcessPendingRadarEvents();
    FinalizePendingRadarEvents(RadarRuntimeStatus::MalformedInput);
    FlushRecording();
    m_audio->Stop();
}

} // namespace EchoRadar
