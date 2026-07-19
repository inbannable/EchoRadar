#include "dataset/DatasetManager.h"
#include "dataset/DatasetJson.h"
#include "dataset/DatasetWavStats.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
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

std::string CsvEscape(const std::string& value) {
    if (value.find_first_of(",\"\r\n") == std::string::npos) {
        return value;
    }
    std::string escaped{"\""};
    for (char c : value) {
        if (c == '"') escaped += "\"\"";
        else escaped.push_back(c);
    }
    escaped.push_back('"');
    return escaped;
}

bool WriteFlatMetadata(const std::string& path,
                       const std::map<std::string, std::string>& values) {
    static const std::unordered_map<std::string, bool> kUnquotedKeys = {
        {"timestamp_ms", true}, {"sample_rate", true}, {"fft_size", true},
        {"hop_size", true}, {"window_start_sample", true}, {"window_frames", true},
        {"feature_rows", true}, {"detector_score", true}, {"candidate_score", true},
        {"confidence", true}, {"trigger_threshold", true}, {"reviewed", true},
    };

    static std::atomic<uint64_t> metadataSequence{0};
    const std::string suffix = std::to_string(metadataSequence.fetch_add(1, std::memory_order_relaxed));
    const fs::path destination(path);
    const fs::path temporary(path + ".tmp." + suffix);
    const fs::path backup(path + ".bak." + suffix);

    {
        std::ofstream out(temporary);
        if (!out) return false;
        out << "{\n";
        bool first = true;
        for (const auto& [key, value] : values) {
            if (!first) out << ",\n";
            first = false;
            out << "  \"" << key << "\": ";
            if (kUnquotedKeys.count(key)) out << value;
            else out << '"' << detail::JsonEscapeStr(value) << '"';
        }
        out << "\n}\n";
        if (!out.good()) {
            out.close();
            std::error_code cleanupError;
            fs::remove(temporary, cleanupError);
            return false;
        }
    }

    std::error_code ec;
    fs::rename(destination, backup, ec);
    if (ec) {
        fs::remove(temporary, ec);
        return false;
    }
    fs::rename(temporary, destination, ec);
    if (ec) {
        std::error_code restoreError;
        fs::rename(backup, destination, restoreError);
        fs::remove(temporary, restoreError);
        return false;
    }
    fs::remove(backup, ec);
    return true;
}

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
    fs::create_directories(fs::path(m_root) / ".pending", ec);
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
            record.decision = detail::GetStr(kv, "decision");
            record.sessionId = detail::GetStr(kv, "session_id");
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
            record.reviewed = detail::GetBoolVal(kv, "reviewed");

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

bool DatasetManager::IsSafeEventId(const std::string& id) {
    if (id.empty() || id.size() > 96) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '-' || c == '_';
    });
}

bool DatasetManager::EventIdExists(const std::string& id) const {
    if (!IsSafeEventId(id)) return true;
    for (DatasetLabel label : kAllLabels) {
        if (fs::exists(fs::path(m_root) / ToString(label) / id)) return true;
    }
    return fs::exists(fs::path(m_root) / ".trash" / id) ||
           fs::exists(fs::path(m_root) / ".pending" / (id + ".tmp"));
}

std::string DatasetManager::GenerateUniqueEventId() const {
    static std::atomic<uint64_t> sequence{0};
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const auto wallNow = std::chrono::system_clock::now().time_since_epoch();
        const auto steadyNow = std::chrono::steady_clock::now().time_since_epoch();
        const uint64_t wallMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(wallNow).count());
        const uint64_t entropy = static_cast<uint64_t>(steadyNow.count());
        const uint64_t seq = sequence.fetch_add(1, std::memory_order_relaxed);

        std::ostringstream id;
        id << "evt_" << wallMs << '_' << std::hex << std::setw(12) << std::setfill('0')
           << (entropy & 0xFFFFFFFFFFFFull) << '_' << std::setw(4) << (seq & 0xFFFFull);
        if (!EventIdExists(id.str())) return id.str();
    }
    return {};
}

DatasetOpResult DatasetManager::PublishEvent(const std::string& id,
                                             DatasetLabel label,
                                             const EventDirectoryWriter& writer) {
    if (!writer) return DatasetOpResult::Failure("Event writer callback is missing");
    if (!IsSafeEventId(id)) return DatasetOpResult::Failure("Unsafe event id: " + id);

    EnsureFolders();
    if (EventIdExists(id)) return DatasetOpResult::Failure("Event id already exists: " + id);

    const fs::path pendingRoot = fs::path(m_root) / ".pending";
    const fs::path staging = pendingRoot / (id + ".tmp");
    const fs::path destination = fs::path(m_root) / ToString(label) / id;
    std::error_code ec;
    if (!fs::create_directory(staging, ec) || ec) {
        return DatasetOpResult::Failure("Could not create event staging directory: " + ec.message());
    }

    const auto cleanupStaging = [&]() {
        std::error_code cleanupError;
        fs::remove_all(staging, cleanupError);
    };

    DatasetOpResult writeResult;
    try {
        writeResult = writer(staging);
    } catch (const std::exception& ex) {
        cleanupStaging();
        return DatasetOpResult::Failure(std::string("Event writer failed: ") + ex.what());
    } catch (...) {
        cleanupStaging();
        return DatasetOpResult::Failure("Event writer failed with an unknown error");
    }
    if (!writeResult.ok) {
        cleanupStaging();
        return writeResult;
    }

    if (fs::exists(destination)) {
        cleanupStaging();
        return DatasetOpResult::Failure("Event destination already exists: " + destination.string());
    }
    fs::rename(staging, destination, ec);
    if (ec) {
        cleanupStaging();
        return DatasetOpResult::Failure("Could not publish event: " + ec.message());
    }
    return DatasetOpResult::Success("Published " + id);
}

DatasetOpResult DatasetManager::ExportManifest(const std::string& outputPath) {
    const DatasetOpResult scanResult = Scan();
    if (!scanResult.ok) return scanResult;

    const fs::path path = outputPath.empty() ? fs::path(m_root) / "manifest.csv"
                                             : fs::path(outputPath);
    std::ofstream out(path);
    if (!out) return DatasetOpResult::Failure("Could not open manifest: " + path.string());

    out << "label,event_id,event_type,decision,session_id,reviewed,timestamp_ms,audio_path,csv_path,json_path,candidate_score,confidence,notes\n";
    std::error_code ec;
    for (const auto& record : m_events) {
        auto relativeOrOriginal = [&](const std::string& original) {
            const fs::path relative = fs::relative(original, m_root, ec);
            if (ec) {
                ec.clear();
                return original;
            }
            return relative.generic_string();
        };
        out << CsvEscape(ToString(record.label)) << ','
            << CsvEscape(record.id) << ','
            << CsvEscape(record.eventType) << ','
            << CsvEscape(record.decision) << ','
            << CsvEscape(record.sessionId) << ','
            << (record.reviewed ? "true" : "false") << ','
            << record.timestampMs << ','
            << CsvEscape(relativeOrOriginal(record.audioPath)) << ','
            << CsvEscape(relativeOrOriginal(record.csvPath)) << ','
            << CsvEscape(relativeOrOriginal(record.jsonPath)) << ','
            << record.candidateScore << ','
            << record.confidence << ','
            << CsvEscape(record.notes) << '\n';
    }
    if (!out.good()) return DatasetOpResult::Failure("Failed while writing manifest: " + path.string());
    return DatasetOpResult::Success("Exported " + std::to_string(m_events.size()) + " events to " + path.string());
}

bool DatasetManager::RewriteLabelInMetadata(const std::string& jsonPath,
                                            const std::string& newLabel,
                                            bool reviewed) {
    const std::string text = detail::ReadFileToString(jsonPath);
    if (text.empty()) return false;
    auto kv = detail::ParseFlatJson(text);
    kv["label"] = newLabel;
    kv["reviewed"] = reviewed ? "true" : "false";
    return WriteFlatMetadata(jsonPath, kv);
}

bool DatasetManager::RewriteNotesInMetadata(const std::string& jsonPath, const std::string& notes, std::string* oldNotes) {
    const std::string text = detail::ReadFileToString(jsonPath);
    if (text.empty()) return false;
    auto kv = detail::ParseFlatJson(text);
    if (oldNotes) *oldNotes = detail::GetStr(kv, "notes");
    kv["notes"] = notes;

    return WriteFlatMetadata(jsonPath, kv);
}

bool DatasetManager::RewriteReviewedInMetadata(const std::string& jsonPath, bool reviewed) {
    const std::string text = detail::ReadFileToString(jsonPath);
    if (text.empty()) return false;
    auto kv = detail::ParseFlatJson(text);
    kv["reviewed"] = reviewed ? "true" : "false";
    return WriteFlatMetadata(jsonPath, kv);
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
        if (record.reviewed) {
            return DatasetOpResult::Success("Already labeled and reviewed as " + std::string(ToString(newLabel)));
        }
        return SetReviewed(id, true);
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
    if (!RewriteLabelInMetadata((dst / "metadata.json").string(), ToString(newLabel))) {
        std::error_code rollbackError;
        fs::rename(dst, src, rollbackError);
        const std::string detail = rollbackError ? "; rollback also failed: " + rollbackError.message() : "";
        return DatasetOpResult::Failure("Could not update metadata after move" + detail);
    }

    PushUndo(UndoAction{UndoAction::Type::Move, id, record.label, newLabel, "", record.reviewed});

    record.label = newLabel;
    record.folderPath = dst.string();
    record.audioPath = (dst / "audio.wav").string();
    record.csvPath = (dst / "features.csv").string();
    record.jsonPath = (dst / "metadata.json").string();
    record.reviewed = true;

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
        return DatasetOpResult::Failure("Trash already contains event id: " + id);
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

    if (fs::exists(dst)) {
        return DatasetOpResult::Failure("Restore destination already exists: " + dst.string());
    }

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
    record.decision = detail::GetStr(kv, "decision");
    record.sessionId = detail::GetStr(kv, "session_id");
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
    record.reviewed = detail::GetBoolVal(kv, "reviewed");

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

DatasetOpResult DatasetManager::SetReviewed(const std::string& id, bool reviewed) {
    const int idx = FindIndex(id);
    if (idx < 0) return DatasetOpResult::Failure("Unknown event id: " + id);
    DatasetEventRecord& record = m_events[static_cast<size_t>(idx)];
    if (record.reviewed == reviewed) {
        return DatasetOpResult::Success(reviewed ? "Already reviewed" : "Already unreviewed");
    }
    if (!RewriteReviewedInMetadata(record.jsonPath, reviewed)) {
        return DatasetOpResult::Failure("Failed to update review status for " + id);
    }
    PushUndo(UndoAction{UndoAction::Type::Reviewed,
                        id,
                        record.label,
                        record.label,
                        "",
                        record.reviewed});
    record.reviewed = reviewed;
    return DatasetOpResult::Success(reviewed ? "Marked " + id + " reviewed"
                                             : "Marked " + id + " unreviewed");
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
        if (fs::exists(dst)) return DatasetOpResult::Failure("Undo destination already exists: " + dst.string());
        if (!RewriteLabelInMetadata((src / "metadata.json").string(),
                                    ToString(action.fromLabel),
                                    action.oldReviewed)) {
            return DatasetOpResult::Failure("Undo move could not restore metadata");
        }
        fs::rename(src, dst, ec);
        if (ec) {
            RewriteLabelInMetadata((src / "metadata.json").string(),
                                   ToString(record.label),
                                   record.reviewed);
            return DatasetOpResult::Failure("Undo move failed: " + ec.message());
        }
        record.label = action.fromLabel;
        record.folderPath = dst.string();
        record.audioPath = (dst / "audio.wav").string();
        record.csvPath = (dst / "features.csv").string();
        record.jsonPath = (dst / "metadata.json").string();
        record.reviewed = action.oldReviewed;
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
    case UndoAction::Type::Reviewed: {
        const int idx = FindIndex(action.id);
        if (idx < 0) return DatasetOpResult::Failure("Undo failed: event not found");
        DatasetEventRecord& record = m_events[static_cast<size_t>(idx)];
        if (!RewriteReviewedInMetadata(record.jsonPath, action.oldReviewed)) {
            return DatasetOpResult::Failure("Undo review status failed for " + action.id);
        }
        record.reviewed = action.oldReviewed;
        return DatasetOpResult::Success("Undid review status for " + action.id);
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
