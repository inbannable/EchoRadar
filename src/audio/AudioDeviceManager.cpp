#include "AudioDeviceManager.h"
#include "WasapiEndpointFormat.h"

#include "miniaudio.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace EchoRadar {
namespace {

std::string FingerprintDeviceId(const ma_device_id& id) {
    constexpr uint64_t kOffset = 1469598103934665603ull;
    constexpr uint64_t kPrime = 1099511628211ull;
    uint64_t hash = kOffset;
    const auto* bytes = reinterpret_cast<const unsigned char*>(&id);
    for (size_t index = 0; index < sizeof(id); ++index) {
        hash ^= bytes[index];
        hash *= kPrime;
    }
    std::ostringstream output;
    output << "ma:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

uint32_t WindowsSpeakerBit(ma_channel channel) {
    switch (channel) {
    case MA_CHANNEL_MONO:
    case MA_CHANNEL_FRONT_CENTER: return WindowsSpeaker::FrontCenter;
    case MA_CHANNEL_FRONT_LEFT: return WindowsSpeaker::FrontLeft;
    case MA_CHANNEL_FRONT_RIGHT: return WindowsSpeaker::FrontRight;
    case MA_CHANNEL_LFE: return WindowsSpeaker::Lfe;
    case MA_CHANNEL_BACK_LEFT: return WindowsSpeaker::BackLeft;
    case MA_CHANNEL_BACK_RIGHT: return WindowsSpeaker::BackRight;
    case MA_CHANNEL_SIDE_LEFT: return WindowsSpeaker::SideLeft;
    case MA_CHANNEL_SIDE_RIGHT: return WindowsSpeaker::SideRight;
    default: return 0;
    }
}

uint32_t WindowsSpeakerMask(const ma_channel* channels, uint32_t channelCount) {
    uint32_t mask = 0;
    for (uint32_t index = 0; index < channelCount; ++index) {
        const uint32_t bit = WindowsSpeakerBit(channels[index]);
        if (bit == 0 || (mask & bit) != 0u) return 0;
        mask |= bit;
    }
    return mask;
}

struct EnumeratedEndpoint {
    ma_device_id nativeId{};
    AudioDeviceInfo info;
};

} // namespace

struct AudioDeviceManager::Impl {
    ma_context context{};
    bool initialized{false};
    std::vector<AudioDeviceInfo> outputDevices;

    static ma_bool32 EnumerateCallback(ma_context*, ma_device_type type,
                                       const ma_device_info* nativeInfo,
                                       void* userData) {
        if (type != ma_device_type_playback) return MA_TRUE;
        auto& outputs = *static_cast<std::vector<EnumeratedEndpoint>*>(userData);
        EnumeratedEndpoint endpoint;
        endpoint.nativeId = nativeInfo->id;
        endpoint.info.id = FingerprintDeviceId(nativeInfo->id);
        endpoint.info.name = nativeInfo->name;
        endpoint.info.isDefault = nativeInfo->isDefault != 0;
        outputs.push_back(std::move(endpoint));
        return MA_TRUE;
    }

    void Probe(EnumeratedEndpoint& endpoint) {
#ifdef _WIN32
        WasapiEndpointFormat endpointFormat;
        if (QueryWasapiEndpointFormat(endpoint.nativeId.wasapi, endpointFormat)) {
            endpoint.info.nativeChannels = endpointFormat.channelCount;
            endpoint.info.nativeSampleRate = endpointFormat.sampleRate;
            endpoint.info.nativeChannelMask = endpointFormat.channelMask;
            if (const auto layout = MakeAudioChannelLayout(
                    endpoint.info.nativeChannels,
                    endpoint.info.nativeChannelMask)) {
                endpoint.info.layout = *layout;
            }
            return;
        }
#endif

        ma_device_config config = ma_device_config_init(ma_device_type_loopback);
        config.capture.pDeviceID = &endpoint.nativeId;
        config.capture.format = ma_format_f32;
        config.capture.channels = 0;
        config.sampleRate = 0;

        ma_device probe{};
        if (ma_device_init(&context, &config, &probe) == MA_SUCCESS) {
            endpoint.info.nativeChannels = probe.capture.internalChannels;
            endpoint.info.nativeSampleRate = probe.capture.internalSampleRate;
            endpoint.info.nativeChannelMask = WindowsSpeakerMask(
                probe.capture.internalChannelMap, probe.capture.internalChannels);
            if (const auto layout = MakeAudioChannelLayout(
                    endpoint.info.nativeChannels, endpoint.info.nativeChannelMask)) {
                endpoint.info.layout = *layout;
            }
            ma_device_uninit(&probe);
            return;
        }

        // A backend can refuse a non-started probe. Detailed device data still
        // supplies useful count/rate metadata, though not a speaker mask.
        ma_device_info details{};
        if (ma_context_get_device_info(&context, ma_device_type_playback,
                                       &endpoint.nativeId, &details) != MA_SUCCESS) {
            return;
        }
        for (uint32_t index = 0; index < details.nativeDataFormatCount; ++index) {
            const auto& format = details.nativeDataFormats[index];
            if (endpoint.info.nativeChannels == 0 || format.sampleRate == 48000) {
                endpoint.info.nativeChannels = format.channels;
                endpoint.info.nativeSampleRate = format.sampleRate;
            }
            if (format.sampleRate == 48000) break;
        }
    }

    void Enumerate() {
        outputDevices.clear();
        if (!initialized) return;
        std::vector<EnumeratedEndpoint> endpoints;
        ma_context_enumerate_devices(&context, EnumerateCallback, &endpoints);
        outputDevices.reserve(endpoints.size());
        for (auto& endpoint : endpoints) {
            Probe(endpoint);
            outputDevices.push_back(std::move(endpoint.info));
        }
        std::stable_sort(outputDevices.begin(), outputDevices.end(),
                         [](const AudioDeviceInfo& left, const AudioDeviceInfo& right) {
                             if (left.isDefault != right.isDefault) return left.isDefault;
                             return left.name < right.name;
                         });
    }
};

AudioDeviceManager::AudioDeviceManager() : m_impl(std::make_unique<Impl>()) {
#ifdef _WIN32
    const ma_backend backends[]{ma_backend_wasapi};
    const ma_result result = ma_context_init(backends, 1, nullptr, &m_impl->context);
#else
    const ma_result result = MA_NO_BACKEND;
#endif
    m_impl->initialized = result == MA_SUCCESS;
    m_impl->Enumerate();
}

AudioDeviceManager::~AudioDeviceManager() {
    if (m_impl && m_impl->initialized) ma_context_uninit(&m_impl->context);
}

const std::vector<AudioDeviceInfo>& AudioDeviceManager::GetOutputDevices() const {
    return m_impl->outputDevices;
}

std::vector<AudioDeviceInfo> AudioDeviceManager::EnumerateOutputDevices() const {
    m_impl->Enumerate();
    return m_impl->outputDevices;
}

std::optional<AudioDeviceInfo>
AudioDeviceManager::FindOutputDeviceByName(std::string_view name) const {
    const std::string needle = Lower(std::string(name));
    for (const auto& device : m_impl->outputDevices) {
        if (Lower(device.name).find(needle) != std::string::npos) return device;
    }
    return std::nullopt;
}

std::optional<AudioDeviceInfo>
AudioDeviceManager::FindOutputDeviceById(std::string_view id) const {
    for (const auto& device : m_impl->outputDevices) {
        if (device.id == id) return device;
    }
    return std::nullopt;
}

std::optional<AudioDeviceInfo> AudioDeviceManager::GetDefaultOutputDevice() const {
    for (const auto& device : m_impl->outputDevices) {
        if (device.isDefault) return device;
    }
    return m_impl->outputDevices.empty()
        ? std::nullopt
        : std::optional<AudioDeviceInfo>(m_impl->outputDevices.front());
}

void AudioDeviceManager::Refresh() {
    m_impl->Enumerate();
}

} // namespace EchoRadar
