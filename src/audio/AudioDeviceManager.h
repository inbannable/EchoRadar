#pragma once
#include "AudioDeviceInfo.h"
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace EchoRadar {

/// Enumerates audio capture devices via miniaudio.
/// Device list is built at construction time; call Refresh() to re-enumerate.
class AudioDeviceManager {
public:
    AudioDeviceManager();
    ~AudioDeviceManager();

    AudioDeviceManager(const AudioDeviceManager&)            = delete;
    AudioDeviceManager& operator=(const AudioDeviceManager&) = delete;

    /// Cached device list from the last enumeration (default device listed first).
    const std::vector<AudioDeviceInfo>& GetInputDevices() const;

    /// Re-enumerate and return the updated list.
    std::vector<AudioDeviceInfo> EnumerateInputDevices() const;

    /// Find the first device whose name contains @p name (case-insensitive).
    std::optional<AudioDeviceInfo> FindInputDeviceByName(std::string_view name) const;

    /// Return the default capture device, or nullopt if none found.
    std::optional<AudioDeviceInfo> GetDefaultInputDevice() const;

    /// Re-enumerate devices and refresh the cache.
    void Refresh();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace EchoRadar
