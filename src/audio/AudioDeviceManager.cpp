#include "AudioDeviceManager.h"

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

} // namespace

struct AudioDeviceManager::Impl {
    ma_context context{};
    bool initialized{false};
    std::vector<AudioDeviceInfo> outputDevices;

    static ma_bool32 EnumerateCallback(ma_context*, ma_device_type type,
                                       const ma_device_info* nativeInfo,
                                       void* userData) {
        if (type != ma_device_type_playback) return MA_TRUE;
        auto& outputs = *static_cast<std::vector<AudioDeviceInfo>*>(userData);
        AudioDeviceInfo info;
        info.id = FingerprintDeviceId(nativeInfo->id);
        info.name = nativeInfo->name;
        info.isDefault = nativeInfo->isDefault != 0;
        if (nativeInfo->nativeDataFormatCount != 0) {
            info.nativeChannels = nativeInfo->nativeDataFormats[0].channels;
            info.nativeSampleRate = nativeInfo->nativeDataFormats[0].sampleRate;
        }
        outputs.push_back(std::move(info));
        return MA_TRUE;
    }

    void Enumerate() {
        outputDevices.clear();
        if (!initialized) return;
        ma_context_enumerate_devices(&context, EnumerateCallback, &outputDevices);
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
