#include "AppState.h"

#include <algorithm>
#include <utility>

namespace EchoRadar {
namespace {

// UiCommand is a header-defined variant; keeping the queue implementation in
// this translation unit gives incremental builds one ABI anchor to rebuild.

SetupTransition Blocked(OnboardingSettings state) {
    return {std::move(state), SetupTransitionStatus::Blocked};
}

SetupTransition Unsupported(OnboardingSettings state) {
    return {std::move(state), SetupTransitionStatus::Unsupported};
}

void ClearAfterEndpoint(OnboardingSettings& state) {
    state.formatValidated = false;
    state.channelsConfirmed = false;
    state.hudPreviewed = false;
    state.completed = false;
}

void ClearAfterFormat(OnboardingSettings& state) {
    state.channelsConfirmed = false;
    state.hudPreviewed = false;
    state.completed = false;
}

} // namespace

LatestSnapshotPublisher::LatestSnapshotPublisher()
    : m_latest(std::make_shared<const AppSnapshot>()) {}

LatestSnapshotPublisher::LatestSnapshotPublisher(AppSnapshot initial)
    : m_latest(std::make_shared<const AppSnapshot>(std::move(initial))) {}

void LatestSnapshotPublisher::Publish(AppSnapshot snapshot) {
    Publish(std::make_shared<const AppSnapshot>(std::move(snapshot)));
}

void LatestSnapshotPublisher::Publish(
    std::shared_ptr<const AppSnapshot> snapshot) {
    if (!snapshot) snapshot = std::make_shared<const AppSnapshot>();
    std::atomic_store_explicit(&m_latest, std::move(snapshot), std::memory_order_release);
}

std::shared_ptr<const AppSnapshot>
LatestSnapshotPublisher::Latest() const noexcept {
    return std::atomic_load_explicit(&m_latest, std::memory_order_acquire);
}

void UiCommandQueue::Push(UiCommand command) {
    // The variant owns every payload, including monitor identifiers and bounds;
    // no renderer-owned storage crosses the thread boundary.
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back(std::move(command));
}

bool UiCommandQueue::TryPop(UiCommand& command) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_commands.empty()) return false;
    command = std::move(m_commands.front());
    m_commands.pop_front();
    return true;
}

std::vector<UiCommand> UiCommandQueue::Drain(size_t maximumCount) {
    std::vector<UiCommand> result;
    std::lock_guard<std::mutex> lock(m_mutex);
    const size_t count = std::min(maximumCount, m_commands.size());
    result.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        result.push_back(std::move(m_commands.front()));
        m_commands.pop_front();
    }
    return result;
}

size_t UiCommandQueue::Size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_commands.size();
}

bool UiCommandQueue::Empty() const {
    return Size() == 0;
}

SetupTransition ApplySetupEvent(OnboardingSettings state,
                                SetupEvent event) {
    switch (event) {
    case SetupEvent::EndpointSelected:
        state.endpointSelected = true;
        ClearAfterEndpoint(state);
        state.supportState = SetupSupportState::Unknown;
        state.step = SetupStep::ValidateFormat;
        break;

    case SetupEvent::EndpointCleared:
        state = {};
        break;

    case SetupEvent::FormatSupported:
    case SetupEvent::HeadphoneFormatSupported:
        if (!state.endpointSelected) return Blocked(std::move(state));
        state.formatValidated = true;
        ClearAfterFormat(state);
        state.supportState = event == SetupEvent::HeadphoneFormatSupported
            ? SetupSupportState::HeadphoneStereo : SetupSupportState::Supported;
        state.step = SetupStep::ConfirmChannels;
        break;

    case SetupEvent::FormatUnsupportedStereo:
    case SetupEvent::FormatUnsupportedSampleRate:
    case SetupEvent::FormatUnsupportedChannelMask:
        if (!state.endpointSelected) return Blocked(std::move(state));
        ClearAfterEndpoint(state);
        state.supportState =
            event == SetupEvent::FormatUnsupportedStereo
                ? SetupSupportState::UnsupportedStereo
                : (event == SetupEvent::FormatUnsupportedSampleRate
                    ? SetupSupportState::UnsupportedSampleRate
                    : SetupSupportState::UnsupportedChannelMask);
        state.step = SetupStep::ValidateFormat;
        return Unsupported(std::move(state));

    case SetupEvent::ChannelActivityConfirmed:
        if (!state.endpointSelected || !state.formatValidated ||
            (state.supportState != SetupSupportState::Supported &&
             state.supportState != SetupSupportState::HeadphoneStereo)) {
            return Blocked(std::move(state));
        }
        state.channelsConfirmed = true;
        state.hudPreviewed = false;
        state.completed = false;
        state.step = SetupStep::PreviewHud;
        break;

    case SetupEvent::HudPreviewConfirmed:
        if (!state.endpointSelected || !state.formatValidated ||
            !state.channelsConfirmed ||
            (state.supportState != SetupSupportState::Supported &&
             state.supportState != SetupSupportState::HeadphoneStereo)) {
            return Blocked(std::move(state));
        }
        state.hudPreviewed = true;
        state.completed = true;
        state.step = SetupStep::Complete;
        break;

    case SetupEvent::CaptureFailed:
        ClearAfterEndpoint(state);
        state.supportState = SetupSupportState::CaptureFailure;
        state.step = state.endpointSelected
            ? SetupStep::ValidateFormat
            : SetupStep::SelectEndpoint;
        return Unsupported(std::move(state));

    case SetupEvent::Back:
        switch (state.step) {
        case SetupStep::SelectEndpoint:
            return Blocked(std::move(state));
        case SetupStep::ValidateFormat:
            state.endpointSelected = false;
            ClearAfterEndpoint(state);
            state.supportState = SetupSupportState::Unknown;
            state.step = SetupStep::SelectEndpoint;
            break;
        case SetupStep::ConfirmChannels:
            state.formatValidated = false;
            ClearAfterFormat(state);
            state.supportState = SetupSupportState::Unknown;
            state.step = SetupStep::ValidateFormat;
            break;
        case SetupStep::PreviewHud:
            state.channelsConfirmed = false;
            state.hudPreviewed = false;
            state.completed = false;
            state.step = SetupStep::ConfirmChannels;
            break;
        case SetupStep::Complete:
            state.hudPreviewed = false;
            state.completed = false;
            state.step = SetupStep::PreviewHud;
            break;
        }
        break;

    case SetupEvent::Retry:
        ClearAfterEndpoint(state);
        state.supportState = SetupSupportState::Unknown;
        state.step = state.endpointSelected
            ? SetupStep::ValidateFormat
            : SetupStep::SelectEndpoint;
        break;

    case SetupEvent::Reset:
        state = {};
        break;
    }
    return {std::move(state), SetupTransitionStatus::Applied};
}

bool IsDirectionalRadarSupported(SetupSupportState state) noexcept {
    return state == SetupSupportState::Supported;
}

SetupFeatureAvailability SetupAvailability(SetupSupportState state) {
    SetupFeatureAvailability result;
    result.directionalRadar = IsDirectionalRadarSupported(state);
    switch (state) {
    case SetupSupportState::Unknown:
        result.explanation =
            "Validate the playback format before starting directional radar.";
        break;
    case SetupSupportState::Supported:
        result.explanation =
            "Discrete surround channels are ready for directional radar.";
        break;
    case SetupSupportState::HeadphoneStereo:
        result.explanation = "Headphone capture and recognition are ready; full azimuth is unavailable.";
        break;
    case SetupSupportState::NoEndpoint:
        result.shouldOfferSoundSettings = true;
        result.explanation =
            "Select a Windows playback endpoint. Recognition and settings "
            "remain accessible.";
        break;
    case SetupSupportState::UnsupportedStereo:
        result.shouldOfferSoundSettings = true;
        result.explanation =
            "Directional radar requires native 5.1 or 7.1 audio; stereo is "
            "not approximated. Recognition and settings remain accessible.";
        break;
    case SetupSupportState::UnsupportedSampleRate:
        result.shouldOfferSoundSettings = true;
        result.explanation =
            "Directional radar requires 48 kHz audio. Recognition and "
            "settings remain accessible.";
        break;
    case SetupSupportState::UnsupportedChannelMask:
        result.shouldOfferSoundSettings = true;
        result.explanation =
            "The channel mask is not a supported native 5.1 or 7.1 layout. "
            "Recognition and settings remain accessible.";
        break;
    case SetupSupportState::CaptureFailure:
        result.explanation =
            "Audio capture failed. Retry capture or select another endpoint; "
            "settings remain accessible.";
        break;
    }
    return result;
}

} // namespace EchoRadar
