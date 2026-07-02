#include "dataset/DatasetManager.h"
#include "dataset/DatasetJson.h"
#include "dataset/DatasetWavStats.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace EchoRadar {

namespace {

constexpr std::array<DatasetLabel, 6> kAllLabels = {
    DatasetLabel::Gunshot,
    DatasetLabel::Footstep,
    DatasetLabel::Reload,
    DatasetLabel::Switch,
    DatasetLabel::Ambient,
    DatasetLabel::Unknown,
};

} // namespace

DatasetManager::DatasetManager(std::string rootDir) : m_root(std::move(rootDir)) {
    EnsureFolders();
}

void DatasetManager::EnsureFolders() const {
    std::error_code ec;
    fs::create_directories(m_root, ec);
    for (DatasetLabel label : kAllLabels) {
        fs::create_directories(fs::path(m_root) / ToString(label), ec);
    }
    fs::create_directories(fs::path(m_root) / ".trash", ec);
}

DatasetOpResult DatasetManager::Scan() {
    m_events.clear();
    EnsureFolders();

    for (DatasetLabel label : kAllLabels) {
        const fs::path labelDir = fs::path(m_root) / ToString(label);
        if (!fs::exists(labelDir)) {
            continue;
        }
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(labelDir, ec)) {
            if (!entry.is_directory()) {
                continue;
            }
            const fs::path jsonPath = entry.path() / "metadata.json";
            const std::string text = detail::ReadFileToString(jsonPath.string());
            if (text.empty()) {
                continue;
            }
            const auto kv = detail::ParseFlatJson(text);

            DatasetEventRecord record;
            record.id = entry.path().filename().string();
            record.label = label;
            record.folderPath = entry.path().string();
            record.audioPath = (entry.path() / "audio.wav").string();
            record.csvPath = (entry.path() / "features.csv").string();
            record.jsonPath = jsonPath.string();

            record.eventType = detail::GetStr(kv, "event_type");
            record.deviceName = detail::GetStr(kv, "device_name");
            record.notes = detail::GetStr(kv, "notes");
            record.timestampMs = detail::GetU64(kv, "timestamp_ms");
            record.sampleRate = static_cast<uint32_t>(detail::GetU64(kv, "sample_rate", 48000));
            record.fftSize = static_cast<uint32_t>(detail::GetU64(kv, "fft_size", 1024));
            record.hopSize = static_cast<uint32_t>(detail::GetU64(kv, "hop_size", 512));
            record.windowStartSample = detail::GetU64(kv, "window_start_sample");
            record.windowFrames = detail::GetU64(kv, "window_frames");
            record.featureRows = detail::GetU64(kv, "feature_rows");
            record.detectorScore = detail::GetFloatVal(kv, "detector_score");
            record.candidateScore = detail::GetFloatVal(kv, "candidate_score");
            record.confidence = detail::GetFloatVal(kv, "confidence");
            record.triggerThreshold = detail::GetFloatVal(kv, "trigger_threshold");

            m_events.push_back(std::move(record));
        }
    }

    std::sort(m_events.begin(), m_events.end(), [](const DatasetEventRecord& a, const DatasetEventRecord& b) {
        if (a.timestampMs != b.timestampMs) return a.timestampMs < b.timestampMs;
        return a.id < b.id;
    });

    return DatasetOpResult::Success();
}

std::optional<DatasetEventRecord> DatasetManager::GetEvent(const std::string& id) const {
    const int idx = FindIndex(id);
    if (idx < 0) return std::nullopt;
    return m_events[static_cast<size_t>(idx)];
}

int DatasetManager::FindIndex(const std::string& id) const {
    for (size_t i = 0; i < m_events.size(); ++i) {
        if (m_events[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

std::map<std::string, size_t> DatasetManager::GetStatistics() const {
    std::map<std::string, size_t> stats;
    for (DatasetLabel label : kAllLabels) {
        stats[ToString(label)] = 0;
    }
    for (const auto& e : m_events) {
        stats[ToString(e.label)]++;
    }
    return stats;
}

bool DatasetManager::RewriteLabelInMetadata(const std::string& jsonPath, const std::string& newLabel) {
    const std::string text = detail::ReadFileToString(jsonPath);
    if (text.empty()) return false;
    auto kv = detail::ParseFlatJson(text);
    kv["label"] = newLabel;

    std::ofstream out(jsonPath);
    if (!out) return false;
    out << "{\n";
    bool first = true;
    for (const auto& [key, value] : kv) {
        if (!first) out << ",\n";
        first = false;
        // Heuristic: keys we know are numeric are written unquoted; everything
        // else (including our own additions like "notes") is quoted.
        static const std::unordered_map<std::string, bool> kNumericKeys = {
            {"timestamp_ms", true}, {"sample_rate", true}, {"fft_size", true},
            {"hop_size", true}, {"window_start_sample", true}, {"window_frames", true},
            {"feature_rows", true}, {"detector_score", true}, {"candidate_score", true},
            {"confidence", true}, {"trigger_threshold", true},
        };
        if (kNumericKeys.count(key)) {
            out << "  \"" << key << "\": " << value;
        } else {
            out << "  \"" << key << "\": \"" << detail::JsonEscapeStr(value) << "\"";
        }
    }
    out << "\n}\n";
    return out.good();
}

bool DatasetManager::RewriteNotesInMetadata(const std::string& jsonPath, const std::string& notes, std::string* oldNotes) {
    const std::string text = detail::ReadFileToString(jsonPath);
    if (text.empty()) return false;
    auto kv = detail::ParseFlatJson(text);
    if (oldNotes) *oldNotes = detail::GetStr(kv, "notes");
    kv["notes"] = notes;

    std::ofstream out(jsonPath);
    if (!out) return false;
    out << "{\n";
    bool first = true;
    for (const auto& [key, value] : kv) {
        if (!first) out << ",\n";
        first = false;
        static const std::unordered_map<std::string, bool> kNumericKeys = {
            {"timestamp_ms", true}, {"sample_rate", true}, {"fft_size", true},
            {"hop_size", true}, {"window_start_sample", true}, {"window_frames", true},
            {"feature_rows", true}, {"detector_score", true}, {"candidate_score", true},
            {"confidence", true}, {"trigger_threshold", true},
        };
        if (kNumericKeys.count(key)) {
            out << "  \"" << key << "\": " << value;
        } else {
            out << "  \"" << key << "\": \"" << detail::JsonEscapeStr(value) << "\"";
        }
    }
    out << "\n}\n";
    return out.good();
}

void DatasetManager::PushUndo(UndoAction action) {
    m_undoStack.push_back(std::move(action));
    while (m_undoStack.size() > kMaxUndoHistory) {
        m_undoStack.pop_front();
    }
}

DatasetOpResult DatasetManager::MoveLabel(const std::string& id, DatasetLabel newLabel) {
    const int idx = FindIndex(id);
    if (idx < 0) {
        return DatasetOpResult::Failure("Unknown event id: " + id);
    }
    DatasetEventRecord& record = m_events[static_cast<size_t>(idx)];
    if (record.label == newLabel) {
        return DatasetOpResult::Success("Already labeled " + std::string(ToString(newLabel)));
    }

    const fs::path src = record.folderPath;
    const fs::path dstDir = fs::path(m_root) / ToString(newLabel);
    const fs::path dst = dstDir / id;

    std::error_code ec;
    fs::create_directories(dstDir, ec);
    if (fs::exists(dst)) {
        return DatasetOpResult::Failure("Destination already exists: " + dst.string());
    }
    fs::rename(src, dst, ec);
    if (ec) {
        return DatasetOpResult::Failure("Move failed: " + ec.message());
    }

    const std::string oldLabelStr = ToString(record.label);
    RewriteLabelInMetadata((dst / "metadata.json").string(), ToString(newLabel));

    PushUndo(UndoAction{UndoAction::Type::Move, id, record.label, newLabel, ""});

    record.label = newLabel;
    record.folderPath = dst.string();
    record.audioPath = (dst / "audio.wav").string();
    record.csvPath = (dst / "features.csv").string();
    record.jsonPath = (dst / "metadata.json").string();

    return DatasetOpResult::Success("Moved " + id + " from " + oldLabelStr + " to " + ToString(newLabel));
}

DatasetOpResult DatasetManager::Delete(const std::string& id) {
    const int idx = FindIndex(id);
    if (idx < 0) {
        return DatasetOpResult::Failure("Unknown event id: " + id);
    }
    DatasetEventRecord& record = m_events[static_cast<size_t>(idx)];

    const fs::path src = record.folderPath;
    const fs::path trashDir = fs::path(m_root) / ".trash";
    const fs::path dst = trashDir / id;

    std::error_code ec;
    fs::create_directories(trashDir, ec);
    if (fs::exists(dst)) {
        fs::remove_all(dst, ec);
    }
    fs::rename(src, dst, ec);
    if (ec) {
        return DatasetOpResult::Failure("Delete failed: " + ec.message());
    }

    PushUndo(UndoAction{UndoAction::Type::Delete, id, record.label, DatasetLabel::Unknown, ""});

    m_events.erase(m_events.begin() + idx);
    return DatasetOpResult::Success("Deleted " + id + " (moved to .trash, recoverable via Undo/Restore)");
}

DatasetOpResult DatasetManager::Restore(const std::string& id) {
    const fs::path trashDir = fs::path(m_root) / ".trash";
    const fs::path src = trashDir / id;
    if (!fs::exists(src)) {
        return DatasetOpResult::Failure("Not found in trash: " + id);
    }

    const std::string text = detail::ReadFileToString((src / "metadata.json").string());
    const auto kv = detail::ParseFlatJson(text);
    const DatasetLabel restoredLabel = LabelFromString(detail::GetStr(kv, "label", "unknown"));

    const fs::path dstDir = fs::path(m_root) / ToString(restoredLabel);
    const fs::path dst = dstDir / id;

    std::error_code ec;
    fs::create_directories(dstDir, ec);
    fs::rename(src, dst, ec);
    if (ec) {
        return DatasetOpResult::Failure("Restore failed: " + ec.message());
    }

    DatasetEventRecord record;
    record.id = id;
    record.label = restoredLabel;
    record.folderPath = dst.string();
    record.audioPath = (dst / "audio.wav").string();
    record.csvPath = (dst / "features.csv").string();
    record.jsonPath = (dst / "metadata.json").string();
    record.eventType = detail::GetStr(kv, "event_type");
    record.deviceName = detail::GetStr(kv, "device_name");
    record.notes = detail::GetStr(kv, "notes");
    record.timestampMs = detail::GetU64(kv, "timestamp_ms");
    record.sampleRate = static_cast<uint32_t>(detail::GetU64(kv, "sample_rate", 48000));
    record.fftSize = static_cast<uint32_t>(detail::GetU64(kv, "fft_size", 1024));
    record.hopSize = static_cast<uint32_t>(detail::GetU64(kv, "hop_size", 512));
    record.windowStartSample = detail::GetU64(kv, "window_start_sample");
    record.windowFrames = detail::GetU64(kv, "window_frames");
    record.featureRows = detail::GetU64(kv, "feature_rows");
    record.detectorScore = detail::GetFloatVal(kv, "detector_score");
    record.candidateScore = detail::GetFloatVal(kv, "candidate_score");
    record.confidence = detail::GetFloatVal(kv, "confidence");
    record.triggerThreshold = detail::GetFloatVal(kv, "trigger_threshold");

    m_events.push_back(record);
    return DatasetOpResult::Success("Restored " + id + " to " + ToString(restoredLabel));
}

DatasetOpResult DatasetManager::UpdateNotes(const std::string& id, const std::string& notes) {
    const int idx = FindIndex(id);
    if (idx < 0) {
        return DatasetOpResult::Failure("Unknown event id: " + id);
    }
    DatasetEventRecord& record = m_events[static_cast<size_t>(idx)];

    std::string oldNotes;
    if (!RewriteNotesInMetadata(record.jsonPath, notes, &oldNotes)) {
        return DatasetOpResult::Failure("Failed to write metadata.json for " + id);
    }

    PushUndo(UndoAction{UndoAction::Type::Notes, id, record.label, record.label, oldNotes});
    record.notes = notes;
    return DatasetOpResult::Success("Updated notes for " + id);
}

DatasetOpResult DatasetManager::Undo() {
    if (m_undoStack.empty()) {
        return DatasetOpResult::Failure("Nothing to undo");
    }
    UndoAction action = m_undoStack.back();
    m_undoStack.pop_back();

    switch (action.type) {
    case UndoAction::Type::Move: {
        const int idx = FindIndex(action.id);
        if (idx < 0) return DatasetOpResult::Failure("Undo failed: event not found");
        DatasetEventRecord& record = m_events[static_cast<size_t>(idx)];
        const fs::path src = record.folderPath;
        const fs::path dstDir = fs::path(m_root) / ToString(action.fromLabel);
        const fs::path dst = dstDir / action.id;
        std::error_code ec;
        fs::create_directories(dstDir, ec);
        fs::rename(src, dst, ec);
        if (ec) return DatasetOpResult::Failure("Undo move failed: " + ec.message());
        RewriteLabelInMetadata((dst / "metadata.json").string(), ToString(action.fromLabel));
        record.label = action.fromLabel;
        record.folderPath = dst.string();
        record.audioPath = (dst / "audio.wav").string();
        record.csvPath = (dst / "features.csv").string();
        record.jsonPath = (dst / "metadata.json").string();
        return DatasetOpResult::Success("Undid move of " + action.id);
    }
    case UndoAction::Type::Delete: {
        const auto result = Restore(action.id);
        return result.ok ? DatasetOpResult::Success("Undid delete of " + action.id)
                          : DatasetOpResult::Failure("Undo delete failed: " + result.message);
    }
    case UndoAction::Type::Notes: {
        const int idx = FindIndex(action.id);
        if (idx < 0) return DatasetOpResult::Failure("Undo failed: event not found");
        DatasetEventRecord& record = m_events[static_cast<size_t>(idx)];
        RewriteNotesInMetadata(record.jsonPath, action.oldNotes, nullptr);
        record.notes = action.oldNotes;
        return DatasetOpResult::Success("Undid notes edit for " + action.id);
    }
    }
    return DatasetOpResult::Failure("Unknown undo action");
}

void DatasetManager::ComputeQualityIfNeeded(DatasetEventRecord& record) {
    if (record.qualityComputed) return;
    const detail::WavInfo info = detail::ReadWavQuickStats(record.audioPath);
    if (info.ok) {
        record.durationMs = info.durationMs;
        record.peakAmplitude = info.peak;
        record.rmsAmplitude = info.rms;
    }
    record.audioFileHash = detail::HashFileFnv1a(record.audioPath);
    record.qualityComputed = true;
}

std::vector<std::string> DatasetManager::FindDuplicates() {
    std::unordered_map<uint64_t, std::vector<std::string>> byHash;
    for (auto& record : m_events) {
        ComputeQualityIfNeeded(record);
        if (record.audioFileHash != 0) {
            byHash[record.audioFileHash].push_back(record.id);
        }
    }
    std::vector<std::string> dupes;
    for (const auto& [hash, ids] : byHash) {
        if (ids.size() > 1) {
            dupes.insert(dupes.end(), ids.begin(), ids.end());
        }
    }
    std::sort(dupes.begin(), dupes.end());
    return dupes;
}

std::vector<std::string> DatasetManager::FindEmpty() {
    std::vector<std::string> out;
    for (auto& record : m_events) {
        ComputeQualityIfNeeded(record);
        if (record.windowFrames == 0 || record.durationMs <= 0.0) {
            out.push_back(record.id);
        }
    }
    return out;
}

std::vector<std::string> DatasetManager::FindVeryShort(double minDurationMs) {
    std::vector<std::string> out;
    for (auto& record : m_events) {
        ComputeQualityIfNeeded(record);
        if (record.durationMs > 0.0 && record.durationMs < minDurationMs) {
            out.push_back(record.id);
        }
    }
    return out;
}

std::vector<std::string> DatasetManager::FindClipped(float clipThreshold) {
    std::vector<std::string> out;
    for (auto& record : m_events) {
        ComputeQualityIfNeeded(record);
        if (record.peakAmplitude >= clipThreshold) {
            out.push_back(record.id);
        }
    }
    return out;
}

} // namespace EchoRadar
