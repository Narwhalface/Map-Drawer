#include "app_types.h"

#include <algorithm>

const Vec3 kRegionPalette[kRegionPaletteSize] = {
    {0.85f, 0.20f, 0.20f}, {0.20f, 0.45f, 0.85f}, {0.85f, 0.65f, 0.15f},
    {0.55f, 0.20f, 0.75f}, {0.20f, 0.75f, 0.55f}, {0.85f, 0.35f, 0.60f},
    {0.45f, 0.80f, 0.20f}, {0.30f, 0.30f, 0.90f}, {0.90f, 0.50f, 0.20f},
    {0.20f, 0.85f, 0.85f},
};

const Vec3 kTerrainColors[kTerrainCount] = {
    {0.15f, 0.15f, 0.17f}, {0.45f, 0.65f, 0.25f}, {0.13f, 0.35f, 0.13f},
    {0.15f, 0.45f, 0.75f}, {0.55f, 0.52f, 0.50f}, {0.85f, 0.75f, 0.45f},
    {0.55f, 0.55f, 0.25f}, {0.55f, 0.40f, 0.22f},
};

const char *const kTerrainNames[kTerrainCount] = {
    "Empty", "Plains", "Forest", "Water", "Mountain", "Desert", "Hills", "Road",
};

std::vector<TerrainDefinition> DefaultTerrainDefinitions() {
    std::vector<TerrainDefinition> definitions;
    definitions.reserve(kTerrainCount);
    for (int index = 0; index < kTerrainCount; ++index)
        definitions.push_back({kTerrainNames[index], kTerrainColors[index]});
    return definitions;
}

std::vector<TerrainDefinition> DefaultDungeonTerrainDefinitions() {
    return {
        {"Erase", {0.08f, 0.09f, 0.11f}},
        {"Floor", {0.48f, 0.45f, 0.40f}},
        {"Wall", {0.20f, 0.22f, 0.26f}},
        {"Door", {0.58f, 0.32f, 0.12f}},
        {"Water", {0.12f, 0.38f, 0.68f}},
        {"Trap", {0.72f, 0.18f, 0.16f}},
    };
}

const char *DungeonTileKindName(DungeonTileKind kind) {
    switch (kind) {
        case DungeonTileKind::Empty: return "Erase";
        case DungeonTileKind::Floor: return "Floor";
        case DungeonTileKind::Wall: return "Wall";
        case DungeonTileKind::Door: return "Door";
        case DungeonTileKind::Water: return "Water";
        case DungeonTileKind::Trap: return "Trap";
    }
    return "?";
}

Vec3 DungeonTileKindColor(DungeonTileKind kind) {
    switch (kind) {
        case DungeonTileKind::Empty: return {0.08f, 0.09f, 0.11f};
        case DungeonTileKind::Floor: return {0.48f, 0.45f, 0.40f};
        case DungeonTileKind::Wall: return {0.20f, 0.22f, 0.26f};
        case DungeonTileKind::Door: return {0.58f, 0.32f, 0.12f};
        case DungeonTileKind::Water: return {0.12f, 0.38f, 0.68f};
        case DungeonTileKind::Trap: return {0.72f, 0.18f, 0.16f};
    }
    return {1.0f, 0.0f, 1.0f};
}

const char *PoiKindName(PoiKind kind) {
    switch (kind) {
        case PoiKind::Dungeon: return "Dungeon";
        case PoiKind::Ruin: return "Ruin";
        case PoiKind::Landmark: return "Landmark";
        case PoiKind::Temple: return "Temple";
        case PoiKind::Camp: return "Camp";
    }
    return "?";
}

PoiVisual GetPoiVisual(PoiKind kind) {
    switch (kind) {
        case PoiKind::Dungeon: return {3, static_cast<float>(kPi) / 2.0f, {0.80f, 0.15f, 0.15f}};
        case PoiKind::Ruin: return {4, static_cast<float>(kPi) / 4.0f, {0.55f, 0.55f, 0.55f}};
        case PoiKind::Landmark: return {3, -static_cast<float>(kPi) / 2.0f, {0.25f, 0.75f, 0.35f}};
        case PoiKind::Temple: return {8, 0.0f, {0.60f, 0.30f, 0.80f}};
        case PoiKind::Camp: return {6, 0.0f, {0.90f, 0.55f, 0.15f}};
    }
    return {4, 0.0f, {1.0f, 1.0f, 1.0f}};
}

Vec3 RouteColor(RouteKind kind) {
    return kind == RouteKind::River ? Vec3{0.20f, 0.55f, 0.85f} : Vec3{0.75f, 0.60f, 0.30f};
}

float RouteThickness(RouteKind kind) {
    return kind == RouteKind::River ? kTileSize * 0.35f : kTileSize * 0.18f;
}

const char *RouteKindName(RouteKind kind) {
    return kind == RouteKind::River ? "River" : "Trade Route";
}

const char *PaintModeName(PaintMode mode) {
    switch (mode) {
        case PaintMode::Terrain: return "Terrain";
        case PaintMode::Region: return "Region";
        case PaintMode::Elevation: return "Elevation";
        case PaintMode::Fog: return "Fog";
    }
    return "?";
}

const char *ElevationEditModeName(ElevationEditMode mode) {
    switch (mode) {
        case ElevationEditMode::Set: return "Set";
        case ElevationEditMode::Raise: return "Raise";
        case ElevationEditMode::Lower: return "Lower";
        case ElevationEditMode::Flatten: return "Flatten";
        case ElevationEditMode::Smooth: return "Smooth";
    }
    return "?";
}

Vec3 ElevationBandColor(int elevation) {
    static constexpr Vec3 colors[] = {
        {0.04f, 0.12f, 0.34f}, {0.05f, 0.24f, 0.55f}, {0.08f, 0.42f, 0.70f},
        {0.12f, 0.60f, 0.67f}, {0.22f, 0.60f, 0.28f}, {0.48f, 0.70f, 0.24f},
        {0.72f, 0.68f, 0.22f}, {0.72f, 0.48f, 0.18f}, {0.54f, 0.31f, 0.16f},
        {0.38f, 0.27f, 0.23f}, {0.48f, 0.48f, 0.48f}, {0.68f, 0.68f, 0.68f},
        {0.92f, 0.93f, 0.95f},
    };
    return colors[std::clamp(elevation, kMinElevation, kMaxElevation) - kMinElevation];
}

const char *ToolName(ToolMode mode) {
    switch (mode) {
        case ToolMode::Brush: return "Brush";
        case ToolMode::FloodFill: return "Flood Fill";
        case ToolMode::Line: return "Line";
        case ToolMode::Curve: return "Curve";
        case ToolMode::Polygon: return "Polygon";
        case ToolMode::Circle: return "Circle";
        case ToolMode::Scatter: return "Scatter";
        case ToolMode::River: return "River";
        case ToolMode::TradeRoute: return "Trade Route";
        case ToolMode::Selection: return "Selection";
        case ToolMode::Measure: return "Measure";
    }
    return "?";
}
