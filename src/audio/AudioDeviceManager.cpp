#include "AudioDeviceManager.h"
#include "miniaudio.h"
#include <algorithm>
#include <cctype>
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

// ─────────────────────────────────────────────────────────────────────────────
//  Impl
// ─────────────────────────────────────────────────────────────────────────────

struct AudioDeviceManager::Impl {
    ma_context               ctx{};
    bool                     ctxInit{false};
    std::vector<AudioDeviceInfo> devices;

    // Passed as user-data to the miniaudio enumeration callback.
    struct EnumUD {
        std::vector<AudioDeviceInfo>* out;
    };

    static ma_bool32 EnumCallback(ma_context*, ma_device_type type,
                                   const ma_device_info* pInfo, void* ud)
    {
        if (type != ma_device_type_capture) return MA_TRUE;
        auto& out = *static_cast<EnumUD*>(ud)->out;
        AudioDeviceInfo info;
        info.name      = pInfo->name;
        info.type      = ClassifyDevice(pInfo->name);  // Detect device type
        info.isDefault = (pInfo->isDefault != 0);
        out.push_back(std::move(info));
        return MA_TRUE;
    }

    void Enumerate() {
        devices.clear();
        if (!ctxInit) return;
        EnumUD ud{&devices};
        ma_context_enumerate_devices(&ctx, EnumCallback, &ud);
        
        // Sort by priority: loopback > virtual cable > default > microphone
        std::stable_sort(devices.begin(), devices.end(),
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
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  AudioDeviceManager
// ─────────────────────────────────────────────────────────────────────────────

AudioDeviceManager::AudioDeviceManager()
    : m_impl(std::make_unique<Impl>())
{
    if (ma_context_init(nullptr, 0, nullptr, &m_impl->ctx) == MA_SUCCESS)
        m_impl->ctxInit = true;
    m_impl->Enumerate();
}

AudioDeviceManager::~AudioDeviceManager() {
    if (m_impl && m_impl->ctxInit)
        ma_context_uninit(&m_impl->ctx);
}

const std::vector<AudioDeviceInfo>& AudioDeviceManager::GetInputDevices() const {
    return m_impl->devices;
}

std::vector<AudioDeviceInfo> AudioDeviceManager::EnumerateInputDevices() const {
    m_impl->Enumerate();
    return m_impl->devices;
}

std::optional<AudioDeviceInfo>
AudioDeviceManager::FindInputDeviceByName(std::string_view name) const {
    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        return s;
    };
    const std::string needle = toLower(std::string(name));
    for (const auto& d : m_impl->devices) {
        if (toLower(d.name).find(needle) != std::string::npos)
            return d;
    }
    return std::nullopt;
}

std::optional<AudioDeviceInfo> AudioDeviceManager::GetDefaultInputDevice() const {
    for (const auto& d : m_impl->devices)
        if (d.isDefault) return d;
    if (!m_impl->devices.empty())
        return m_impl->devices.front();
    return std::nullopt;
}

void AudioDeviceManager::Refresh() {
    m_impl->Enumerate();
}

} // namespace EchoRadar
