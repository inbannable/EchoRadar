#include "Pcm16WavWriter.h"

#include "AudioTypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace EchoRadar {
namespace {

constexpr uint16_t kPcmFormat = 0x0001u;
constexpr uint16_t kExtensibleFormat = 0xfffeu;
constexpr uint16_t kBitsPerSample = 16u;
constexpr uint16_t kExtensibleExtraBytes = 22u;
constexpr uint32_t kClassicRiffOverhead = 36u;
constexpr uint32_t kExtensibleRiffOverhead = 60u;
constexpr size_t kConversionSamples = 4096u;

constexpr std::array<unsigned char, 16> kPcmSubformatGuid{
    0x01, 0x00, 0x00, 0x00,
    0x00, 0x00,
    0x10, 0x00,
    0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71,
};

void WriteLe16(std::ostream& output, uint16_t value) {
    const std::array<unsigned char, 2> bytes{
        static_cast<unsigned char>(value & 0xffu),
        static_cast<unsigned char>((value >> 8u) & 0xffu),
    };
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void WriteLe32(std::ostream& output, uint32_t value) {
    const std::array<unsigned char, 4> bytes{
        static_cast<unsigned char>(value & 0xffu),
        static_cast<unsigned char>((value >> 8u) & 0xffu),
        static_cast<unsigned char>((value >> 16u) & 0xffu),
        static_cast<unsigned char>((value >> 24u) & 0xffu),
    };
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

bool IsSupportedFormat(const Pcm16WavWriterConfig& config) {
    if (config.sampleRate == 0) return false;
    switch (config.channels) {
    case 1:
        return config.channelMask == 0 ||
               config.channelMask == WindowsSpeaker::FrontCenter;
    case 2:
        return config.channelMask == 0 ||
               config.channelMask == WindowsSpeaker::Stereo;
    case 6:
    case 8:
        return IsValidWindowsSpeakerMask(config.channels, config.channelMask);
    default:
        return false;
    }
}

int16_t ToPcm16(float sample) {
    if (std::isnan(sample)) return 0;
    if (sample >= 1.0f) return 32767;
    if (sample <= -1.0f) return static_cast<int16_t>(-32768);
    const float clamped = std::clamp(sample, -1.0f, 1.0f);
    return static_cast<int16_t>(std::lrint(
        clamped * (clamped < 0.0f ? 32768.0f : 32767.0f)));
}

} // namespace

Pcm16WavWriter::Pcm16WavWriter(const std::filesystem::path& path,
                               const Pcm16WavWriterConfig& config,
                               std::string* error) {
    Open(path, config, error);
}

Pcm16WavWriter::~Pcm16WavWriter() {
    Close(nullptr);
}

bool Pcm16WavWriter::Fail(std::string message, std::string* error) noexcept {
    if (m_lastError.empty()) m_lastError = std::move(message);
    if (error) *error = m_lastError;
    return false;
}

bool Pcm16WavWriter::Open(const std::filesystem::path& path,
                          const Pcm16WavWriterConfig& config,
                          std::string* error) {
    if (m_output.is_open() && !Close(error)) return false;

    m_path.clear();
    m_config = {};
    m_dataBytes = 0;
    m_maximumFrames = 0;
    m_riffOverhead = 0;
    m_dataSizeOffset = 0;
    m_lastError.clear();
    m_wasClosedSuccessfully = false;

    if (!IsSupportedFormat(config)) {
        return Fail("Invalid PCM16 WAV writer format or channel mask", error);
    }
    const uint16_t blockAlign = static_cast<uint16_t>(config.channels * 2u);
    if (config.sampleRate >
        std::numeric_limits<uint32_t>::max() / blockAlign) {
        return Fail("PCM16 WAV byte rate exceeds the RIFF field limit", error);
    }

    const bool extensible = config.channels > 2 || config.channelMask != 0;
    m_riffOverhead = extensible
        ? kExtensibleRiffOverhead
        : kClassicRiffOverhead;
    const uint64_t riffMaximumFrames =
        (static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) -
         m_riffOverhead) / blockAlign;
    if (config.maximumFrames != 0 &&
        config.maximumFrames > riffMaximumFrames) {
        return Fail("Configured WAV frame limit exceeds classic RIFF capacity", error);
    }

    m_path = path;
    m_config = config;
    m_maximumFrames = config.maximumFrames == 0
        ? riffMaximumFrames
        : config.maximumFrames;
    m_output.open(path, std::ios::binary | std::ios::trunc);
    if (!m_output) {
        return Fail("Could not create WAV: " + path.string(), error);
    }
    if (!WriteProvisionalHeader(error)) {
        m_output.close();
        return false;
    }
    if (error) error->clear();
    return true;
}

bool Pcm16WavWriter::WriteProvisionalHeader(std::string* error) noexcept {
    const bool extensible = m_riffOverhead == kExtensibleRiffOverhead;
    const uint16_t blockAlign = static_cast<uint16_t>(m_config.channels * 2u);

    m_output.write("RIFF", 4);
    WriteLe32(m_output, m_riffOverhead);
    m_output.write("WAVEfmt ", 8);
    WriteLe32(m_output, extensible ? 40u : 16u);
    WriteLe16(m_output, extensible ? kExtensibleFormat : kPcmFormat);
    WriteLe16(m_output, m_config.channels);
    WriteLe32(m_output, m_config.sampleRate);
    WriteLe32(m_output, m_config.sampleRate * blockAlign);
    WriteLe16(m_output, blockAlign);
    WriteLe16(m_output, kBitsPerSample);
    if (extensible) {
        WriteLe16(m_output, kExtensibleExtraBytes);
        WriteLe16(m_output, kBitsPerSample);
        WriteLe32(m_output, m_config.channelMask);
        m_output.write(reinterpret_cast<const char*>(kPcmSubformatGuid.data()),
                       static_cast<std::streamsize>(kPcmSubformatGuid.size()));
    }
    m_output.write("data", 4);
    m_dataSizeOffset = static_cast<std::streamoff>(m_output.tellp());
    WriteLe32(m_output, 0u);
    if (!m_output.good() || m_dataSizeOffset <= 0) {
        return Fail("Failed while writing the provisional WAV header", error);
    }
    return true;
}

bool Pcm16WavWriter::AppendFrames(std::span<const float> interleaved,
                                  size_t frameCount,
                                  std::string* error) {
    if (!m_output.is_open()) return Fail("PCM16 WAV writer is not open", error);
    if (HasError()) {
        if (error) *error = m_lastError;
        return false;
    }
    if (frameCount > std::numeric_limits<size_t>::max() / m_config.channels ||
        interleaved.size() != frameCount * m_config.channels) {
        return Fail("PCM16 WAV append is not frame-aligned", error);
    }
    if (frameCount == 0) {
        if (error) error->clear();
        return true;
    }
    if (FrameCount() > m_maximumFrames ||
        frameCount > m_maximumFrames - FrameCount()) {
        return Fail("PCM16 WAV recording exceeds its configured frame limit", error);
    }

    std::array<unsigned char, kConversionSamples * 2u> converted{};
    const size_t maximumChunkFrames =
        std::max<size_t>(1u, kConversionSamples / m_config.channels);
    size_t sourceFrame = 0;
    while (sourceFrame < frameCount) {
        const size_t chunkFrames =
            std::min(maximumChunkFrames, frameCount - sourceFrame);
        const size_t chunkSamples = chunkFrames * m_config.channels;
        const size_t sourceSample = sourceFrame * m_config.channels;
        for (size_t index = 0; index < chunkSamples; ++index) {
            const uint16_t pcm = static_cast<uint16_t>(
                ToPcm16(interleaved[sourceSample + index]));
            converted[index * 2u] = static_cast<unsigned char>(pcm & 0xffu);
            converted[index * 2u + 1u] =
                static_cast<unsigned char>((pcm >> 8u) & 0xffu);
        }
        const uint64_t chunkBytes = static_cast<uint64_t>(chunkSamples) * 2u;
        m_output.write(reinterpret_cast<const char*>(converted.data()),
                       static_cast<std::streamsize>(chunkBytes));
        if (!m_output.good()) {
            return Fail("Failed while appending PCM16 WAV samples", error);
        }
        m_dataBytes += chunkBytes;
        sourceFrame += chunkFrames;
    }
    if (error) error->clear();
    return true;
}

bool Pcm16WavWriter::AppendInterleaved(std::span<const float> interleaved,
                                       std::string* error) {
    if (m_config.channels == 0 ||
        interleaved.size() % m_config.channels != 0) {
        return Fail("PCM16 WAV append is not channel-aligned", error);
    }
    return AppendFrames(interleaved,
                        interleaved.size() / m_config.channels,
                        error);
}

uint64_t Pcm16WavWriter::FrameCount() const noexcept {
    if (m_config.channels == 0) return 0;
    return m_dataBytes / (static_cast<uint64_t>(m_config.channels) * 2u);
}

bool Pcm16WavWriter::FinalizeHeader(std::string* error) noexcept {
    if (m_dataBytes > std::numeric_limits<uint32_t>::max() - m_riffOverhead) {
        return Fail("PCM16 WAV payload exceeds classic RIFF capacity", error);
    }
    const uint32_t dataBytes = static_cast<uint32_t>(m_dataBytes);
    const uint32_t riffSize = m_riffOverhead + dataBytes;

    // Clear a transient stream failure so finalization can make a best effort.
    // Persistent I/O failures will fail the following seek/write checks again.
    m_output.clear();
    m_output.seekp(4, std::ios::beg);
    WriteLe32(m_output, riffSize);
    m_output.seekp(m_dataSizeOffset, std::ios::beg);
    WriteLe32(m_output, dataBytes);
    m_output.seekp(0, std::ios::end);
    m_output.flush();
    if (!m_output.good()) {
        return Fail("Failed while finalizing the PCM16 WAV header", error);
    }
    return true;
}

bool Pcm16WavWriter::Close(std::string* error) noexcept {
    if (!m_output.is_open()) {
        const bool success = m_wasClosedSuccessfully && !HasError();
        if (error) {
            if (success) error->clear();
            else *error = m_lastError.empty()
                ? "PCM16 WAV writer is not open"
                : m_lastError;
        }
        return success;
    }

    const bool hadError = HasError();
    const bool finalized = FinalizeHeader(error);
    m_output.close();
    const bool closed = !m_output.fail();
    if (!closed) Fail("Failed while closing the PCM16 WAV file", error);
    m_wasClosedSuccessfully = finalized && closed && !hadError;
    if (!m_wasClosedSuccessfully && error && error->empty()) {
        *error = m_lastError;
    }
    return m_wasClosedSuccessfully;
}

} // namespace EchoRadar
