#include "AudioDeviceManager.h"
#include "miniaudio.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace EchoRadar {

// ─────────────────────────────────────────────────────────────────────────────
//  Device Type Detection
// ─────────────────────────────────────────────────────────────────────────────

/// Heuristic to classify device type by name patterns.
static AudioDeviceType ClassifyDevice(const std::string& name) {
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    
    // Virtual Cable devices (VB-Audio, etc.)
    if (lower.find("cable") != std::string::npos ||
        lower.find("vb-audio") != std::string::npos ||
        lower.find("meeter") != std::string::npos ||
        lower.find("obs") != std::string::npos) {
        return AudioDeviceType::VirtualCable;
    }
    
    // Stereo Mix / Loopback (system audio output capture)
    if (lower.find("stereo mix") != std::string::npos ||
        lower.find("what u hear") != std::string::npos ||
        lower.find("loopback") != std::string::npos ||
        lower.find("wave out mix") != std::string::npos ||
        lower.find("mix") != std::string::npos) {
        return AudioDeviceType::Loopback;
    }
    
    // Line Input / Aux
    if (lower.find("line in") != std::string::npos ||
        lower.find("aux") != std::string::npos) {
        return AudioDeviceType::LineIn;
    }
    
    // Physical Microphones
    if (lower.find("microphone") != std::string::npos ||
        lower.find("mic ") != std::string::npos ||
        lower.find("mic)") != std::string::npos) {
        return AudioDeviceType::Microphone;
    }
    
    // Default to Microphone for unknown devices (safer default)
    return AudioDeviceType::Microphone;
}

static std::string FingerprintDeviceId(const ma_device_id& id) {
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

// ─────────────────────────────────────────────────────────────────────────────
//  Impl
// ─────────────────────────────────────────────────────────────────────────────

struct AudioDeviceManager::Impl {
    ma_context               ctx{};
    bool                     ctxInit{false};
    std::vector<AudioDeviceInfo> inputDevices;
    std::vector<AudioDeviceInfo> outputDevices;

    // Passed as user-data to the miniaudio enumeration callback.
    struct EnumUD {
        std::vector<AudioDeviceInfo>* inputs;
        std::vector<AudioDeviceInfo>* outputs;
    };

    static ma_bool32 EnumCallback(ma_context*, ma_device_type type,
                                   const ma_device_info* pInfo, void* ud)
    {
        if (type != ma_device_type_capture && type != ma_device_type_playback) return MA_TRUE;
        auto& data = *static_cast<EnumUD*>(ud);
        auto& out = type == ma_device_type_playback ? *data.outputs : *data.inputs;
        AudioDeviceInfo info;
        info.id        = FingerprintDeviceId(pInfo->id);
        info.name      = pInfo->name;
        info.flow      = type == ma_device_type_playback
            ? AudioDeviceFlow::Output : AudioDeviceFlow::Input;
        info.type      = type == ma_device_type_playback
            ? AudioDeviceType::Loopback : ClassifyDevice(pInfo->name);
        info.isDefault = (pInfo->isDefault != 0);
        if (pInfo->nativeDataFormatCount != 0) {
            info.nativeChannels = pInfo->nativeDataFormats[0].channels;
            info.nativeSampleRate = pInfo->nativeDataFormats[0].sampleRate;
        }
        out.push_back(std::move(info));
        return MA_TRUE;
    }

    void Enumerate() {
        inputDevices.clear();
        outputDevices.clear();
        if (!ctxInit) return;
        EnumUD ud{&inputDevices, &outputDevices};
        ma_context_enumerate_devices(&ctx, EnumCallback, &ud);
        
        // Sort by priority: loopback > virtual cable > default > microphone
        std::stable_sort(inputDevices.begin(), inputDevices.end(),
                         [](const AudioDeviceInfo& a, const AudioDeviceInfo& b) {
                             auto priority = [](const AudioDeviceInfo& d) {
                                 if (d.type == AudioDeviceType::Loopback) return 4;
                                 if (d.type == AudioDeviceType::VirtualCable) return 3;
                                 if (d.isDefault) return 2;
                                 if (d.type == AudioDeviceType::Microphone) return 1;
                                 return 0;
                             };
                             return priority(a) > priority(b);
                         });
        std::stable_sort(outputDevices.begin(), outputDevices.end(),
                         [](const AudioDeviceInfo& a, const AudioDeviceInfo& b) {
                             if (a.isDefault != b.isDefault) return a.isDefault;
                             return a.name < b.name;
                         });
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  AudioDeviceManager
// ─────────────────────────────────────────────────────────────────────────────

AudioDeviceManager::AudioDeviceManager()
    : m_impl(std::make_unique<Impl>())
{
#ifdef _WIN32
    const ma_backend backends[] = {ma_backend_wasapi};
    const ma_result result = ma_context_init(backends, 1, nullptr, &m_impl->ctx);
#else
    const ma_result result = ma_context_init(nullptr, 0, nullptr, &m_impl->ctx);
#endif
    if (result == MA_SUCCESS)
        m_impl->ctxInit = true;
    m_impl->Enumerate();
}

AudioDeviceManager::~AudioDeviceManager() {
    if (m_impl && m_impl->ctxInit)
        ma_context_uninit(&m_impl->ctx);
}

const std::vector<AudioDeviceInfo>& AudioDeviceManager::GetInputDevices() const {
    return m_impl->inputDevices;
}

const std::vector<AudioDeviceInfo>& AudioDeviceManager::GetOutputDevices() const {
    return m_impl->outputDevices;
}

std::vector<AudioDeviceInfo> AudioDeviceManager::EnumerateInputDevices() const {
    m_impl->Enumerate();
    return m_impl->inputDevices;
}

std::vector<AudioDeviceInfo> AudioDeviceManager::EnumerateOutputDevices() const {
    m_impl->Enumerate();
    return m_impl->outputDevices;
}

std::optional<AudioDeviceInfo>
AudioDeviceManager::FindInputDeviceByName(std::string_view name) const {
    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        return s;
    };
    const std::string needle = toLower(std::string(name));
    for (const auto& d : m_impl->inputDevices) {
        if (toLower(d.name).find(needle) != std::string::npos)
            return d;
    }
    return std::nullopt;
}

std::optional<AudioDeviceInfo>
AudioDeviceManager::FindOutputDeviceByName(std::string_view name) const {
    auto toLower = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        return value;
    };
    const std::string needle = toLower(std::string(name));
    for (const auto& device : m_impl->outputDevices) {
        if (toLower(device.name).find(needle) != std::string::npos) return device;
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

std::optional<AudioDeviceInfo> AudioDeviceManager::GetDefaultInputDevice() const {
    for (const auto& d : m_impl->inputDevices)
        if (d.isDefault) return d;
    if (!m_impl->inputDevices.empty())
        return m_impl->inputDevices.front();
    return std::nullopt;
}

std::optional<AudioDeviceInfo> AudioDeviceManager::GetDefaultOutputDevice() const {
    for (const auto& device : m_impl->outputDevices) {
        if (device.isDefault) return device;
    }
    if (!m_impl->outputDevices.empty()) return m_impl->outputDevices.front();
    return std::nullopt;
}

void AudioDeviceManager::Refresh() {
    m_impl->Enumerate();
}

} // namespace EchoRadar
