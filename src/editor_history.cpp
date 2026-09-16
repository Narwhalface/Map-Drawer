#include "editor_history.h"

#include "app_config.h"

#include <algorithm>
#include <utility>

void EditorHistory::BeginStroke(PaintMode layer, std::size_t expectedTiles) {
    strokeActive_ = true;
    strokeChanged_ = false;
    strokeLayer_ = layer;
    strokeOriginal_.clear();
    if (expectedTiles > 0)
        strokeOriginal_.reserve(std::min(expectedTiles, kBulkReserveLimit));
}

void EditorHistory::RecordOriginal(uint64_t key, int oldValue) {
    if (!strokeActive_) return;
    const auto [it, inserted] = strokeOriginal_.emplace(key, static_cast<int16_t>(oldValue));
    (void)it;
    if (inserted) strokeChanged_ = true;
}

bool EditorHistory::HasRecorded(uint64_t key) const {
    return strokeOriginal_.count(key) != 0;
}

bool EditorHistory::EndStroke(const GetValue &getValue) {
    if (!strokeActive_) return false;
    strokeActive_ = false;
    if (!strokeChanged_ || strokeOriginal_.empty()) return false;

    StrokeRecord record;
    record.layer = strokeLayer_;
    record.changes.reserve(strokeOriginal_.size());
    for (const auto &[key, oldValue] : strokeOriginal_) {
        record.changes.push_back(
            {key, oldValue, static_cast<int16_t>(getValue(strokeLayer_, key))});
    }
    strokeOriginal_.clear();
    undoStack_.push_back(std::move(record));
    if (undoStack_.size() > kMaxUndoStrokes) undoStack_.erase(undoStack_.begin());
    redoStack_.clear();
    return true;
}

void EditorHistory::CancelStroke() {
    strokeActive_ = false;
    strokeChanged_ = false;
    strokeOriginal_.clear();
}

void EditorHistory::Clear() {
    CancelStroke();
    undoStack_.clear();
    redoStack_.clear();
}

std::optional<HistoryResult> EditorHistory::Apply(std::vector<StrokeRecord> &source,
                                                   std::vector<StrokeRecord> &destination,
                                                   bool useNewValue,
                                                   const SetValue &setValue) {
    if (source.empty()) return std::nullopt;
    StrokeRecord record = std::move(source.back());
    source.pop_back();
    for (const TileChange &change : record.changes) {
        setValue(record.layer, change.key, useNewValue ? change.newValue : change.oldValue);
    }
    HistoryResult result{record.layer, record.changes.size()};
    destination.push_back(std::move(record));
    return result;
}

std::optional<HistoryResult> EditorHistory::Undo(const SetValue &setValue) {
    return Apply(undoStack_, redoStack_, false, setValue);
}

std::optional<HistoryResult> EditorHistory::Redo(const SetValue &setValue) {
    return Apply(redoStack_, undoStack_, true, setValue);
}
