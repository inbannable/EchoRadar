#pragma once

#include <audio/AudioCapture.h>
#include <audio/AudioDeviceInfo.h>
#include <audio/AudioTypes.h>
#include <audio/SignalActivity.h>
#include <radar/RadarTypes.h>
#include <recognition/RecognitionTypes.h>
#include <recognition/RecognitionRuntimeConfig.h>
#include <settings/AppSettings.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace EchoRadar {

enum class ModelUiState : uint8_t {
    Disabled,
    Loading,
    Ready,
    Missing,
    Malformed,
    Failed,
};

enum class HudUiState : uint8_t {
    Hidden,
    Visible,
    Editing,
    Failed,
};

enum class RecoveryUiState : uint8_t {
    None,
    RestartingCapture,
    ReloadingModel,
    RebuildingRadar,
};

enum class InlineErrorKind : uint8_t {
    None,
    Loading,
    SilentInput,
    MissingModel,
    UnsupportedLayout,
    UnsupportedSampleRate,
    RecoveringDevice,
    CaptureFailure,
    MalformedModel,
};

struct CaptureSnapshot {
    AudioCaptureState state{AudioCaptureState::Stopped};
    std::string endpointId;
    std::string endpointName;
    uint32_t sampleRate{0};
    AudioLevels levels{};
    bool audioFresh{false};
    SignalActivitySnapshot signal{};
    uint32_t processingSampleRate{48000};
    uint64_t streamGeneration{0};
    uint64_t streamSample{0};
    uint64_t droppedFrames{0};
    uint64_t discardedBacklogFrames{0};
    uint64_t restartCount{0};
};

struct LayoutSnapshot {
    AudioChannelLayout detected{};
    bool directionalRadarAvailable{false};
    std::string statusText;
};

struct RadarSnapshot {
    RadarMode mode{RadarMode::Continuous};
    RadarPreset preset{RadarPreset::All};
    RadarRuntimeStatus state{RadarRuntimeStatus::WaitingForAudio};
    RadarFrame frame{};
};

struct ModelSnapshot {
    ModelUiState state{ModelUiState::Disabled};
    std::string name;
    std::string version;
    std::string statusText;
    uint64_t inferenceCount{0};
    uint64_t suppressedEvents{0};
    std::array<float, kSoundClassCount> probabilities{};
};

struct HudSnapshot {
    HudUiState state{HudUiState::Hidden};
    std::string displayId;
    HudRect bounds{};
    bool clickThrough{true};
};

struct InlineErrorSnapshot {
    InlineErrorKind kind{InlineErrorKind::None};
    std::string code;
    std::string message;
    std::string recoveryAction;
    bool recoverable{false};

    explicit operator bool() const noexcept {
        return kind != InlineErrorKind::None;
    }
};

struct RecoverySnapshot {
    RecoveryUiState state{RecoveryUiState::None};
    uint32_t attempt{0};
    std::string statusText;
};

struct RecentEventSnapshot {
    uint64_t eventId{0};
    uint64_t streamGeneration{0};
    double timestampSeconds{0.0};
    SoundClass soundClass{SoundClass::Gunshot};
    float confidence{0.0f};
    bool suppressed{false};
    bool liveMarkerEligible{true};
    float strongestAzimuthDegrees{0.0f};
    std::array<float, kRadarSectorCount> miniatureRadarDbfs{};
    uint32_t peakCount{0};
    std::array<RadarPeak, kRadarMaximumPeaks> peaks{};
};

// A complete value object assembled by the runtime. UI readers only receive a
// shared_ptr<const AppSnapshot>, so a frame is internally consistent and cannot
// be modified after publication.
struct AppSnapshot {
    uint64_t revision{0};
    double timestampSeconds{0.0};
    CaptureSnapshot capture{};
    LayoutSnapshot layout{};
    RadarSnapshot radar{};
    ModelSnapshot model{};
    HudSnapshot hud{};
    InlineErrorSnapshot error{};
    RecoverySnapshot recovery{};
    RecognitionRuntimeTuning recognitionTuning{};
    std::vector<RecentEventSnapshot> recentEvents;
    AppSettings settings{};
    std::vector<AudioDeviceInfo> outputDevices;
    bool recording{false};
    bool sessionLoggingActive{false};
    std::string sessionLoggingError;
};

class LatestSnapshotPublisher {
public:
    LatestSnapshotPublisher();
    explicit LatestSnapshotPublisher(AppSnapshot initial);

    void Publish(AppSnapshot snapshot);
    void Publish(std::shared_ptr<const AppSnapshot> snapshot);
    std::shared_ptr<const AppSnapshot> Latest() const noexcept;

private:
    std::shared_ptr<const AppSnapshot> m_latest;
};

using AppSnapshotPublisher = LatestSnapshotPublisher;

struct StartCaptureCommand {};
struct StopCaptureCommand {};
struct RetryCaptureCommand {};
struct OpenSoundSettingsCommand {};
struct DismissErrorCommand {};
struct SaveSettingsCommand {};

struct SelectEndpointCommand {
    std::string endpointId;
};

struct SetRadarModeCommand {
    RadarMode mode{RadarMode::Continuous};
};

struct SetRadarPresetCommand {
    RadarPreset preset{RadarPreset::All};
};

struct SetRadarSensitivityCommand {
    float sensitivityDbfs{-48.0f};
};

struct SetRadarSmoothingCommand {
    uint32_t attackMilliseconds{30};
    uint32_t releaseMilliseconds{250};
    uint32_t holdMilliseconds{300};
};

struct SetCustomRadarCurveCommand {
    std::array<float, kRadarCustomCurvePointCount> curveDb{};
};

struct SetHudVisibleCommand {
    bool visible{true};
};

// Emitted by the HUD renderer whenever its target monitor changes. Keeping the
// usable work area with the stable display identifier lets the runtime publish
// the same per-display record that the dashboard edits.
struct SetActiveHudDisplayCommand {
    std::string displayId{"default"};
    HudRect usableBounds{};
    float dpiScale{1.0f};
};

struct SetHudEditModeCommand {
    std::string displayId;
    bool enabled{false};
};

struct UpdateHudDisplayCommand {
    std::string displayId;
    HudDisplaySettings settings;
};

struct SetHudOffsetCommand {
    std::string displayId;
    float offsetX{0.0f};
    float offsetY{0.0f};
};

struct SetRecordingCommand {
    bool enabled{false};
};

struct SetSessionLoggingCommand {
    bool enabled{true};
};

struct UpdateRecognitionTuningCommand {
    RecognitionRuntimeTuning tuning{};
};

struct UpdateDashboardPlacementCommand {
    float windowX{100.0f};
    float windowY{100.0f};
    float windowWidth{1280.0f};
    float windowHeight{800.0f};
    bool maximized{false};
};

enum class DashboardSection : uint8_t {
    AudioSetup,
    EventDetails,
    HudEditor,
    Advanced,
};

struct SetDashboardSectionCollapsedCommand {
    DashboardSection section{DashboardSection::AudioSetup};
    bool collapsed{false};
};

struct UpdateEventDisplayCommand {
    EventDisplaySettings settings;
};

enum class SetupEvent : uint8_t {
    EndpointSelected,
    EndpointCleared,
    FormatSupported,
    FormatUnsupportedStereo,
    FormatUnsupportedSampleRate,
    FormatUnsupportedChannelMask,
    ChannelActivityConfirmed,
    HudPreviewConfirmed,
    CaptureFailed,
    Back,
    Retry,
    Reset,
    HeadphoneFormatSupported,
};

struct ApplySetupEventCommand {
    SetupEvent event{SetupEvent::Reset};
};

using UiCommand = std::variant<
    StartCaptureCommand,
    StopCaptureCommand,
    RetryCaptureCommand,
    OpenSoundSettingsCommand,
    DismissErrorCommand,
    SaveSettingsCommand,
    SelectEndpointCommand,
    SetRadarModeCommand,
    SetRadarPresetCommand,
    SetRadarSensitivityCommand,
    SetRadarSmoothingCommand,
    SetCustomRadarCurveCommand,
    SetHudVisibleCommand,
    SetActiveHudDisplayCommand,
    SetHudEditModeCommand,
    UpdateHudDisplayCommand,
    SetHudOffsetCommand,
    SetRecordingCommand,
    SetSessionLoggingCommand,
    UpdateRecognitionTuningCommand,
    UpdateDashboardPlacementCommand,
    SetDashboardSectionCollapsedCommand,
    UpdateEventDisplayCommand,
    ApplySetupEventCommand>;

class UiCommandQueue {
public:
    void Push(UiCommand command);

    template <typename Command, typename... Args>
    void Emplace(Args&&... args) {
        Push(UiCommand{
            std::in_place_type<Command>,
            std::forward<Args>(args)...});
    }

    bool TryPop(UiCommand& command);
    std::vector<UiCommand> Drain(
        size_t maximumCount = static_cast<size_t>(-1));
    size_t Size() const;
    bool Empty() const;

private:
    mutable std::mutex m_mutex;
    std::deque<UiCommand> m_commands;
};

enum class SetupTransitionStatus : uint8_t {
    Applied,
    Blocked,
    Unsupported,
};

struct SetupTransition {
    OnboardingSettings state{};
    SetupTransitionStatus status{SetupTransitionStatus::Applied};
};

struct SetupFeatureAvailability {
    bool directionalRadar{false};
    bool recognitionAccessible{true};
    bool settingsAccessible{true};
    bool shouldOfferSoundSettings{false};
    std::string explanation;
};

SetupTransition ApplySetupEvent(OnboardingSettings state, SetupEvent event);
SetupFeatureAvailability SetupAvailability(SetupSupportState state);
bool IsDirectionalRadarSupported(SetupSupportState state) noexcept;

} // namespace EchoRadar
