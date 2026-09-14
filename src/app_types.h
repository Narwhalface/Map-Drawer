#pragma once

#include "app_config.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct Vec3 {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

struct Region {
    int id = 0;
    Vec3 color{};
    std::string name;
    std::string ruler;
};

struct City {
    int32_t col = 0;
    int32_t row = 0;
    std::string name;
    std::string ruler;
};

enum class PoiKind { Dungeon, Ruin, Landmark, Temple, Camp };
inline constexpr int kPoiKindCount = 5;

struct PointOfInterest {
    int32_t col = 0;
    int32_t row = 0;
    PoiKind kind = PoiKind::Landmark;
    std::string name;
    std::string description;
};

struct PoiVisual {
    int sides = 4;
    float rotation = 0.0f;
    Vec3 color{};
};

enum class RouteKind { River, TradeRoute };

struct Route {
    RouteKind kind = RouteKind::River;
    std::string name;
    std::vector<std::pair<double, double>> points;
};

enum class PaintMode { Terrain, Region, Elevation, Fog };
enum class ElevationEditMode { Set, Raise, Lower, Flatten, Smooth };
enum class ToolMode { Brush, FloodFill, Line, Curve, Polygon, Circle, Scatter, River, TradeRoute, Selection, Measure };
enum class ModalType { None, Region, City, Poi, Route, Info, Confirm, ProjectName, Search };
enum class PlacementMode { None, City, Poi };
enum class ConfirmAction { None, ClearLayer, LoadProject, OverwriteProject, RecoverAutosave, GenerateRelief };

enum class UiAction {
    None, SetMode, SetTool, SetTerrain, SetElevationValue, SetElevationTool,
    ElevationDown, ElevationUp, BrushDown, BrushUp, ToggleShape,
    NewRegion, CycleRegion, NewCity, NewPoi, DeleteMarker, DeleteRoute, WorldInfo, Help, Find, RollEncounter,
    Undo, Redo, Save, Load, Clear, Export, ToggleGrid, ToggleGeometry, ToggleRegions,
    ToggleLabels, TogglePlayerView, ToggleElevationView, ToggleContours, ToggleHillshade, GenerateRelief,
    FogHideAll, FogRevealAll, ResetCamera, FitMap, ProjectName,
    SelectionCopy, SelectionCut, SelectionPaste, SelectionDelete,
    SelectionPaint, SelectionRegion, SelectionElevationDown, SelectionElevationUp, SelectionClear,
    ModalPrevious, ModalNext, ModalAccept, ModalCancel, ModalPoiKind
};

struct TileChange {
    uint64_t key = 0;
    int16_t oldValue = 0;
    int16_t newValue = 0;
};

struct SearchResult {
    std::string label;
    double worldX = 0.0;
    double worldY = 0.0;
};

struct StrokeRecord {
    PaintMode layer = PaintMode::Terrain;
    std::vector<TileChange> changes;
};

struct UiHit {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    UiAction action = UiAction::None;
    int value = 0;
};

struct ClipboardTile {
    int32_t dc = 0;
    int32_t dr = 0;
    int16_t value = 0;
};

inline constexpr int kRegionPaletteSize = 10;
extern const Vec3 kRegionPalette[kRegionPaletteSize];
extern const Vec3 kTerrainColors[kTerrainCount];
extern const char *const kTerrainNames[kTerrainCount];

const char *PoiKindName(PoiKind kind);
PoiVisual GetPoiVisual(PoiKind kind);
Vec3 RouteColor(RouteKind kind);
float RouteThickness(RouteKind kind);
const char *RouteKindName(RouteKind kind);
const char *PaintModeName(PaintMode mode);
const char *ElevationEditModeName(ElevationEditMode mode);
Vec3 ElevationBandColor(int elevation);
const char *ToolName(ToolMode mode);
