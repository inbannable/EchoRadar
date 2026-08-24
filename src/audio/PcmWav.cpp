#include "PcmWav.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

namespace EchoRadar {
namespace {

uint16_t ReadLe16(const unsigned char* bytes) {
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8u);
}

uint32_t ReadLe32(const unsigned char* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8u) |
           (static_cast<uint32_t>(bytes[2]) << 16u) |
           (static_cast<uint32_t>(bytes[3]) << 24u);
}

void WriteLe16(std::ostream& out, uint16_t value) {
    const std::array<unsigned char, 2> bytes{
        static_cast<unsigned char>(value & 0xffu),
        static_cast<unsigned char>((value >> 8u) & 0xffu),
    };
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void WriteLe32(std::ostream& out, uint32_t value) {
    const std::array<unsigned char, 4> bytes{
        static_cast<unsigned char>(value & 0xffu),
        static_cast<unsigned char>((value >> 8u) & 0xffu),
        static_cast<unsigned char>((value >> 16u) & 0xffu),
        static_cast<unsigned char>((value >> 24u) & 0xffu),
    };
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

constexpr std::array<unsigned char, 16> kPcmSubformatGuid{
    0x01, 0x00, 0x00, 0x00,
    0x00, 0x00,
    0x10, 0x00,
    0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71,
};

uint32_t DefaultChannelMask(uint16_t channels) {
    switch (channels) {
    case 1: return WindowsSpeaker::FrontCenter;
    case 2: return WindowsSpeaker::Stereo;
    case 6: return WindowsSpeaker::Surround51Side;
    case 8: return WindowsSpeaker::Surround71;
    default: return 0;
    }
}

bool IsSupportedPcmLayout(uint16_t channels, uint32_t channelMask) {
    if (channels == 1) {
        return channelMask == 0 || channelMask == WindowsSpeaker::FrontCenter;
    }
    if (channels == 2) {
        return channelMask == 0 || channelMask == WindowsSpeaker::Stereo;
    }
    return IsValidWindowsSpeakerMask(channels, channelMask);
}

} // namespace

bool LoadPcmWav(const std::filesystem::path& path, PcmAudio& out, std::string* error) {
    out = {};
    std::ifstream in(path, std::ios::binary);
    if (!in) return Fail(error, "Could not open WAV: " + path.string());

    std::array<unsigned char, 12> riff{};
    in.read(reinterpret_cast<char*>(riff.data()), riff.size());
    if (in.gcount() != static_cast<std::streamsize>(riff.size()) ||
        std::string(reinterpret_cast<char*>(riff.data()), 4) != "RIFF" ||
        std::string(reinterpret_cast<char*>(riff.data() + 8), 4) != "WAVE") {
        return Fail(error, "Not a RIFF/WAVE file: " + path.string());
    }

    uint16_t format = 0;
    uint16_t channels = 0;
    uint16_t bitDepth = 0;
    uint16_t blockAlign = 0;
    uint32_t sampleRate = 0;
    uint32_t channelMask = 0;
    std::vector<unsigned char> data;

    while (in && (format == 0 || data.empty())) {
        std::array<unsigned char, 8> header{};
        in.read(reinterpret_cast<char*>(header.data()), header.size());
        if (in.gcount() != static_cast<std::streamsize>(header.size())) break;
        const std::string id(reinterpret_cast<char*>(header.data()), 4);
        const uint32_t chunkSize = ReadLe32(header.data() + 4);
        if (chunkSize > (1u << 30u)) return Fail(error, "Unreasonable WAV chunk size");

        if (id == "fmt ") {
            std::vector<unsigned char> fmt(chunkSize);
            in.read(reinterpret_cast<char*>(fmt.data()), static_cast<std::streamsize>(fmt.size()));
            if (fmt.size() < 16 || in.gcount() != static_cast<std::streamsize>(fmt.size())) {
                return Fail(error, "Truncated WAV fmt chunk");
            }
            format = ReadLe16(fmt.data());
            channels = ReadLe16(fmt.data() + 2);
            sampleRate = ReadLe32(fmt.data() + 4);
            blockAlign = ReadLe16(fmt.data() + 12);
            bitDepth = ReadLe16(fmt.data() + 14);
            if (format == 0xfffeu) {
                if (fmt.size() < 40 || ReadLe16(fmt.data() + 16) < 22u) {
                    return Fail(error, "Truncated WAVEFORMATEXTENSIBLE fmt chunk");
                }
                const uint16_t validBits = ReadLe16(fmt.data() + 18);
                channelMask = ReadLe32(fmt.data() + 20);
                if ((validBits != 0 && validBits != bitDepth) ||
                    !std::equal(kPcmSubformatGuid.begin(), kPcmSubformatGuid.end(),
                                fmt.begin() + 24)) {
                    return Fail(error, "Only integer PCM WAVEFORMATEXTENSIBLE is supported");
                }
                format = 1;
            }
        } else if (id == "data") {
            data.resize(chunkSize);
            in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
            if (in.gcount() != static_cast<std::streamsize>(data.size())) {
                return Fail(error, "Truncated WAV data chunk");
            }
        } else {
            in.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
        }
        if ((chunkSize & 1u) != 0u) in.seekg(1, std::ios::cur);
    }

    if (format != 1) return Fail(error, "Only integer PCM WAV is supported");
    if (!IsSupportedPcmLayout(channels, channelMask)) {
        return Fail(error, "Unsupported WAV channel layout");
    }
    if (bitDepth != 8 && bitDepth != 16) return Fail(error, "Only PCM8/PCM16 WAV is supported");
    if (sampleRate == 0) return Fail(error, "WAV sample rate is zero");
    const uint16_t expectedAlign = static_cast<uint16_t>(channels * (bitDepth / 8u));
    if (blockAlign != expectedAlign || data.size() % blockAlign != 0) {
        return Fail(error, "Invalid WAV block alignment");
    }

    const size_t sampleCount = data.size() / (bitDepth / 8u);
    out.sampleRate = sampleRate;
    out.channels = channels;
    out.channelMask = channelMask;
    out.interleaved.resize(sampleCount);
    if (bitDepth == 8) {
        for (size_t i = 0; i < sampleCount; ++i) {
            out.interleaved[i] = (static_cast<float>(data[i]) - 128.0f) / 128.0f;
        }
    } else {
        for (size_t i = 0; i < sampleCount; ++i) {
            const int16_t value = static_cast<int16_t>(ReadLe16(data.data() + i * 2));
            out.interleaved[i] = static_cast<float>(value) / 32768.0f;
        }
    }
    if (error) error->clear();
    return true;
}

bool WritePcm16Wav(const std::filesystem::path& path, const PcmAudio& audio, std::string* error) {
    const uint32_t channelMask = audio.channelMask != 0
        ? audio.channelMask
        : DefaultChannelMask(audio.channels);
    if (audio.sampleRate == 0 || !IsSupportedPcmLayout(audio.channels, channelMask)) {
        return Fail(error, "Invalid audio format for WAV output");
    }
    if (audio.interleaved.size() % audio.channels != 0) {
        return Fail(error, "Interleaved sample count is not channel-aligned");
    }
    const uint64_t dataBytes64 = static_cast<uint64_t>(audio.interleaved.size()) * 2ull;
    const bool useExtensible = audio.channels > 2 || audio.channelMask != 0;
    const uint32_t riffOverhead = useExtensible ? 60u : 36u;
    if (dataBytes64 > std::numeric_limits<uint32_t>::max() - riffOverhead) {
        return Fail(error, "WAV is too large for RIFF");
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) return Fail(error, "Could not create WAV: " + path.string());

    const uint32_t dataBytes = static_cast<uint32_t>(dataBytes64);
    const uint16_t blockAlign = static_cast<uint16_t>(audio.channels * 2u);
    out.write("RIFF", 4);
    WriteLe32(out, riffOverhead + dataBytes);
    out.write("WAVEfmt ", 8);
    WriteLe32(out, useExtensible ? 40u : 16u);
    WriteLe16(out, useExtensible ? 0xfffeu : 1u);
    WriteLe16(out, audio.channels);
    WriteLe32(out, audio.sampleRate);
    WriteLe32(out, audio.sampleRate * blockAlign);
    WriteLe16(out, blockAlign);
    WriteLe16(out, 16u);
    if (useExtensible) {
        WriteLe16(out, 22u);
        WriteLe16(out, 16u);
        WriteLe32(out, channelMask);
        out.write(reinterpret_cast<const char*>(kPcmSubformatGuid.data()),
                  static_cast<std::streamsize>(kPcmSubformatGuid.size()));
    }
    out.write("data", 4);
    WriteLe32(out, dataBytes);
    for (const float sample : audio.interleaved) {
        const float clamped = std::clamp(sample, -1.0f, 1.0f);
        const int16_t pcm = static_cast<int16_t>(std::lrint(clamped * (clamped < 0.0f ? 32768.0f : 32767.0f)));
        WriteLe16(out, static_cast<uint16_t>(pcm));
    }
    if (!out.good()) return Fail(error, "Failed while writing WAV");
    if (error) error->clear();
    return true;
}

PcmAudio ResampleLinear(const PcmAudio& source, uint32_t targetSampleRate) {
    if (source.sampleRate == 0 || targetSampleRate == 0 || source.channels == 0 || source.FrameCount() == 0) {
        return {};
    }
    if (source.sampleRate == targetSampleRate) return source;

    const size_t sourceFrames = source.FrameCount();
    const size_t targetFrames = static_cast<size_t>(std::llround(
        static_cast<double>(sourceFrames) * targetSampleRate / source.sampleRate));
    PcmAudio out;
    out.sampleRate = targetSampleRate;
    out.channels = source.channels;
    out.channelMask = source.channelMask;
    out.interleaved.resize(targetFrames * out.channels);
    const double scale = static_cast<double>(source.sampleRate) / targetSampleRate;
    for (size_t frame = 0; frame < targetFrames; ++frame) {
        const double position = static_cast<double>(frame) * scale;
        const size_t left = std::min(static_cast<size_t>(position), sourceFrames - 1);
        const size_t right = std::min(left + 1, sourceFrames - 1);
        const float fraction = static_cast<float>(position - static_cast<double>(left));
        for (uint16_t channel = 0; channel < out.channels; ++channel) {
            const float a = source.interleaved[left * out.channels + channel];
            const float b = source.interleaved[right * out.channels + channel];
            out.interleaved[frame * out.channels + channel] = a + (b - a) * fraction;
        }
    }
    return out;
}

PcmAudio ConvertToStereo48k(const PcmAudio& source) {
    if (source.channels == 0 || source.sampleRate == 0 ||
        source.interleaved.size() % source.channels != 0) {
        return {};
    }

    PcmAudio stereo;
    stereo.sampleRate = source.sampleRate;
    stereo.channels = 2;
    stereo.interleaved.resize(source.FrameCount() * 2);

    if (source.channels == 1 || source.channels == 2) {
        for (size_t frame = 0; frame < source.FrameCount(); ++frame) {
            const float left = source.interleaved[frame * source.channels];
            const float right = source.channels == 2
                ? source.interleaved[frame * 2 + 1]
                : left;
            stereo.interleaved[frame * 2] = left;
            stereo.interleaved[frame * 2 + 1] = right;
        }
    } else {
        const uint32_t mask = source.channelMask != 0
            ? source.channelMask
            : DefaultChannelMask(source.channels);
        const auto layout = MakeAudioChannelLayout(source.channels, mask);
        if (!layout || !DownmixForRecognition(source.interleaved, source.FrameCount(),
                                              *layout, stereo.interleaved)) {
            return {};
        }
    }
    return ResampleLinear(stereo, 48000);
}

} // namespace EchoRadar
