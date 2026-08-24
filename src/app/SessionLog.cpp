#include "SessionLog.h"

#include <support/FlatJson.h>

#include <algorithm>
#include <iomanip>

namespace EchoRadar {

SessionLogWriter::SessionLogWriter(std::filesystem::path path) {
    Open(path, nullptr);
}

bool SessionLogWriter::Open(const std::filesystem::path& path,
                            std::string* error) {
    Close();
    std::error_code filesystemError;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), filesystemError);
    }
    if (filesystemError) {
        if (error) *error = "Could not create the v2 session directory";
        return false;
    }
    m_output.open(path, std::ios::binary | std::ios::app);
    if (!m_output) {
        if (error) *error = "Could not open the v2 JSONL session log";
        return false;
    }
    m_output << std::fixed << std::setprecision(4);
    if (error) error->clear();
    return true;
}

void SessionLogWriter::Close() {
    if (m_output.is_open()) m_output.close();
    m_haveRadarFrame = false;
    m_lastRadarSample = 0;
    m_lastRadarGeneration = 0;
}

const char* SessionLogWriter::CaptureStateName(AudioCaptureState state) {
    switch (state) {
    case AudioCaptureState::Stopped: return "stopped";
    case AudioCaptureState::Starting: return "starting";
    case AudioCaptureState::Running: return "running";
    case AudioCaptureState::Recovering: return "recovering";
    case AudioCaptureState::Failed: return "failed";
    case AudioCaptureState::Unsupported: return "unsupported";
    }
    return "failed";
}

void SessionLogWriter::WriteLayout(const AudioChannelLayout& layout) {
    m_output << "{\"kind\":\"" << ToString(layout.kind)
             << "\",\"channels\":" << layout.channelCount
             << ",\"mask\":" << layout.channelMask
             << ",\"roles\":[";
    for (uint32_t index = 0; index < layout.channelCount; ++index) {
        if (index != 0) m_output << ',';
        m_output << '\"' << ToString(layout.roles[index]) << '\"';
    }
    m_output << "]}";
}

void SessionLogWriter::WriteStreamStatus(const AudioCaptureStatus& status,
                                         std::string_view message) {
    if (!m_output) return;
    m_output << "{\"schema_version\":3,\"record_type\":\"stream_status\""
             << ",\"stream_generation\":" << status.streamGeneration
             << ",\"capture_state\":\"" << CaptureStateName(status.state) << '\"'
             << ",\"endpoint_id\":\"" << detail::JsonEscapeStr(status.endpointId) << '\"'
             << ",\"endpoint_name\":\"" << detail::JsonEscapeStr(status.endpointName) << '\"'
             << ",\"sample_rate\":" << status.sampleRate
             << ",\"native_sample_rate\":" << status.nativeSampleRate
             << ",\"native_channels\":" << status.nativeChannels
             << ",\"native_channel_mask\":" << status.nativeChannelMask
             << ",\"dropped_frames\":" << status.droppedFrames
             << ",\"discarded_backlog_frames\":" << status.discardedBacklogFrames
             << ",\"restart_count\":" << status.restartCount
             << ",\"layout\":";
    WriteLayout(status.layout);
    m_output << ",\"detail\":\"" << detail::JsonEscapeStr(std::string(message)) << "\"}\n";
    m_output.flush();
}

void SessionLogWriter::WriteRadarFrame(const RadarFrame& frame) {
    if (!m_output || frame.status != RadarRuntimeStatus::Active) return;
    constexpr uint64_t kTenHzInterval = 4800;
    if (m_haveRadarFrame && frame.streamGeneration == m_lastRadarGeneration &&
        frame.timestampSample < m_lastRadarSample + kTenHzInterval) {
        return;
    }
    m_haveRadarFrame = true;
    m_lastRadarGeneration = frame.streamGeneration;
    m_lastRadarSample = frame.timestampSample;
    m_output << "{\"schema_version\":3,\"record_type\":\"radar_frame\""
             << ",\"stream_generation\":" << frame.streamGeneration
             << ",\"timestamp_sample\":" << frame.timestampSample
             << ",\"preset\":\"" << ToString(frame.preset) << '\"'
             << ",\"status\":\"" << ToString(frame.status) << '\"'
             << ",\"confidence\":" << frame.confidence
             << ",\"strongest_sector\":" << frame.strongestSector
             << ",\"strongest_azimuth_degrees\":" << frame.strongestAzimuthDegrees
             << ",\"sectors_dbfs\":[";
    for (size_t index = 0; index < frame.sectorActivitiesDbfs.size(); ++index) {
        if (index != 0) m_output << ',';
        m_output << frame.sectorActivitiesDbfs[index];
    }
    m_output << "]}\n";
}

void SessionLogWriter::WriteEventDirection(uint64_t eventId,
                                           const RadarEventResult& result) {
    if (!m_output) return;
    const SoundEvent& event = result.recognitionEvent;
    m_output << "{\"schema_version\":3,\"record_type\":\"event_direction\""
             << ",\"event_id\":" << eventId
             << ",\"stream_generation\":" << event.streamGeneration
             << ",\"class\":\"" << ToString(event.soundClass) << '\"'
             << ",\"onset_sample\":" << event.onsetSample
             << ",\"confidence\":" << event.confidence
             << ",\"recognition_model_version\":\""
             << detail::JsonEscapeStr(event.modelVersion) << '\"'
             << ",\"preset\":\"" << ToString(result.preset) << '\"'
             << ",\"radar_status\":\"" << ToString(result.status) << '\"'
             << ",\"peak_count\":" << result.peakCount
             << ",\"peaks\":[";
    for (uint32_t index = 0; index < std::min<uint32_t>(
             result.peakCount, static_cast<uint32_t>(result.peaks.size())); ++index) {
        if (index != 0) m_output << ',';
        const RadarPeak& peak = result.peaks[index];
        m_output << "{\"azimuth_degrees\":" << peak.azimuthDegrees
                 << ",\"energy_dbfs\":" << peak.energyDbfs
                 << ",\"confidence\":" << peak.confidence
                 << ",\"angular_uncertainty_degrees\":"
                 << peak.angularUncertaintyDegrees << '}';
    }
    m_output << "]}\n";
    m_output.flush();
}

} // namespace EchoRadar
