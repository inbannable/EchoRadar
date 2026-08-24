#pragma once

#include <audio/AudioCapture.h>
#include <radar/RadarTypes.h>

#include <filesystem>
#include <fstream>
#include <string_view>

namespace EchoRadar {

/// Append-only JSONL schema-3 session writer. Continuous frames are capped at
/// 10 Hz by sample time; event results and stream transitions are never folded.
class SessionLogWriter {
public:
    SessionLogWriter() = default;
    explicit SessionLogWriter(std::filesystem::path path);

    bool Open(const std::filesystem::path& path, std::string* error = nullptr);
    void Close();
    bool IsOpen() const { return m_output.is_open(); }
    bool IsHealthy() const { return m_output.is_open() && !!m_output; }

    void WriteStreamStatus(const AudioCaptureStatus& status,
                           std::string_view message = {});
    void WriteRadarFrame(const RadarFrame& frame);
    void WriteEventDirection(uint64_t eventId,
                             const RadarEventResult& result);

private:
    std::ofstream m_output;
    uint64_t m_lastRadarSample{0};
    uint64_t m_lastRadarGeneration{0};
    bool m_haveRadarFrame{false};

    void WriteLayout(const AudioChannelLayout& layout);
    static const char* CaptureStateName(AudioCaptureState state);
};

} // namespace EchoRadar
