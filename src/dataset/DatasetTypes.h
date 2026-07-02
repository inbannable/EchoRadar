#pragma once

#include <cstdint>
#include <string>

namespace EchoRadar {

// Canonical dataset labels. "Unknown" is the default bucket for freshly
// recorded events until a human (or future auto-labeler) classifies them.
enum class DatasetLabel {
    Gunshot,
    Footstep,
    Reload,
    Switch,
    Ambient,
    Unknown
};

inline const char* ToString(DatasetLabel label) {
    switch (label) {
    case DatasetLabel::Gunshot: return "gunshot";
    case DatasetLabel::Footstep: return "footstep";
    case DatasetLabel::Reload: return "reload";
    case DatasetLabel::Switch: return "switch";
    case DatasetLabel::Ambient: return "ambient";
    case DatasetLabel::Unknown: return "unknown";
    }
    return "unknown";
}

inline DatasetLabel LabelFromString(const std::string& value) {
    if (value == "gunshot") return DatasetLabel::Gunshot;
    if (value == "footstep") return DatasetLabel::Footstep;
    if (value == "reload") return DatasetLabel::Reload;
    if (value == "switch") return DatasetLabel::Switch;
    if (value == "ambient") return DatasetLabel::Ambient;
    return DatasetLabel::Unknown;
}

// One row of the in-memory dataset index. Mirrors the on-disk
// dataset/<label>/<id>/{audio.wav,features.csv,metadata.json} layout.
struct DatasetEventRecord {
    std::string id;              // e.g. "000512"
    DatasetLabel label{DatasetLabel::Unknown};
    std::string folderPath;      // absolute/relative path to the event folder
    std::string audioPath;
    std::string csvPath;
    std::string jsonPath;

    std::string eventType;       // "candidate" / "gunshot" / "ambient"
    std::string deviceName;
    std::string notes;

    uint64_t timestampMs{0};
    uint32_t sampleRate{48000};
    uint32_t fftSize{1024};
    uint32_t hopSize{512};
    uint64_t windowStartSample{0};
    uint64_t windowFrames{0};
    uint64_t featureRows{0};

    float detectorScore{0.0f};
    float candidateScore{0.0f};
    float confidence{0.0f};
    float triggerThreshold{0.0f};

    // Lazily-computed quality-check fields (populated on demand by
    // DatasetManager::RunQualityScan, not by Scan()).
    double durationMs{0.0};
    float peakAmplitude{0.0f};
    float rmsAmplitude{0.0f};
    uint64_t audioFileHash{0};
    bool qualityComputed{false};
};

} // namespace EchoRadar
