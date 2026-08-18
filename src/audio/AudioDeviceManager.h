#pragma once

#include "AudioDeviceInfo.h"

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace EchoRadar {

/// Enumerates Windows render endpoints for loopback capture.
class AudioDeviceManager {
public:
    AudioDeviceManager();
    ~AudioDeviceManager();

    AudioDeviceManager(const AudioDeviceManager&) = delete;
    AudioDeviceManager& operator=(const AudioDeviceManager&) = delete;

    const std::vector<AudioDeviceInfo>& GetOutputDevices() const;
    std::vector<AudioDeviceInfo> EnumerateOutputDevices() const;
    std::optional<AudioDeviceInfo> FindOutputDeviceByName(std::string_view name) const;
    std::optional<AudioDeviceInfo> FindOutputDeviceById(std::string_view id) const;
    std::optional<AudioDeviceInfo> GetDefaultOutputDevice() const;
    void Refresh();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace EchoRadar
