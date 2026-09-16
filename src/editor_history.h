#pragma once

#include "app_types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

struct HistoryResult {
    PaintMode layer = PaintMode::Terrain;
    std::size_t tileCount = 0;
};

// Owns stroke recording and undo/redo state. Document mutation stays in the
// editor command layer through callbacks, so this class is independent of UI,
// GLFW, OpenGL, and whether the active document is a world or dungeon.
class EditorHistory {
public:
    using GetValue = std::function<int(PaintMode, uint64_t)>;
    using SetValue = std::function<void(PaintMode, uint64_t, int)>;

    void BeginStroke(PaintMode layer, std::size_t expectedTiles = 0);
    void RecordOriginal(uint64_t key, int oldValue);
    bool HasRecorded(uint64_t key) const;
    bool EndStroke(const GetValue &getValue);
    void CancelStroke();
    void Clear();

    std::optional<HistoryResult> Undo(const SetValue &setValue);
    std::optional<HistoryResult> Redo(const SetValue &setValue);

    bool StrokeActive() const { return strokeActive_; }
    bool StrokeChanged() const { return strokeChanged_; }
    PaintMode StrokeLayer() const { return strokeLayer_; }
    std::size_t UndoCount() const { return undoStack_.size(); }
    std::size_t RedoCount() const { return redoStack_.size(); }

private:
    std::optional<HistoryResult> Apply(std::vector<StrokeRecord> &source,
                                       std::vector<StrokeRecord> &destination,
                                       bool useNewValue, const SetValue &setValue);

    std::vector<StrokeRecord> undoStack_;
    std::vector<StrokeRecord> redoStack_;
    bool strokeActive_ = false;
    bool strokeChanged_ = false;
    PaintMode strokeLayer_ = PaintMode::Terrain;
    std::unordered_map<uint64_t, int16_t> strokeOriginal_;
};
