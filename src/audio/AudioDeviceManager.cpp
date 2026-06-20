#include "AudioDeviceManager.h"
#include "miniaudio.h"
#include <algorithm>
#include <cctype>
#include <string>

namespace EchoRadar {

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
        info.isDefault = (pInfo->isDefault != 0);
        out.push_back(std::move(info));
        return MA_TRUE;
    }

    void Enumerate() {
        devices.clear();
        if (!ctxInit) return;
        EnumUD ud{&devices};
        ma_context_enumerate_devices(&ctx, EnumCallback, &ud);
        // Sort so default device is always first.
        std::stable_sort(devices.begin(), devices.end(),
                         [](const AudioDeviceInfo& a, const AudioDeviceInfo& b) {
                             return static_cast<int>(a.isDefault) >
                                    static_cast<int>(b.isDefault);
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
