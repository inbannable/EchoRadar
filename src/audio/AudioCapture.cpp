#include "AudioCapture.h"
#include "AudioRingBuffer.h"
#include "miniaudio.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace EchoRadar {
namespace {

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

std::string FingerprintDeviceId(const ma_device_id& id) {
    constexpr uint64_t kOffset = 1469598103934665603ull;
    constexpr uint64_t kPrime = 1099511628211ull;
    uint64_t hash = kOffset;
    const auto* bytes = reinterpret_cast<const unsigned char*>(&id);
    for (size_t index = 0; index < sizeof(id); ++index) {
        hash ^= bytes[index];
        hash *= kPrime;
    }
    std::ostringstream out;
    out << "ma:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

struct EndpointMatch {
    AudioEndpointSelection selection{AudioEndpointSelection::FollowDefault};
    std::string requestedId;
    std::string requestedName;
    ma_device_id id{};
    AudioDeviceInfo info;
    bool found{false};
};

ma_bool32 FindEndpointCallback(ma_context*, ma_device_type type,
                               const ma_device_info* nativeInfo, void* userData) {
    auto& match = *static_cast<EndpointMatch*>(userData);
    if (type != ma_device_type_playback) return MA_TRUE;

    AudioDeviceInfo info;
    info.id = FingerprintDeviceId(nativeInfo->id);
    info.name = nativeInfo->name;
    info.isDefault = nativeInfo->isDefault != 0;
    if (nativeInfo->nativeDataFormatCount != 0) {
        info.nativeChannels = nativeInfo->nativeDataFormats[0].channels;
        info.nativeSampleRate = nativeInfo->nativeDataFormats[0].sampleRate;
    }

    bool selected = false;
    if (match.selection == AudioEndpointSelection::Fixed) {
        selected = (!match.requestedId.empty() && info.id == match.requestedId) ||
            (!match.requestedName.empty() &&
             Lower(info.name).find(Lower(match.requestedName)) != std::string::npos);
    } else {
        selected = info.isDefault || !match.found;
    }

    if (selected) {
        match.id = nativeInfo->id;
        match.info = std::move(info);
        match.found = true;
    }
    return match.selection == AudioEndpointSelection::Fixed && match.found ? MA_FALSE : MA_TRUE;
}

bool FindEndpoint(ma_context& context, const AudioCaptureConfig& config,
                  ma_device_id& id, AudioDeviceInfo& info) {
    EndpointMatch match;
    match.selection = config.selection;
    match.requestedId = config.endpointId;
    match.requestedName = config.endpointName;
    ma_context_enumerate_devices(&context, FindEndpointCallback, &match);
    if (!match.found) return false;
    id = match.id;
    info = std::move(match.info);
    return true;
}

} // namespace

struct AudioCapture::Impl {
    ma_context context{};
    ma_device device{};
    bool contextInitialized{false};
    bool deviceInitialized{false};

    AudioCaptureConfig config;
    std::unique_ptr<AudioRingBuffer> ring;
    std::atomic<float> leftRms{0.0f};
    std::atomic<float> rightRms{0.0f};
    std::atomic<float> leftPeak{0.0f};
    std::atomic<float> rightPeak{0.0f};
    std::atomic<bool> running{false};
    std::atomic<bool> plannedStop{false};
    std::atomic<bool> unexpectedStop{false};
    std::atomic<uint64_t> droppedFrames{0};
    std::atomic<uint64_t> lossSerial{0};
    std::atomic<AudioCaptureState> state{AudioCaptureState::Stopped};

    mutable std::mutex statusMutex;
    AudioDeviceInfo activeEndpoint;
    std::string lastError;

    uint64_t observedLossSerial{0};
    uint64_t nextReadSample{0};
    std::atomic<uint64_t> streamGeneration{0};
    std::atomic<uint64_t> discardedBacklogFrames{0};
    std::atomic<uint64_t> restartCount{0};
    bool pendingDiscontinuity{false};

    std::chrono::steady_clock::time_point nextEndpointPoll{};
    std::chrono::steady_clock::time_point nextRetry{};
    std::chrono::milliseconds retryDelay{250};

    void SetError(std::string error) {
        std::lock_guard<std::mutex> lock(statusMutex);
        lastError = std::move(error);
    }

    void SetEndpoint(const AudioDeviceInfo& info) {
        std::lock_guard<std::mutex> lock(statusMutex);
        activeEndpoint = info;
        lastError.clear();
    }

    void UpdateLevels(const float* samples, ma_uint32 frameCount) {
        if (samples == nullptr || frameCount == 0) return;
        float sumLeft = 0.0f;
        float sumRight = 0.0f;
        float peakLeft = 0.0f;
        float peakRight = 0.0f;
        for (ma_uint32 index = 0; index < frameCount; ++index) {
            const float left = samples[index * 2];
            const float right = samples[index * 2 + 1];
            sumLeft += left * left;
            sumRight += right * right;
            peakLeft = std::max(peakLeft, std::abs(left));
            peakRight = std::max(peakRight, std::abs(right));
        }
        const float inverse = 1.0f / static_cast<float>(frameCount);
        leftRms.store(std::sqrt(sumLeft * inverse), std::memory_order_relaxed);
        rightRms.store(std::sqrt(sumRight * inverse), std::memory_order_relaxed);
        leftPeak.store(peakLeft, std::memory_order_relaxed);
        rightPeak.store(peakRight, std::memory_order_relaxed);
    }

    static void DataCallback(ma_device* nativeDevice, void*, const void* input,
                             ma_uint32 frameCount) {
        if (nativeDevice == nullptr || nativeDevice->pUserData == nullptr ||
            input == nullptr || frameCount == 0) {
            return;
        }
        auto& self = *static_cast<Impl*>(nativeDevice->pUserData);
        if (!self.running.load(std::memory_order_acquire) || !self.ring) return;
        const auto* samples = static_cast<const float*>(input);
        const size_t written = self.ring->PushInterleaved(samples, frameCount);
        if (written != frameCount) {
            self.droppedFrames.fetch_add(frameCount - written, std::memory_order_relaxed);
            self.lossSerial.fetch_add(1, std::memory_order_release);
        }
        self.UpdateLevels(samples, frameCount);

    }

    static void StopCallback(ma_device* nativeDevice) {
        if (nativeDevice == nullptr || nativeDevice->pUserData == nullptr) return;
        auto& self = *static_cast<Impl*>(nativeDevice->pUserData);
        self.running.store(false, std::memory_order_release);
        if (!self.plannedStop.load(std::memory_order_acquire)) {
            self.unexpectedStop.store(true, std::memory_order_release);
        }
    }

    void CloseDevice() {
        plannedStop.store(true, std::memory_order_release);
        running.store(false, std::memory_order_release);
        if (deviceInitialized) {
            ma_device_uninit(&device);
            deviceInitialized = false;
        }
        plannedStop.store(false, std::memory_order_release);
        unexpectedStop.store(false, std::memory_order_release);
        if (ring) ring->Clear();
        leftRms.store(0.0f, std::memory_order_relaxed);
        rightRms.store(0.0f, std::memory_order_relaxed);
        leftPeak.store(0.0f, std::memory_order_relaxed);
        rightPeak.store(0.0f, std::memory_order_relaxed);
        nextReadSample = 0;
    }

    bool OpenDevice(bool restarting, std::string& error) {
        ma_device_id endpointId{};
        AudioDeviceInfo endpoint;
        if (!FindEndpoint(context, config, endpointId, endpoint)) {
            error = config.selection == AudioEndpointSelection::Fixed
                ? "Configured audio endpoint is unavailable"
                : "No default audio endpoint is available";
            return false;
        }

        ma_device_config nativeConfig = ma_device_config_init(ma_device_type_loopback);
        nativeConfig.capture.format = ma_format_f32;
        nativeConfig.capture.channels = config.channels;
        nativeConfig.sampleRate = config.sampleRate;
        nativeConfig.periodSizeInFrames = config.sampleRate / 100u;
        nativeConfig.dataCallback = DataCallback;
        nativeConfig.stopCallback = StopCallback;
        nativeConfig.pUserData = this;
        if (config.selection == AudioEndpointSelection::Fixed) {
            nativeConfig.capture.pDeviceID = &endpointId;
        }

        const ma_result initializeResult = ma_device_init(&context, &nativeConfig, &device);
        if (initializeResult != MA_SUCCESS) {
            error = std::string("Could not open audio endpoint: ") +
                ma_result_description(initializeResult);
            return false;
        }
        deviceInitialized = true;
        running.store(true, std::memory_order_release);
        const ma_result startResult = ma_device_start(&device);
        if (startResult != MA_SUCCESS) {
            running.store(false, std::memory_order_release);
            ma_device_uninit(&device);
            deviceInitialized = false;
            error = std::string("Could not start audio endpoint: ") +
                ma_result_description(startResult);
            return false;
        }

        if (streamGeneration.load(std::memory_order_relaxed) == 0) {
            streamGeneration.store(1, std::memory_order_relaxed);
        } else if (restarting) {
            streamGeneration.fetch_add(1, std::memory_order_relaxed);
        }
        pendingDiscontinuity = true;
        nextReadSample = 0;
        observedLossSerial = lossSerial.load(std::memory_order_acquire);
        if (restarting) restartCount.fetch_add(1, std::memory_order_relaxed);
        SetEndpoint(endpoint);
        state.store(AudioCaptureState::Running, std::memory_order_release);
        nextEndpointPoll = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(config.endpointPollMs);
        retryDelay = std::chrono::milliseconds(250);
        return true;
    }

    void EnterRecovery(std::string error) {
        SetError(std::move(error));
        state.store(AudioCaptureState::Recovering, std::memory_order_release);
        nextRetry = std::chrono::steady_clock::now() + retryDelay;
        retryDelay = std::min(retryDelay * 2, std::chrono::milliseconds(5000));
    }
};

AudioCapture::AudioCapture() : m_impl(std::make_unique<Impl>()) {}
AudioCapture::~AudioCapture() { Stop(); }

bool AudioCapture::Start(const AudioCaptureConfig& config) {
    return StartInternal(config);
}

bool AudioCapture::StartInternal(const AudioCaptureConfig& config) {
    Stop();
    m_impl->config = config;
    m_impl->droppedFrames.store(0, std::memory_order_relaxed);
    m_impl->lossSerial.store(0, std::memory_order_relaxed);
    m_impl->observedLossSerial = 0;
    m_impl->nextReadSample = 0;
    m_impl->streamGeneration.store(0, std::memory_order_relaxed);
    m_impl->discardedBacklogFrames.store(0, std::memory_order_relaxed);
    m_impl->restartCount.store(0, std::memory_order_relaxed);
    m_impl->pendingDiscontinuity = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->statusMutex);
        m_impl->activeEndpoint = {};
        m_impl->lastError.clear();
    }
    if (config.sampleRate != 48000 || config.channels != 2 || config.bufferFrames < 2 ||
        config.retainedFrames > config.maxBacklogFrames ||
        config.maxBacklogFrames > config.bufferFrames || config.endpointPollMs == 0) {
        m_impl->SetError("Audio capture requires 48 kHz stereo and a valid backlog policy");
        m_impl->state.store(AudioCaptureState::Failed, std::memory_order_release);
        return false;
    }
#ifndef _WIN32
    m_impl->SetError("System loopback capture is supported only by the Windows WASAPI build");
    m_impl->state.store(AudioCaptureState::Unsupported, std::memory_order_release);
    return false;
#endif

    m_impl->ring = std::make_unique<AudioRingBuffer>(config.bufferFrames);
    m_impl->state.store(AudioCaptureState::Starting, std::memory_order_release);
#ifdef _WIN32
    const ma_backend backends[] = {ma_backend_wasapi};
    const ma_result contextResult = ma_context_init(backends, 1, nullptr, &m_impl->context);
#else
    const ma_result contextResult = ma_context_init(nullptr, 0, nullptr, &m_impl->context);
#endif
    if (contextResult != MA_SUCCESS) {
        m_impl->SetError(std::string("Could not initialize audio context: ") +
                         ma_result_description(contextResult));
        m_impl->state.store(AudioCaptureState::Failed, std::memory_order_release);
        return false;
    }
    m_impl->contextInitialized = true;
    std::string error;
    if (!m_impl->OpenDevice(false, error)) {
        m_impl->EnterRecovery(std::move(error));
    }
    return true;
}

void AudioCapture::Poll() {
    if (!m_impl || !m_impl->contextInitialized) return;
    const AudioCaptureState current = m_impl->state.load(std::memory_order_acquire);
    if (current == AudioCaptureState::Stopped || current == AudioCaptureState::Failed ||
        current == AudioCaptureState::Unsupported) {
        return;
    }

    if (m_impl->unexpectedStop.exchange(false, std::memory_order_acq_rel)) {
        m_impl->CloseDevice();
        m_impl->EnterRecovery("Audio endpoint stopped unexpectedly");
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_impl->state.load(std::memory_order_acquire) == AudioCaptureState::Running &&
        m_impl->config.selection == AudioEndpointSelection::FollowDefault &&
        now >= m_impl->nextEndpointPoll) {
        ma_device_id ignored{};
        AudioDeviceInfo currentDefault;
        const bool found = FindEndpoint(m_impl->context, m_impl->config, ignored, currentDefault);
        AudioCaptureStatus status = GetStatus();
        if (!found || currentDefault.id != status.endpointId) {
            m_impl->CloseDevice();
            std::string error;
            if (!m_impl->OpenDevice(true, error)) m_impl->EnterRecovery(std::move(error));
        } else {
            m_impl->nextEndpointPoll = now +
                std::chrono::milliseconds(m_impl->config.endpointPollMs);
        }
    }

    if (m_impl->state.load(std::memory_order_acquire) == AudioCaptureState::Recovering &&
        now >= m_impl->nextRetry) {
        std::string error;
        if (!m_impl->OpenDevice(true, error)) m_impl->EnterRecovery(std::move(error));
    }
}

void AudioCapture::Stop() {
    if (!m_impl) return;
    m_impl->state.store(AudioCaptureState::Stopped, std::memory_order_release);
    m_impl->CloseDevice();
    if (m_impl->contextInitialized) {
        ma_context_uninit(&m_impl->context);
        m_impl->contextInitialized = false;
    }
}

bool AudioCapture::IsRunning() const {
    return m_impl && m_impl->running.load(std::memory_order_acquire) &&
        m_impl->state.load(std::memory_order_acquire) == AudioCaptureState::Running;
}

AudioCaptureState AudioCapture::GetState() const {
    return m_impl ? m_impl->state.load(std::memory_order_acquire) : AudioCaptureState::Stopped;
}

AudioCaptureStatus AudioCapture::GetStatus() const {
    AudioCaptureStatus status;
    if (!m_impl) return status;
    status.state = m_impl->state.load(std::memory_order_acquire);
    status.sampleRate = m_impl->config.sampleRate;
    status.channels = m_impl->config.channels;
    status.streamGeneration = m_impl->streamGeneration.load(std::memory_order_relaxed);
    status.droppedFrames = m_impl->droppedFrames.load(std::memory_order_relaxed);
    status.discardedBacklogFrames =
        m_impl->discardedBacklogFrames.load(std::memory_order_relaxed);
    status.restartCount = m_impl->restartCount.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(m_impl->statusMutex);
    status.endpointId = m_impl->activeEndpoint.id;
    status.endpointName = m_impl->activeEndpoint.name;
    status.nativeChannels = m_impl->activeEndpoint.nativeChannels;
    status.nativeSampleRate = m_impl->activeEndpoint.nativeSampleRate;
    status.lastError = m_impl->lastError;
    return status;
}

size_t AudioCapture::GetAvailableFrames() const {
    return m_impl && m_impl->ring ? m_impl->ring->GetAvailableFrames() : 0;
}

uint64_t AudioCapture::GetDroppedFrames() const {
    return m_impl ? m_impl->droppedFrames.load(std::memory_order_relaxed) : 0;
}

AudioReadResult AudioCapture::Read(float* destination, size_t frameCount) {
    AudioReadResult result;
    if (!m_impl || destination == nullptr || frameCount == 0 || !m_impl->ring) return result;
    Poll();

    const uint64_t loss = m_impl->lossSerial.load(std::memory_order_acquire);
    if (loss != m_impl->observedLossSerial) {
        m_impl->ring->Clear();
        m_impl->observedLossSerial = loss;
        m_impl->streamGeneration.fetch_add(1, std::memory_order_relaxed);
        m_impl->nextReadSample = 0;
        m_impl->pendingDiscontinuity = true;
    }

    const size_t available = m_impl->ring->GetAvailableFrames();
    if (available > m_impl->config.maxBacklogFrames) {
        const size_t discard = available - m_impl->config.retainedFrames;
        m_impl->discardedBacklogFrames.fetch_add(
            m_impl->ring->DiscardFrames(discard), std::memory_order_relaxed);
        m_impl->streamGeneration.fetch_add(1, std::memory_order_relaxed);
        m_impl->nextReadSample = 0;
        m_impl->pendingDiscontinuity = true;
    }

    result.firstSample = m_impl->nextReadSample;
    result.streamGeneration = m_impl->streamGeneration.load(std::memory_order_relaxed);
    result.discontinuity = m_impl->pendingDiscontinuity;
    m_impl->pendingDiscontinuity = false;
    result.frames = m_impl->ring->PopInterleaved(destination, frameCount);
    m_impl->nextReadSample += result.frames;
    return result;
}

size_t AudioCapture::ReadInterleaved(float* destination, size_t frameCount) {
    return Read(destination, frameCount).frames;
}

AudioLevels AudioCapture::GetCurrentLevels() const {
    if (!m_impl) return {};
    return AudioLevels{
        m_impl->leftRms.load(std::memory_order_relaxed),
        m_impl->rightRms.load(std::memory_order_relaxed),
        m_impl->leftPeak.load(std::memory_order_relaxed),
        m_impl->rightPeak.load(std::memory_order_relaxed),
    };
}

} // namespace EchoRadar
