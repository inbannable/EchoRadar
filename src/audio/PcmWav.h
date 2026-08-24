#pragma once

#include "AudioTypes.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace EchoRadar {

struct PcmAudio {
    uint32_t sampleRate{0};
    uint16_t channels{0};
    uint32_t channelMask{0};
    std::vector<float> interleaved;

    size_t FrameCount() const {
        return channels == 0 ? 0 : interleaved.size() / channels;
    }

    std::optional<AudioChannelLayout> Layout() const noexcept {
        const uint32_t mask = channelMask != 0
            ? channelMask
            : (channels == 1 ? WindowsSpeaker::FrontCenter
                             : (channels == 2 ? WindowsSpeaker::Stereo : 0u));
        return MakeAudioChannelLayout(channels, mask);
    }
};

bool LoadPcmWav(const std::filesystem::path& path, PcmAudio& out, std::string* error = nullptr);
bool WritePcm16Wav(const std::filesystem::path& path, const PcmAudio& audio, std::string* error = nullptr);
PcmAudio ResampleLinear(const PcmAudio& source, uint32_t targetSampleRate);
PcmAudio ConvertToStereo48k(const PcmAudio& source);

} // namespace EchoRadar
