#pragma once

#include "project_document.h"

#include <cstddef>
#include <cstdint>

struct EditTarget {
    ProjectDocument &document;
    Dungeon *dungeon = nullptr;
    bool dungeonMode = false;
};

namespace editor_commands {

int GetLayerValue(const EditTarget &target, PaintMode layer, uint64_t key);
bool SetLayerValue(const EditTarget &target, PaintMode layer, uint64_t key, int value);
void ReserveLayer(const EditTarget &target, PaintMode layer, std::size_t additionalTiles);

} // namespace editor_commands
