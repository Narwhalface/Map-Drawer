#include "editor_state.h"

void EditorState::MarkDirty() {
    dirty = true;
    searchMatches.clear();
    lastSearchQuery.clear();
    lastFoundLabel.clear();
}

void EditorState::ResetForLoadedDocument() {
    history.Clear();
    searchMatches.clear();
    lastSearchQuery.clear();
    lastFoundLabel.clear();
    ++sceneRevision;
    dirty = false;
}
