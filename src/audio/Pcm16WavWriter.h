#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>

namespace EchoRadar {

/// Format and safety limit for an incremental PCM16 RIFF/WAVE recording.
struct Pcm16WavWriterConfig {
    uint32_t sampleRate{48000};
    uint16_t channels{0};
    uint32_t channelMask{0};

    /// Optional application limit. Zero uses the largest whole-frame payload
    /// representable by a classic 32-bit RIFF/WAVE file.
    uint64_t maximumFrames{0};
};

/// Bounded, incremental PCM16 WAV writer.
///
/// Samples are converted and written in fixed-size chunks, so memory usage is
/// independent of recording duration. Six/eight-channel output always uses
/// WAVEFORMATEXTENSIBLE and requires an exact supported Windows speaker mask.
/// Close() repairs the RIFF and data sizes; the destructor performs the same
/// best-effort finalization when Close() was not called explicitly.
///
/// This class is intended for one processing thread and is not thread-safe.
class Pcm16WavWriter {
public:
    Pcm16WavWriter() = default;
    Pcm16WavWriter(const std::filesystem::path& path,
                   const Pcm16WavWriterConfig& config,
                   std::string* error = nullptr);
    ~Pcm16WavWriter();

    Pcm16WavWriter(const Pcm16WavWriter&) = delete;
    Pcm16WavWriter& operator=(const Pcm16WavWriter&) = delete;
    Pcm16WavWriter(Pcm16WavWriter&&) = delete;
    Pcm16WavWriter& operator=(Pcm16WavWriter&&) = delete;

    bool Open(const std::filesystem::path& path,
              const Pcm16WavWriterConfig& config,
              std::string* error = nullptr);

    /// Append exactly frameCount interleaved frames. The span length must equal
    /// frameCount * Channels(). A rejected append never writes a partial frame.
    bool AppendFrames(std::span<const float> interleaved,
                      size_t frameCount,
                      std::string* error = nullptr);

    /// Append a channel-aligned interleaved span.
    bool AppendInterleaved(std::span<const float> interleaved,
                           std::string* error = nullptr);

    /// Compatibility spelling for callers that treat the writer as a stream.
    bool WriteFrames(std::span<const float> interleaved,
                     size_t frameCount,
                     std::string* error = nullptr) {
        return AppendFrames(interleaved, frameCount, error);
    }

    /// Finalize header sizes, flush, and close. Idempotent after a successful
    /// close. Returns false when this recording encountered an earlier error.
    bool Close(std::string* error = nullptr) noexcept;

    bool IsOpen() const noexcept { return m_output.is_open(); }
    bool HasError() const noexcept { return !m_lastError.empty(); }
    std::string_view LastError() const noexcept { return m_lastError; }
    uint64_t FrameCount() const noexcept;
    uint64_t DataBytes() const noexcept { return m_dataBytes; }
    uint64_t MaximumFrames() const noexcept { return m_maximumFrames; }
    uint32_t SampleRate() const noexcept { return m_config.sampleRate; }
    uint16_t Channels() const noexcept { return m_config.channels; }
    uint32_t ChannelMask() const noexcept { return m_config.channelMask; }
    const std::filesystem::path& Path() const noexcept { return m_path; }

private:
    std::ofstream m_output;
    std::filesystem::path m_path;
    Pcm16WavWriterConfig m_config{};
    uint64_t m_dataBytes{0};
    uint64_t m_maximumFrames{0};
    uint32_t m_riffOverhead{0};
    std::streamoff m_dataSizeOffset{0};
    std::string m_lastError;
    bool m_wasClosedSuccessfully{false};

    bool WriteProvisionalHeader(std::string* error) noexcept;
    bool FinalizeHeader(std::string* error) noexcept;
    bool Fail(std::string message, std::string* error) noexcept;
};

} // namespace EchoRadar
