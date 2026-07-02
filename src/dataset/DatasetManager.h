#pragma once

#include "dataset/DatasetTypes.h"

#include <deque>
#include <optional>
#include <map>
#include <string>
#include <vector>

namespace EchoRadar {

// Result of an operation, with a human readable reason on failure so the
// GUI (or CLI, or a future Python binding) can surface something useful.
struct DatasetOpResult {
    bool ok{false};
    std::string message;

    static DatasetOpResult Success(std::string msg = "") { return {true, std::move(msg)}; }
    static DatasetOpResult Failure(std::string msg) { return {false, std::move(msg)}; }
};

// DatasetManager is the single source of truth for all filesystem mutations
// on the dataset/ tree. GUIs (gunshot_visualizer's Dataset Studio) and any
// future CLI/Python tooling MUST route Move/Delete/Restore/UpdateMetadata
// operations through this class instead of touching files directly. This
// keeps behaviour (undo history, trash, manifest, statistics) consistent
// across every consumer.
class DatasetManager {
public:
    explicit DatasetManager(std::string rootDir);

    // Rebuilds the in-memory index by walking dataset/<label>/<id>/metadata.json.
    // Safe to call repeatedly (e.g. on a "Refresh" button).
    DatasetOpResult Scan();

    const std::vector<DatasetEventRecord>& GetEvents() const { return m_events; }
    std::optional<DatasetEventRecord> GetEvent(const std::string& id) const;

    // label -> count, always includes all six canonical labels (possibly 0).
    std::map<std::string, size_t> GetStatistics() const;

    // Moves dataset/<oldLabel>/<id> -> dataset/<newLabel>/<id>, rewrites the
    // "label" field inside metadata.json, and pushes an undo entry.
    DatasetOpResult MoveLabel(const std::string& id, DatasetLabel newLabel);

    // Soft-delete: moves the event folder into dataset/.trash/<id>. Restore()
    // or Undo() can bring it back. Nothing is permanently removed here.
    DatasetOpResult Delete(const std::string& id);

    // Restores an event previously removed via Delete() back to the label
    // it had before deletion.
    DatasetOpResult Restore(const std::string& id);

    // Merges free-text notes into metadata.json (adds/updates "notes" key).
    DatasetOpResult UpdateNotes(const std::string& id, const std::string& notes);

    // Reverts the most recent Move/Delete/Notes operation. Keeps up to
    // kMaxUndoHistory entries.
    DatasetOpResult Undo();
    size_t UndoStackSize() const { return m_undoStack.size(); }

    // Quality checks. These read audio.wav (duration/peak/rms) lazily and
    // cache the result on the record, so repeated calls are cheap.
    std::vector<std::string> FindDuplicates();
    std::vector<std::string> FindEmpty();
    std::vector<std::string> FindVeryShort(double minDurationMs = 50.0);
    std::vector<std::string> FindClipped(float clipThreshold = 0.999f);

    const std::string& Root() const { return m_root; }

    static constexpr size_t kMaxUndoHistory = 20;

private:
    struct UndoAction {
        enum class Type { Move, Delete, Notes };
        Type type{Type::Move};
        std::string id;
        DatasetLabel fromLabel{DatasetLabel::Unknown};
        DatasetLabel toLabel{DatasetLabel::Unknown};
        std::string oldNotes;
    };

    void EnsureFolders() const;
    void ComputeQualityIfNeeded(DatasetEventRecord& record);
    int FindIndex(const std::string& id) const;
    bool WriteMetadataField(const DatasetEventRecord& record, const std::string& key, const std::string& value, bool quoted);
    bool RewriteLabelInMetadata(const std::string& jsonPath, const std::string& newLabel);
    bool RewriteNotesInMetadata(const std::string& jsonPath, const std::string& notes, std::string* oldNotes);
    void PushUndo(UndoAction action);

    std::string m_root;
    std::vector<DatasetEventRecord> m_events;
    std::deque<UndoAction> m_undoStack;
};

} // namespace EchoRadar
