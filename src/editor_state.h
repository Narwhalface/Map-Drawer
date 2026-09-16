#pragma once

#include "editor_history.h"
#include "project_document.h"

#include <cstdint>
#include <string>
#include <vector>

// State shared by editor commands independently of the GLFW/OpenGL shell.
// Window, pointer, modal, and camera state remain in the application layer.
struct EditorState {
    ProjectDocument document;
    EditorHistory history;
    bool dirty = false;
    uint64_t sceneRevision = 1;
    std::string lastSearchQuery;
    std::string lastFoundLabel;
    std::vector<SearchResult> searchMatches;

    void MarkDirty();
    void ResetForLoadedDocument();
};
