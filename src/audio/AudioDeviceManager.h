#pragma once
#include "AudioDeviceInfo.h"
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace EchoRadar {

/// Enumerates input devices and render endpoints via miniaudio.
/// Device list is built at construction time; call Refresh() to re-enumerate.
class AudioDeviceManager {
public:
    AudioDeviceManager();
    ~AudioDeviceManager();

    AudioDeviceManager(const AudioDeviceManager&)            = delete;
    AudioDeviceManager& operator=(const AudioDeviceManager&) = delete;

    /// Cached device list from the last enumeration (default device listed first).
    const std::vector<AudioDeviceInfo>& GetInputDevices() const;

    /// Cached render endpoints. These are the devices that can be captured with
    /// WASAPI loopback without rerouting playback through a virtual cable.
    const std::vector<AudioDeviceInfo>& GetOutputDevices() const;

    /// Re-enumerate and return the updated list.
    std::vector<AudioDeviceInfo> EnumerateInputDevices() const;
    std::vector<AudioDeviceInfo> EnumerateOutputDevices() const;

    /// Find the first device whose name contains @p name (case-insensitive).
    std::optional<AudioDeviceInfo> FindInputDeviceByName(std::string_view name) const;
    std::optional<AudioDeviceInfo> FindOutputDeviceByName(std::string_view name) const;
    std::optional<AudioDeviceInfo> FindOutputDeviceById(std::string_view id) const;

    /// Return the default capture device, or nullopt if none found.
    std::optional<AudioDeviceInfo> GetDefaultInputDevice() const;
    std::optional<AudioDeviceInfo> GetDefaultOutputDevice() const;

    /// Re-enumerate devices and refresh the cache.
    void Refresh();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace EchoRadar
