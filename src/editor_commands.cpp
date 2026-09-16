#include "editor_commands.h"

namespace {

template <typename Layer>
int ReadLayer(const Layer &layer, uint64_t key) {
    const auto found = layer.find(key);
    return found == layer.end() ? 0 : static_cast<int>(found->second);
}

template <typename Layer>
bool WriteLayer(Layer &layer, uint64_t key, int value) {
    using StoredValue = typename Layer::mapped_type;
    const auto found = layer.find(key);
    const int oldValue = found == layer.end() ? 0 : static_cast<int>(found->second);
    if (oldValue == value) return false;
    if (value == 0) {
        if (found != layer.end()) layer.erase(found);
    } else if (found == layer.end()) {
        layer.emplace(key, static_cast<StoredValue>(value));
    } else {
        found->second = static_cast<StoredValue>(value);
    }
    return true;
}

template <typename Layer>
void ReserveAdditional(Layer &layer, std::size_t additionalTiles) {
    layer.reserve(layer.size() + additionalTiles);
}

} // namespace

namespace editor_commands {

int GetLayerValue(const EditTarget &target, PaintMode layer, uint64_t key) {
    if (target.dungeonMode) {
        if (!target.dungeon) return 0;
        if (layer == PaintMode::Terrain) return ReadLayer(target.dungeon->tiles, key);
        if (layer == PaintMode::Elevation) return ReadLayer(target.dungeon->elevation, key);
        if (layer == PaintMode::Fog) return ReadLayer(target.dungeon->fog, key);
        return 0;
    }
    if (layer == PaintMode::Terrain) return ReadLayer(target.document.terrain, key);
    if (layer == PaintMode::Region) return ReadLayer(target.document.regionsByTile, key);
    if (layer == PaintMode::Elevation) return ReadLayer(target.document.elevation, key);
    return ReadLayer(target.document.fog, key);
}

bool SetLayerValue(const EditTarget &target, PaintMode layer, uint64_t key, int value) {
    if (target.dungeonMode) {
        if (!target.dungeon) return false;
        if (layer == PaintMode::Terrain) return WriteLayer(target.dungeon->tiles, key, value);
        if (layer == PaintMode::Elevation) return WriteLayer(target.dungeon->elevation, key, value);
        if (layer == PaintMode::Fog) return WriteLayer(target.dungeon->fog, key, value);
        return false;
    }
    if (layer == PaintMode::Terrain) return WriteLayer(target.document.terrain, key, value);
    if (layer == PaintMode::Region) return WriteLayer(target.document.regionsByTile, key, value);
    if (layer == PaintMode::Elevation) return WriteLayer(target.document.elevation, key, value);
    return WriteLayer(target.document.fog, key, value);
}

void ReserveLayer(const EditTarget &target, PaintMode layer, std::size_t additionalTiles) {
    if (target.dungeonMode) {
        if (!target.dungeon) return;
        if (layer == PaintMode::Terrain) ReserveAdditional(target.dungeon->tiles, additionalTiles);
        else if (layer == PaintMode::Elevation)
            ReserveAdditional(target.dungeon->elevation, additionalTiles);
        else if (layer == PaintMode::Fog)
            ReserveAdditional(target.dungeon->fog, additionalTiles);
        return;
    }
    if (layer == PaintMode::Terrain) ReserveAdditional(target.document.terrain, additionalTiles);
    else if (layer == PaintMode::Region)
        ReserveAdditional(target.document.regionsByTile, additionalTiles);
    else if (layer == PaintMode::Elevation)
        ReserveAdditional(target.document.elevation, additionalTiles);
    else
        ReserveAdditional(target.document.fog, additionalTiles);
}

} // namespace editor_commands
