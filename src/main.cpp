// DND Overworld Map Drawer
// An infinite, pannable/zoomable tile-based map painter: pick a terrain brush with
// number keys, paint tiles with the mouse, pan with WASD/arrows, zoom with the
// scroll wheel, and save/load the world as one versioned project file.
#include "app_config.h"
#include "app_types.h"
#include "logger.h"
#include "grid_geometry.h"
#include "mesh_buffer.h"
#include "project_document.h"
#include "shader_program.h"
#include "tiny_font.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <queue>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// World-space size of the tiny in-map font.
constexpr float kLabelPixelSize = kTileSize * 0.11f; // world size of one font "pixel"
constexpr float kLabelGlyphAdvance = kLabelPixelSize * 4.0f; // 3 cols + 1 gap

const char *kVertexShaderSource = R"glsl(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec4 aColor;

uniform vec2 uResolution;

out vec4 vColor;

void main() {
    vec2 ndc = vec2((aPos.x / uResolution.x) * 2.0 - 1.0,
                     1.0 - (aPos.y / uResolution.y) * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vColor = aColor;
}
)glsl";

const char *kFragmentShaderSource = R"glsl(
#version 330 core
in vec4 vColor;
out vec4 FragColor;

void main() {
    FragColor = vColor;
}
)glsl";

void UpdateWindowTitle();
void SelectTool(ToolMode mode);
void SaveProjectNow();
void RequestProjectSave();
bool LoadProjectFromPath(const std::string &path, bool allowLegacyFallback);
void RequestProjectLoad();
void RequestClearActiveLayer();
void ClearActiveLayerNow();
void SaveProjectConfig();
void LoadProjectConfig();
void CheckForRecoveryAutosave();
void GenerateTerrainRelief();
void OpenKeybindHelp();

// Persisted world data lives together; aliases keep the editing code concise while
// making the ownership boundary explicit for save/load operations.
ProjectDocument gProjectDocument;
auto &gMapData = gProjectDocument.terrain;
auto &gRegionData = gProjectDocument.regionsByTile;
auto &gElevationData = gProjectDocument.elevation;
auto &gFogData = gProjectDocument.fog;
auto &gRegions = gProjectDocument.regions;
auto &gCities = gProjectDocument.cities;
auto &gPois = gProjectDocument.pointsOfInterest;
auto &gEncounters = gProjectDocument.encounters;
auto &gRoutes = gProjectDocument.routes;
bool &gHexGrid = gProjectDocument.hexGrid;
int &gMetresPerElevationLevel = gProjectDocument.metresPerElevationLevel;
int &gSeaLevel = gProjectDocument.seaLevel;
int &gContourInterval = gProjectDocument.contourInterval;
bool &gElevationView = gProjectDocument.elevationView;
bool &gShowElevationContours = gProjectDocument.showContours;
bool &gShowHillshade = gProjectDocument.showHillshade;

// ---- Transient editor state -----------------------------------------------
int gNextRegionId = 1;
int gActiveRegionId = 0;
std::vector<std::pair<double, double>> gRoutePoints; // pending River/TradeRoute path being drawn
PaintMode gPaintMode = PaintMode::Terrain;
bool gShowRegions = true;
bool gShowLabels = true;
int gBrush = 1;
int gElevationBrush = 1;
ElevationEditMode gElevationEditMode = ElevationEditMode::Set;
bool gFlattenHeightCaptured = false;
int gFlattenHeight = 0;
int gBrushRadius = 0; // 0 = single tile, N = (2N+1)x(2N+1) square
bool gRoundBrush = false; // false = square stamp, true = circular stamp (more organic edges)
bool gShowGrid = true;
bool gPlayerView = false;
ToolMode gToolMode = ToolMode::Brush;
bool gPaintingLeft = false;
bool gPaintingRight = false;
bool gMiddlePanning = false;
double gPanLastX = 0.0, gPanLastY = 0.0;
double gCameraX = -kUiWidth; // leave the world origin visible beside the GUI sidebar
double gCameraY = 0.0;
double gZoom = 1.0;
int gWindowWidth = 1280;
int gWindowHeight = 960;
GLFWwindow *gWindow = nullptr;
std::vector<UiHit> gUiHits;
ModalType gModalType = ModalType::None;
PlacementMode gPlacementMode = PlacementMode::None;
std::vector<std::string> gModalFields;
std::string gInfoTitle;
std::vector<std::string> gInfoLines;
ConfirmAction gConfirmAction = ConfirmAction::None;
std::string gProjectFile = kDefaultProjectFile;
bool gProjectDirty = false;
int gModalField = 0;
int32_t gModalCol = 0, gModalRow = 0;
PoiKind gModalPoiKind = PoiKind::Landmark;
RouteKind gModalRouteKind = RouteKind::River;
int gEditingEncounterIndex = -1;
double gLastCanvasWorldX = 0.0, gLastCanvasWorldY = 0.0;
bool gUseUiTarget = false;
uint64_t gSceneRevision = 1;

// Line tool: press-drag-release paints a thick line between two points.
bool gLineDragging = false;
bool gLineErase = false;
double gLineStartWX = 0.0, gLineStartWY = 0.0;

// Curve tool: three clicks define a quadratic Bezier (start, control, end).
int gCurveStage = 0; // 0 = none, 1 = have start, 2 = have start+control
double gCurveP0X = 0.0, gCurveP0Y = 0.0, gCurveP1X = 0.0, gCurveP1Y = 0.0;

// Polygon tool: click to add vertices; right-click fills, Backspace cancels.
std::vector<std::pair<double, double>> gPolygonPoints;

// Circle tool: press-drag-release fills a disc.
bool gCircleDragging = false;
bool gCircleErase = false;
double gCircleCenterWX = 0.0, gCircleCenterWY = 0.0;

// Undo/redo: each stroke records the pre- and post-edit value of every tile it touched,
// tagged with the tile layer it applies to.
std::vector<StrokeRecord> gUndoStack;
std::vector<StrokeRecord> gRedoStack;
bool gStrokeActive = false;
bool gStrokeChanged = false;
PaintMode gStrokeLayer = PaintMode::Terrain;
std::unordered_map<uint64_t, int16_t> gStrokeOriginal; // key -> value before this stroke touched it

// Shift+drag rectangle fill state.
bool gRectDragging = false;
bool gRectErase = false;
int32_t gRectStartCol = 0, gRectStartRow = 0;
int32_t gRectEndCol = 0, gRectEndRow = 0;
bool gSelectionDragging = false;
bool gSelectionActive = false;
int32_t gSelectionStartCol = 0, gSelectionStartRow = 0;
int32_t gSelectionEndCol = 0, gSelectionEndRow = 0;
PaintMode gClipboardLayer = PaintMode::Terrain;
std::vector<ClipboardTile> gTileClipboard;
int32_t gClipboardWidth = 0, gClipboardHeight = 0;
int gMeasureStage = 0; // 0 = none, 1 = following cursor, 2 = locked result
int32_t gMeasureStartCol = 0, gMeasureStartRow = 0;
int32_t gMeasureEndCol = 0, gMeasureEndRow = 0;
std::string gLastSearchQuery;
std::string gLastFoundLabel;
std::vector<SearchResult> gSearchMatches;
size_t gSearchMatchIndex = 0;

void GlfwErrorCallback(int error, const char *description) {
    LOG_ERROR("GLFW error %d: %s", error, description);
}

void FramebufferSizeCallback(GLFWwindow * /*window*/, int width, int height) {
    glViewport(0, 0, width, height);
}

void WindowSizeCallback(GLFWwindow * /*window*/, int width, int height) {
    gWindowWidth = width;
    gWindowHeight = height;
}

uint64_t TileKey(int32_t col, int32_t row) {
    return grid_geometry::Pack(col, row);
}

std::pair<double, double> TileCenterWorld(int32_t col, int32_t row) {
    return grid_geometry::TileCenter(col, row, gHexGrid);
}

std::pair<int32_t, int32_t> TileCoords(uint64_t key) {
    return grid_geometry::Unpack(key);
}

std::pair<int32_t, int32_t> WorldToTile(double worldX, double worldY) {
    return grid_geometry::WorldToTile(worldX, worldY, gHexGrid);
}

double MeasuredTileDistance() {
    return grid_geometry::TileDistance(gMeasureStartCol, gMeasureStartRow,
                                       gMeasureEndCol, gMeasureEndRow, gHexGrid);
}

void TileBoundsForWorldRect(double minX, double minY, double maxX, double maxY,
                            int32_t &minCol, int32_t &minRow, int32_t &maxCol, int32_t &maxRow) {
    grid_geometry::BoundsForWorldRect(minX, minY, maxX, maxY, gHexGrid,
                                      minCol, minRow, maxCol, maxRow);
}

void VisibleTileBounds(int32_t &minCol, int32_t &minRow, int32_t &maxCol, int32_t &maxRow) {
    TileBoundsForWorldRect(gCameraX, gCameraY, gCameraX + gWindowWidth / gZoom,
                           gCameraY + gWindowHeight / gZoom, minCol, minRow, maxCol, maxRow);
}

// Converts the current mouse position to a (col, row) tile coordinate.
std::pair<int32_t, int32_t> CursorTile() {
    if (gUseUiTarget) return WorldToTile(gLastCanvasWorldX, gLastCanvasWorldY);
    double mouseX = 0.0, mouseY = 0.0;
    glfwGetCursorPos(gWindow, &mouseX, &mouseY);
    return WorldToTile(gCameraX + mouseX / gZoom, gCameraY + mouseY / gZoom);
}

// Converts the current mouse position to world-space pixel coordinates.
std::pair<double, double> CursorWorld() {
    if (gUseUiTarget) return {gLastCanvasWorldX, gLastCanvasWorldY};
    double mouseX = 0.0, mouseY = 0.0;
    glfwGetCursorPos(gWindow, &mouseX, &mouseY);
    return {gCameraX + mouseX / gZoom, gCameraY + mouseY / gZoom};
}

void FitMapToWindow() {
    bool haveBounds = false;
    double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
    auto includePoint = [&](double x, double y, double radiusX = 0.0, double radiusY = 0.0) {
        if (!haveBounds) {
            minX = x - radiusX; maxX = x + radiusX;
            minY = y - radiusY; maxY = y + radiusY;
            haveBounds = true;
        } else {
            minX = std::min(minX, x - radiusX); maxX = std::max(maxX, x + radiusX);
            minY = std::min(minY, y - radiusY); maxY = std::max(maxY, y + radiusY);
        }
    };
    auto includeTileLayer = [&](const auto &layer) {
        for (const auto &entry : layer) {
            auto [col, row] = TileCoords(entry.first);
            auto [x, y] = TileCenterWorld(col, row);
            includePoint(x, y, kTileSize * 0.5, gHexGrid ? kHexRowHeight * 0.5 : kTileSize * 0.5);
        }
    };
    includeTileLayer(gMapData);
    includeTileLayer(gRegionData);
    includeTileLayer(gElevationData);
    includeTileLayer(gFogData);
    for (const auto &city : gCities) {
        auto [x, y] = TileCenterWorld(city.col, city.row);
        includePoint(x, y, kTileSize * 0.5, kTileSize * 0.5);
    }
    for (const auto &poi : gPois) {
        auto [x, y] = TileCenterWorld(poi.col, poi.row);
        includePoint(x, y, kTileSize * 0.5, kTileSize * 0.5);
    }
    for (const auto &encounter : gEncounters) {
        auto [x, y] = TileCenterWorld(encounter.col, encounter.row);
        includePoint(x, y, kTileSize * 0.5, kTileSize * 0.5);
    }
    for (const auto &route : gRoutes)
        for (const auto &point : route.points) includePoint(point.first, point.second, kTileSize, kTileSize);

    if (!haveBounds) {
        gCameraX = -kUiWidth;
        gCameraY = 0.0;
        gZoom = 1.0;
        LOG_INFO("Fit map: world is empty; reset the camera instead");
        return;
    }
    double canvasWidth = std::max(100.0, static_cast<double>(gWindowWidth) - kUiWidth - 40.0);
    double canvasHeight = std::max(100.0, static_cast<double>(gWindowHeight) - 40.0);
    double worldWidth = std::max(static_cast<double>(kTileSize), maxX - minX);
    double worldHeight = std::max(static_cast<double>(kTileSize), maxY - minY);
    gZoom = std::clamp(std::min(canvasWidth / worldWidth, canvasHeight / worldHeight), kMinZoom, kMaxZoom);
    double centerX = (minX + maxX) * 0.5;
    double centerY = (minY + maxY) * 0.5;
    double screenCenterX = kUiWidth + (gWindowWidth - kUiWidth) * 0.5;
    gCameraX = centerX - screenCenterX / gZoom;
    gCameraY = centerY - (gWindowHeight * 0.5) / gZoom;
    LOG_INFO("Fit map: bounds %.0f x %.0f world pixels, zoom %.2f%%", worldWidth, worldHeight,
             gZoom * 100.0);
}

std::array<std::pair<int32_t, int32_t>, 8> NeighborTiles(int32_t col, int32_t row, int &count) {
    return grid_geometry::Neighbors(col, row, gHexGrid, count);
}

int GetLayerValue(PaintMode layer, uint64_t key) {
    if (layer == PaintMode::Terrain) {
        auto it = gMapData.find(key);
        return it == gMapData.end() ? 0 : it->second;
    }
    if (layer == PaintMode::Region) {
        auto it = gRegionData.find(key);
        return it == gRegionData.end() ? 0 : it->second;
    }
    if (layer == PaintMode::Elevation) {
        auto it = gElevationData.find(key);
        return it == gElevationData.end() ? 0 : it->second;
    }
    auto it = gFogData.find(key);
    return it == gFogData.end() ? 0 : it->second;
}

float ElevationHillshade(int32_t col, int32_t row) {
    if (!gShowHillshade) return 1.0f;
    if (!gHexGrid) {
        int northwest = GetLayerValue(PaintMode::Elevation, TileKey(col - 1, row - 1));
        int southeast = GetLayerValue(PaintMode::Elevation, TileKey(col + 1, row + 1));
        return std::clamp(1.0f + static_cast<float>(northwest - southeast) * 0.075f, 0.62f, 1.35f);
    }
    int count = 0;
    auto neighbors = NeighborTiles(col, row, count);
    int northwest = GetLayerValue(PaintMode::Elevation, TileKey(neighbors[count - 1].first,
                                                                 neighbors[count - 1].second));
    int southeast = GetLayerValue(PaintMode::Elevation, TileKey(neighbors[2].first, neighbors[2].second));
    float slope = static_cast<float>(northwest - southeast) * 0.075f;
    return std::clamp(1.0f + slope, 0.62f, 1.35f);
}

int GetCurrentLayerValue(int32_t col, int32_t row) {
    return GetLayerValue(gPaintMode, TileKey(col, row));
}

void MarkProjectDirty() {
    gProjectDirty = true;
    gSearchMatches.clear();
    gLastSearchQuery.clear();
    gLastFoundLabel.clear();
}

void SetLayerValue(PaintMode layer, uint64_t key, int value) {
    if (layer == PaintMode::Terrain) {
        if (value == 0) gMapData.erase(key);
        else gMapData[key] = static_cast<uint8_t>(value);
    } else if (layer == PaintMode::Region) {
        if (value == 0) gRegionData.erase(key);
        else gRegionData[key] = static_cast<uint8_t>(value);
    } else if (layer == PaintMode::Elevation) {
        if (value == 0) gElevationData.erase(key);
        else gElevationData[key] = static_cast<int8_t>(value);
    } else {
        if (value == 0) gFogData.erase(key);
        else gFogData[key] = static_cast<uint8_t>(value);
    }
}

// Returns the value the active tool should paint with, or -1 if painting isn't possible
// right now (e.g. region mode with no active region selected).
int ActivePaintValue() {
    if (gPaintMode == PaintMode::Terrain) return gBrush;
    if (gPaintMode == PaintMode::Elevation)
        return gElevationEditMode == ElevationEditMode::Set ? gElevationBrush : 1;
    if (gPaintMode == PaintMode::Fog) return 1;
    if (gActiveRegionId == 0) {
        LOG_WARN("No active region - press N to found one first");
        return kNoPaintValue;
    }
    return gActiveRegionId;
}

size_t EstimatedTileCount(int32_t minCol, int32_t minRow, int32_t maxCol, int32_t maxRow) {
    uint64_t width = static_cast<uint64_t>(static_cast<int64_t>(maxCol) - minCol + 1);
    uint64_t height = static_cast<uint64_t>(static_cast<int64_t>(maxRow) - minRow + 1);
    uint64_t count = width > std::numeric_limits<uint64_t>::max() / height
                         ? std::numeric_limits<uint64_t>::max()
                         : width * height;
    return static_cast<size_t>(std::min<uint64_t>(count, kBulkReserveLimit));
}

void BeginStroke(size_t expectedTiles = 0, int paintValue = kNoPaintValue) {
    gStrokeActive = true;
    gStrokeChanged = false;
    gStrokeLayer = gPaintMode;
    gStrokeOriginal.clear();
    gFlattenHeightCaptured = false;
    expectedTiles = std::min(expectedTiles, kBulkReserveLimit);
    if (expectedTiles > 0) {
        gStrokeOriginal.reserve(expectedTiles);
        if (paintValue != 0) {
            if (gStrokeLayer == PaintMode::Terrain) gMapData.reserve(gMapData.size() + expectedTiles);
            else if (gStrokeLayer == PaintMode::Region) gRegionData.reserve(gRegionData.size() + expectedTiles);
            else if (gStrokeLayer == PaintMode::Elevation) gElevationData.reserve(gElevationData.size() + expectedTiles);
            else gFogData.reserve(gFogData.size() + expectedTiles);
        }
    }
}

// Sets (or, for value 0, erases) a tile in the active layer, recording its pre-stroke value.
void SetTileRecorded(int32_t col, int32_t row, int value) {
    uint64_t key = TileKey(col, row);
    PaintMode layer = gStrokeActive ? gStrokeLayer : gPaintMode;
    if (layer == PaintMode::Elevation && value != 0 && gElevationEditMode != ElevationEditMode::Set) {
        if (gStrokeActive && gStrokeOriginal.count(key) != 0) return;
        int oldValue = GetLayerValue(PaintMode::Elevation, key);
        if (gElevationEditMode == ElevationEditMode::Raise) {
            value = std::min(kMaxElevation, oldValue + 1);
        } else if (gElevationEditMode == ElevationEditMode::Lower) {
            value = std::max(kMinElevation, oldValue - 1);
        } else if (gElevationEditMode == ElevationEditMode::Flatten) {
            if (!gFlattenHeightCaptured) {
                gFlattenHeight = oldValue;
                gFlattenHeightCaptured = true;
            }
            value = gFlattenHeight;
        } else if (gElevationEditMode == ElevationEditMode::Smooth) {
            int neighborCount = 0;
            auto neighbors = NeighborTiles(col, row, neighborCount);
            int sum = oldValue;
            for (int i = 0; i < neighborCount; ++i)
                sum += GetLayerValue(PaintMode::Elevation,
                                     TileKey(neighbors[i].first, neighbors[i].second));
            value = std::clamp(static_cast<int>(std::lround(static_cast<double>(sum) /
                                                            (neighborCount + 1))),
                               kMinElevation, kMaxElevation);
        }
    }
    auto update = [&](auto &tileLayer) {
        using StoredValue = typename std::decay_t<decltype(tileLayer)>::mapped_type;
        auto it = tileLayer.find(key);
        int oldValue = it == tileLayer.end() ? 0 : static_cast<int>(it->second);
        if (oldValue == value) return false;
        if (gStrokeActive) gStrokeOriginal.emplace(key, static_cast<int16_t>(oldValue));
        if (value == 0) {
            if (it != tileLayer.end()) tileLayer.erase(it);
        } else if (it == tileLayer.end()) {
            tileLayer.emplace(key, static_cast<StoredValue>(value));
        } else {
            it->second = static_cast<StoredValue>(value);
        }
        return true;
    };
    bool changed = layer == PaintMode::Terrain ? update(gMapData)
                   : layer == PaintMode::Region ? update(gRegionData)
                   : layer == PaintMode::Elevation ? update(gElevationData)
                                                   : update(gFogData);
    if (changed) {
        MarkProjectDirty();
        if (gStrokeActive) gStrokeChanged = true;
        else ++gSceneRevision;
    }
}

void EndStroke() {
    if (!gStrokeActive) return;
    gStrokeActive = false;
    if (!gStrokeChanged || gStrokeOriginal.empty()) return;

    StrokeRecord record;
    record.layer = gStrokeLayer;
    record.changes.reserve(gStrokeOriginal.size());
    for (const auto &entry : gStrokeOriginal) {
        int newValue = GetLayerValue(gStrokeLayer, entry.first);
        record.changes.push_back({entry.first, entry.second, static_cast<int16_t>(newValue)});
    }
    gStrokeOriginal.clear();
    ++gSceneRevision;
    MarkProjectDirty();

    gUndoStack.push_back(std::move(record));
    if (gUndoStack.size() > kMaxUndoStrokes) gUndoStack.erase(gUndoStack.begin());
    gRedoStack.clear();
}

void Undo() {
    if (gUndoStack.empty()) return;
    StrokeRecord record = std::move(gUndoStack.back());
    gUndoStack.pop_back();
    for (const auto &change : record.changes) {
        SetLayerValue(record.layer, change.key, change.oldValue);
    }
    ++gSceneRevision;
    MarkProjectDirty();
    LOG_INFO("Undo (%zu tiles, %s layer)", record.changes.size(),
              PaintModeName(record.layer));
    gRedoStack.push_back(std::move(record));
}

void Redo() {
    if (gRedoStack.empty()) return;
    StrokeRecord record = std::move(gRedoStack.back());
    gRedoStack.pop_back();
    for (const auto &change : record.changes) {
        SetLayerValue(record.layer, change.key, change.newValue);
    }
    ++gSceneRevision;
    MarkProjectDirty();
    LOG_INFO("Redo (%zu tiles, %s layer)", record.changes.size(),
              PaintModeName(record.layer));
    gUndoStack.push_back(std::move(record));
}

// Flood-fills the contiguous region of matching value (in the active layer) starting at (col, row).
void FloodFill(int32_t col, int32_t row, int value) {
    if (value == kNoPaintValue) return;
    int target = GetCurrentLayerValue(col, row);
    if (target == value) return;

    BeginStroke(kFloodFillLimit, value);
    std::queue<std::pair<int32_t, int32_t>> frontier;
    std::unordered_set<uint64_t> visited;
    visited.reserve(kFloodFillLimit);
    frontier.push({col, row});
    visited.insert(TileKey(col, row));

    int processed = 0;
    while (!frontier.empty() && processed < kFloodFillLimit) {
        auto [c, r] = frontier.front();
        frontier.pop();
        SetTileRecorded(c, r, value);
        ++processed;

        constexpr int squareDc[4] = {1, -1, 0, 0};
        constexpr int squareDr[4] = {0, 0, 1, -1};
        constexpr int hexDc[6] = {0, 0, 1, 1, -1, -1};
        constexpr int evenHexDr[6] = {-1, 1, -1, 0, -1, 0};
        constexpr int oddHexDr[6] = {-1, 1, 0, 1, 0, 1};
        int neighborCount = gHexGrid ? 6 : 4;
        for (int i = 0; i < neighborCount; ++i) {
            int32_t nc = c + (gHexGrid ? hexDc[i] : squareDc[i]);
            int32_t nr = r + (gHexGrid ? ((c % 2 != 0) ? oddHexDr[i] : evenHexDr[i]) : squareDr[i]);
            uint64_t key = TileKey(nc, nr);
            if (visited.count(key)) continue;
            if (GetCurrentLayerValue(nc, nr) == target) {
                visited.insert(key);
                frontier.push({nc, nr});
            }
        }
    }
    EndStroke();

    if (processed >= kFloodFillLimit)
        LOG_WARN("Flood fill stopped after %d tiles (safety limit reached)", kFloodFillLimit);
    else
        LOG_INFO("Flood fill: %d tiles changed", processed);
}

void CommitRectFill() {
    int32_t minCol = std::min(gRectStartCol, gRectEndCol);
    int32_t maxCol = std::max(gRectStartCol, gRectEndCol);
    int32_t minRow = std::min(gRectStartRow, gRectEndRow);
    int32_t maxRow = std::max(gRectStartRow, gRectEndRow);
    int value = gRectErase ? 0 : ActivePaintValue();
    if (value == kNoPaintValue) return;

    BeginStroke(EstimatedTileCount(minCol, minRow, maxCol, maxRow), value);
    for (int32_t row = minRow; row <= maxRow; ++row) {
        for (int32_t col = minCol; col <= maxCol; ++col) {
            SetTileRecorded(col, row, value);
        }
    }
    EndStroke();
    uint64_t tileCount = static_cast<uint64_t>(static_cast<int64_t>(maxCol) - minCol + 1) *
                         static_cast<uint64_t>(static_cast<int64_t>(maxRow) - minRow + 1);
    LOG_INFO("Rectangle %s: %llu tiles", gRectErase ? "erase" : "fill",
             static_cast<unsigned long long>(tileCount));
}

// Whether tile offset (dc, dr) from the brush center is covered by the current brush shape.
bool BrushCovers(int32_t centerCol, int32_t dc, int32_t dr) {
    if (!gRoundBrush || gBrushRadius == 0) return true;
    if (gHexGrid) {
        auto cubeZ = [](int32_t col, int32_t row) {
            int32_t parity = (col % 2 != 0) ? 1 : 0;
            return row - (col - parity) / 2;
        };
        int32_t centerZ = cubeZ(centerCol, 0);
        int32_t targetCol = centerCol + dc;
        int32_t targetZ = cubeZ(targetCol, dr);
        int32_t dx = dc;
        int32_t dz = targetZ - centerZ;
        int32_t dy = -dx - dz;
        return std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) <= gBrushRadius;
    }
    return dc * dc + dr * dr <= gBrushRadius * gBrushRadius + gBrushRadius;
}

// Stamps the current brush (square or round, per gRoundBrush) centered on a tile.
void StampBrush(int32_t centerCol, int32_t centerRow, int value) {
    for (int32_t dr = -gBrushRadius; dr <= gBrushRadius; ++dr) {
        for (int32_t dc = -gBrushRadius; dc <= gBrushRadius; ++dc) {
            if (BrushCovers(centerCol, dc, dr)) SetTileRecorded(centerCol + dc, centerRow + dr, value);
        }
    }
}

void StampBrushAtWorld(double worldX, double worldY, int value) {
    auto [col, row] = WorldToTile(worldX, worldY);
    StampBrush(col, row, value);
}

// Stamps the brush along a straight line between two world-space points (rivers, roads, walls).
void RasterizeLine(double x0, double y0, double x1, double y1, int value) {
    if (value == kNoPaintValue) return;
    double length = std::hypot(x1 - x0, y1 - y0);
    int steps = std::max(1, static_cast<int>(length / (kTileSize * 0.5)));

    size_t brushTiles = static_cast<size_t>(gBrushRadius * 2 + 1) * (gBrushRadius * 2 + 1);
    BeginStroke(std::min(kBulkReserveLimit, static_cast<size_t>(steps + 1) * brushTiles), value);
    for (int i = 0; i <= steps; ++i) {
        double t = static_cast<double>(i) / steps;
        StampBrushAtWorld(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, value);
    }
    EndStroke();
    LOG_INFO("Line drawn");
}

// Stamps the brush along a quadratic Bezier curve (start, control, end) for organic rivers/coasts.
void RasterizeCurve(double x0, double y0, double x1, double y1, double x2, double y2, int value) {
    if (value == kNoPaintValue) return;
    double approxLength = std::hypot(x1 - x0, y1 - y0) + std::hypot(x2 - x1, y2 - y1);
    int steps = std::max(8, static_cast<int>(approxLength / (kTileSize * 0.5)));

    size_t brushTiles = static_cast<size_t>(gBrushRadius * 2 + 1) * (gBrushRadius * 2 + 1);
    BeginStroke(std::min(kBulkReserveLimit, static_cast<size_t>(steps + 1) * brushTiles), value);
    for (int i = 0; i <= steps; ++i) {
        double t = static_cast<double>(i) / steps;
        double mt = 1.0 - t;
        double x = mt * mt * x0 + 2.0 * mt * t * x1 + t * t * x2;
        double y = mt * mt * y0 + 2.0 * mt * t * y1 + t * t * y2;
        StampBrushAtWorld(x, y, value);
    }
    EndStroke();
    LOG_INFO("Curve drawn");
}

// Fills a filled disc of the given world-space radius (lakes, islands, craters).
void FillCircle(double centerWX, double centerWY, double radiusWorld, int value) {
    if (value == kNoPaintValue || radiusWorld <= 0.0) return;
    int32_t minCol = 0, minRow = 0, maxCol = 0, maxRow = 0;
    TileBoundsForWorldRect(centerWX - radiusWorld, centerWY - radiusWorld,
                           centerWX + radiusWorld, centerWY + radiusWorld,
                           minCol, minRow, maxCol, maxRow);

    BeginStroke(EstimatedTileCount(minCol, minRow, maxCol, maxRow), value);
    size_t changed = 0;
    for (int32_t row = minRow; row <= maxRow; ++row) {
        for (int32_t col = minCol; col <= maxCol; ++col) {
            auto [tx, ty] = TileCenterWorld(col, row);
            double dx = tx - centerWX, dy = ty - centerWY;
            if (dx * dx + dy * dy <= radiusWorld * radiusWorld) {
                SetTileRecorded(col, row, value);
                ++changed;
            }
        }
    }
    EndStroke();
    LOG_INFO("Circle fill: %zu tiles", changed);
}

// Fills the interior of a world-space polygon using a scanline point-in-polygon test.
void FillPolygon(const std::vector<std::pair<double, double>> &points, int value) {
    if (points.size() < 3 || value == kNoPaintValue) return;

    double minX = points[0].first, maxX = minX;
    double minY = points[0].second, maxY = minY;
    for (const auto &p : points) {
        minX = std::min(minX, p.first);
        maxX = std::max(maxX, p.first);
        minY = std::min(minY, p.second);
        maxY = std::max(maxY, p.second);
    }
    int32_t minCol = 0, minRow = 0, maxCol = 0, maxRow = 0;
    TileBoundsForWorldRect(minX, minY, maxX, maxY, minCol, minRow, maxCol, maxRow);
    size_t n = points.size();

    BeginStroke(EstimatedTileCount(minCol, minRow, maxCol, maxRow), value);
    size_t changed = 0;
    for (int32_t row = minRow; row <= maxRow; ++row) {
        int parityPasses = gHexGrid ? 2 : 1;
        for (int parity = 0; parity < parityPasses; ++parity) {
            int32_t sampleCol = parity;
            double testY = TileCenterWorld(sampleCol, row).second;
            std::vector<double> intersections;
            intersections.reserve(n);
            for (size_t i = 0, j = n - 1; i < n; j = i++) {
                const auto &a = points[i];
                const auto &b = points[j];
                if ((a.second > testY) != (b.second > testY)) {
                    intersections.push_back((b.first - a.first) * (testY - a.second) /
                                                (b.second - a.second) + a.first);
                }
            }
            std::sort(intersections.begin(), intersections.end());
            size_t intersectionIndex = 0;
            for (int32_t col = minCol; col <= maxCol; ++col) {
                if (gHexGrid && ((col % 2 != 0) ? 1 : 0) != parity) continue;
                double testX = TileCenterWorld(col, row).first;
                while (intersectionIndex < intersections.size() &&
                       intersections[intersectionIndex] <= testX)
                    ++intersectionIndex;
                if (intersectionIndex % 2 != 0) {
                    SetTileRecorded(col, row, value);
                    ++changed;
                }
            }
        }
    }
    EndStroke();
    LOG_INFO("Polygon fill: %zu tiles", changed);
}

// Randomly stamps a fraction of tiles within brush range of a point (scattered forests, rubble).
void ScatterAt(double worldX, double worldY, int value) {
    if (value == kNoPaintValue) return;
    auto [centerCol, centerRow] = WorldToTile(worldX, worldY);
    for (int32_t dr = -gBrushRadius; dr <= gBrushRadius; ++dr) {
        for (int32_t dc = -gBrushRadius; dc <= gBrushRadius; ++dc) {
            if (!BrushCovers(centerCol, dc, dr)) continue;
            if (static_cast<float>(std::rand()) / RAND_MAX < kScatterDensity) {
                SetTileRecorded(centerCol + dc, centerRow + dr, value);
            }
        }
    }
}

void OpenModal(ModalType type, std::vector<std::string> fields) {
    gModalType = type;
    gModalFields = std::move(fields);
    gModalField = 0;
}

void OpenConfirmation(ConfirmAction action, const std::string &title,
                      std::vector<std::string> lines) {
    gConfirmAction = action;
    gInfoTitle = title;
    gInfoLines = std::move(lines);
    OpenModal(ModalType::Confirm, {});
}

void OpenKeybindHelp() {
    gInfoTitle = "KEYBOARD & MOUSE HELP";
    gInfoLines = {
        "NAVIGATION",
        "WASD / ARROWS     PAN THE CAMERA",
        "MIDDLE DRAG       PAN QUICKLY ACROSS THE MAP",
        "MOUSE WHEEL       ZOOM AROUND THE CURSOR",
        "HOME              FIT ALL MAP CONTENT",
        "R                 RESET CAMERA AND ZOOM",
        "PAINTING",
        "LEFT CLICK/DRAG   USE THE ACTIVE TOOL",
        "RIGHT CLICK/DRAG  ERASE OR FINISH A PATH",
        "SHIFT + DRAG      FILL OR ERASE A RECTANGLE",
        "[ / ]             DECREASE / INCREASE BRUSH SIZE",
        "H                 TOGGLE ROUND / BLOCK BRUSH",
        "T                 CYCLE TERRAIN / REGION / HEIGHT / FOG",
        "Q / E             LOWER / RAISE ELEVATION VALUE",
        "TOOLS",
        "F1 BRUSH   F2 FLOOD FILL   F3 LINE   F4 CURVE",
        "F5 POLYGON   F6 CIRCLE   F7 SCATTER",
        "F8 RIVER   F9 TRADE ROUTE   F10 SELECTION",
        "F11 PLAYER VIEW   F12 MEASURE",
        "WORLD AND PROJECT",
        "M CITY   K POI   N NEW REGION   TAB CYCLE REGION",
        "V REGIONS   L LABELS   G GRID   Y HEX / SQUARE",
        "CTRL+F FIND   CTRL+E PLACE ENCOUNTER   I WORLD INFO",
        "CLICK AN ENCOUNTER MARKER TO EDIT ITS DETAILS",
        "DELETE MARKER/SELECTION   X DELETE ROUTE",
        "CTRL+C/X/V COPY / CUT / PASTE SELECTION",
        "CTRL+Z/Y UNDO / REDO   C CLEAR ACTIVE LAYER",
        "CTRL+S/L SAVE / LOAD   P EXPORT   ESC CLOSE / QUIT",
        "PRESS ? OR USE THE HELP BUTTON TO OPEN THIS MENU",
    };
    OpenModal(ModalType::Info, {});
}

// Opens an in-window naming dialog for the pending River/TradeRoute path.
void CommitRoute(RouteKind kind) {
    if (gRoutePoints.size() < 2) {
        LOG_WARN("Need at least 2 points to create a %s", RouteKindName(kind));
        gRoutePoints.clear();
        return;
    }
    gModalRouteKind = kind;
    OpenModal(ModalType::Route, {""});
}

// Removes whichever route passes closest to the cursor, if within a tile or two.
void RemoveRouteAtCursor() {
    auto [wx, wy] = CursorWorld();
    double bestDist = kTileSize * 2.0; // ignore routes further than this
    size_t bestIndex = gRoutes.size();

    for (size_t i = 0; i < gRoutes.size(); ++i) {
        const auto &pts = gRoutes[i].points;
        for (size_t j = 0; j + 1 < pts.size(); ++j) {
            double x0 = pts[j].first, y0 = pts[j].second;
            double x1 = pts[j + 1].first, y1 = pts[j + 1].second;
            double dx = x1 - x0, dy = y1 - y0;
            double lenSq = dx * dx + dy * dy;
            double t = lenSq > 0.0 ? std::clamp(((wx - x0) * dx + (wy - y0) * dy) / lenSq, 0.0, 1.0) : 0.0;
            double px = x0 + dx * t, py = y0 + dy * t;
            double dist = std::hypot(wx - px, wy - py);
            if (dist < bestDist) {
                bestDist = dist;
                bestIndex = i;
            }
        }
    }

    if (bestIndex == gRoutes.size()) {
        LOG_WARN("No route near the cursor");
        return;
    }
    LOG_INFO("Removed %s '%s'", RouteKindName(gRoutes[bestIndex].kind), gRoutes[bestIndex].name.c_str());
    gRoutes.erase(gRoutes.begin() + static_cast<long>(bestIndex));
    ++gSceneRevision;
    MarkProjectDirty();
}

// Opens an in-window form for a new region.
void CreateRegion() {
    if (static_cast<int>(gRegions.size()) >= kMaxRegions) {
        LOG_WARN("Region limit reached (%d) - cannot found another", kMaxRegions);
        return;
    }
    OpenModal(ModalType::Region, {"", ""});
}

// Cycles the active region among all founded regions (Tab).
void CycleActiveRegion() {
    if (gRegions.empty()) {
        LOG_WARN("No regions founded yet - press N to found one");
        return;
    }
    std::vector<int> ids;
    ids.reserve(gRegions.size());
    for (const auto &entry : gRegions) ids.push_back(entry.first);
    std::sort(ids.begin(), ids.end());

    auto it = std::find(ids.begin(), ids.end(), gActiveRegionId);
    size_t nextIndex = (it == ids.end()) ? 0 : (static_cast<size_t>(it - ids.begin()) + 1) % ids.size();
    gActiveRegionId = ids[nextIndex];
    const Region &region = gRegions[gActiveRegionId];
    LOG_INFO("Active region: #%d '%s' (ruler: %s)", region.id, region.name.c_str(), region.ruler.c_str());
}

// Opens an in-window form for a city at the cursor.
void PlaceCityAtCursor() {
    std::tie(gModalCol, gModalRow) = CursorTile();
    OpenModal(ModalType::City, {"", ""});
}

// Opens an in-window form for a point of interest at the cursor.
void PlacePoiAtCursor() {
    std::tie(gModalCol, gModalRow) = CursorTile();
    gModalPoiKind = PoiKind::Landmark;
    OpenModal(ModalType::Poi, {"", ""});
}

// Opens an in-window form for a persistent DM encounter at the cursor.
void PlaceEncounterAtCursor() {
    std::tie(gModalCol, gModalRow) = CursorTile();
    gEditingEncounterIndex = -1;
    OpenModal(ModalType::Encounter, {"", ""});
}

void EditEncounter(size_t index) {
    if (index >= gEncounters.size()) return;
    const Encounter &encounter = gEncounters[index];
    gModalCol = encounter.col;
    gModalRow = encounter.row;
    gEditingEncounterIndex = static_cast<int>(index);
    OpenModal(ModalType::Encounter, {encounter.name, encounter.description});
}

bool ShowMarkerInfoAtTile(int32_t col, int32_t row) {
    if (gPlayerView && gFogData.count(TileKey(col, row)) != 0) return false;
    auto cityIt = std::find_if(gCities.begin(), gCities.end(),
                               [&](const City &city) { return city.col == col && city.row == row; });
    if (cityIt != gCities.end()) {
        gInfoTitle = "CITY DETAILS";
        gInfoLines = {"NAME: " + cityIt->name, "RULER: " + cityIt->ruler,
                      "TILE: " + std::to_string(col) + " " + std::to_string(row)};
        OpenModal(ModalType::Info, {});
        return true;
    }
    auto poiIt = std::find_if(gPois.begin(), gPois.end(),
                              [&](const PointOfInterest &poi) { return poi.col == col && poi.row == row; });
    if (poiIt != gPois.end()) {
        gInfoTitle = "POINT OF INTEREST";
        gInfoLines = {"TYPE: " + std::string(PoiKindName(poiIt->kind)), "NAME: " + poiIt->name};
        std::string details = poiIt->description.empty() ? "None" : poiIt->description;
        constexpr size_t detailLineLength = 55;
        gInfoLines.push_back("DETAILS: " + details.substr(0, detailLineLength));
        if (details.size() > detailLineLength)
            gInfoLines.push_back("         " + details.substr(detailLineLength, detailLineLength));
        gInfoLines.push_back("TILE: " + std::to_string(col) + " " + std::to_string(row));
        OpenModal(ModalType::Info, {});
        return true;
    }
    if (!gPlayerView) {
        auto encounterIt = std::find_if(
            gEncounters.begin(), gEncounters.end(),
            [&](const Encounter &encounter) { return encounter.col == col && encounter.row == row; });
        if (encounterIt != gEncounters.end()) {
            EditEncounter(static_cast<size_t>(encounterIt - gEncounters.begin()));
            return true;
        }
    }
    return false;
}

bool ShowMarkerInfoAtCursor() {
    auto [col, row] = CursorTile();
    return ShowMarkerInfoAtTile(col, row);
}

std::string Lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

void RunWorldSearch(const std::string &query) {
    std::string needle = Lowercase(query);
    if (needle.empty()) return;
    if (needle != gLastSearchQuery || gSearchMatches.empty()) {
        gLastSearchQuery = needle;
        gSearchMatches.clear();
        gSearchMatchIndex = 0;
        auto matches = [&](const std::string &name) { return Lowercase(name).find(needle) != std::string::npos; };

        for (const City &city : gCities) {
            if (!matches(city.name) && !matches(city.ruler)) continue;
            auto [x, y] = TileCenterWorld(city.col, city.row);
            gSearchMatches.push_back({"CITY: " + city.name, x, y});
        }
        for (const PointOfInterest &poi : gPois) {
            if (!matches(poi.name) && !matches(PoiKindName(poi.kind)) && !matches(poi.description)) continue;
            auto [x, y] = TileCenterWorld(poi.col, poi.row);
            gSearchMatches.push_back({std::string(PoiKindName(poi.kind)) + ": " + poi.name, x, y});
        }
        if (!gPlayerView) {
            for (const Encounter &encounter : gEncounters) {
                if (!matches(encounter.name) && !matches(encounter.description)) continue;
                auto [x, y] = TileCenterWorld(encounter.col, encounter.row);
                gSearchMatches.push_back({"ENCOUNTER: " + encounter.name, x, y});
            }
        }
        for (const auto &regionEntry : gRegions) {
            const Region &region = regionEntry.second;
            if (!matches(region.name) && !matches(region.ruler)) continue;
            double sumX = 0.0, sumY = 0.0;
            size_t count = 0;
            for (const auto &tile : gRegionData) {
                if (tile.second != region.id) continue;
                auto [col, row] = TileCoords(tile.first);
                auto [x, y] = TileCenterWorld(col, row);
                sumX += x;
                sumY += y;
                ++count;
            }
            if (count != 0)
                gSearchMatches.push_back({"REGION: " + region.name, sumX / count, sumY / count});
        }
        for (const Route &route : gRoutes) {
            if ((!matches(route.name) && !matches(RouteKindName(route.kind))) || route.points.empty()) continue;
            double sumX = 0.0, sumY = 0.0;
            for (const auto &point : route.points) {
                sumX += point.first;
                sumY += point.second;
            }
            gSearchMatches.push_back({std::string(RouteKindName(route.kind)) + ": " + route.name,
                                      sumX / route.points.size(), sumY / route.points.size()});
        }
    } else {
        gSearchMatchIndex = (gSearchMatchIndex + 1) % gSearchMatches.size();
    }

    if (gSearchMatches.empty()) {
        gLastFoundLabel = "NO MATCH FOR: " + query;
        LOG_WARN("No city, POI, encounter, region or route matches '%s'", query.c_str());
        return;
    }
    const SearchResult &result = gSearchMatches[gSearchMatchIndex];
    double canvasCenterX = kUiWidth + (gWindowWidth - kUiWidth) * 0.5;
    gCameraX = result.worldX - canvasCenterX / gZoom;
    gCameraY = result.worldY - (gWindowHeight * 0.5) / gZoom;
    gLastFoundLabel = result.label + "  " + std::to_string(gSearchMatchIndex + 1) + "/" +
                      std::to_string(gSearchMatches.size());
    LOG_INFO("Found %s (%zu of %zu)", result.label.c_str(), gSearchMatchIndex + 1, gSearchMatches.size());
}

void CloseModal(bool accept) {
    if (gModalType == ModalType::None) return;
    if (gModalType == ModalType::Confirm) {
        ConfirmAction action = gConfirmAction;
        gModalType = ModalType::None;
        gConfirmAction = ConfirmAction::None;
        gInfoTitle.clear();
        gInfoLines.clear();
        if (accept) {
            if (action == ConfirmAction::ClearLayer) ClearActiveLayerNow();
            else if (action == ConfirmAction::LoadProject) LoadProjectFromPath(gProjectFile, true);
            else if (action == ConfirmAction::OverwriteProject) SaveProjectNow();
            else if (action == ConfirmAction::GenerateRelief) GenerateTerrainRelief();
            else if (action == ConfirmAction::RecoverAutosave) {
                if (LoadProjectFromPath(kAutosaveFile, false)) {
                    gProjectDirty = true;
                    LOG_INFO("Recovered autosaved work; save the project to keep it permanently");
                }
            }
        }
        UpdateWindowTitle();
        return;
    }
    if (gModalType == ModalType::ProjectName) {
        if (accept && !gModalFields.empty()) {
            std::string safeName;
            for (char c : gModalFields[0]) {
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') safeName += c;
                else if (std::isspace(static_cast<unsigned char>(c))) safeName += '_';
            }
            if (!safeName.empty()) {
                gProjectFile = safeName + ".txt";
                MarkProjectDirty();
                SaveProjectConfig();
                LOG_INFO("Active project file: %s", gProjectFile.c_str());
            }
        }
        gModalType = ModalType::None;
        gModalFields.clear();
        UpdateWindowTitle();
        return;
    }
    if (gModalType == ModalType::Search) {
        std::string query = gModalFields.empty() ? std::string{} : gModalFields[0];
        gModalType = ModalType::None;
        gModalFields.clear();
        if (accept) RunWorldSearch(query);
        UpdateWindowTitle();
        return;
    }
    if (accept) {
        if (gModalType == ModalType::Region) {
            int id = gNextRegionId++;
            Region region;
            region.id = id;
            region.color = kRegionPalette[(id - 1) % kRegionPaletteSize];
            region.name = gModalFields[0].empty() ? ("Region " + std::to_string(id)) : gModalFields[0];
            region.ruler = gModalFields[1].empty() ? "Unclaimed" : gModalFields[1];
            gRegions[id] = region;
            gActiveRegionId = id;
            gPaintMode = PaintMode::Region;
            LOG_INFO("Founded region #%d '%s' (ruler: %s)", id, region.name.c_str(), region.ruler.c_str());
        } else if (gModalType == ModalType::City) {
            City city;
            city.col = gModalCol;
            city.row = gModalRow;
            city.name = gModalFields[0].empty() ? "Unnamed settlement" : gModalFields[0];
            city.ruler = gModalFields[1].empty() ? "Unclaimed" : gModalFields[1];
            gCities.push_back(city);
            LOG_INFO("City '%s' founded at (%d, %d)", city.name.c_str(), city.col, city.row);
        } else if (gModalType == ModalType::Poi) {
            PointOfInterest poi;
            poi.col = gModalCol;
            poi.row = gModalRow;
            poi.kind = gModalPoiKind;
            poi.name = gModalFields[0].empty() ? PoiKindName(poi.kind) : gModalFields[0];
            poi.description = gModalFields[1];
            gPois.push_back(poi);
            LOG_INFO("%s '%s' placed at (%d, %d)", PoiKindName(poi.kind), poi.name.c_str(), poi.col, poi.row);
        } else if (gModalType == ModalType::Encounter) {
            Encounter encounter;
            encounter.col = gModalCol;
            encounter.row = gModalRow;
            encounter.name = gModalFields[0].empty() ? "Encounter" : gModalFields[0];
            encounter.description = gModalFields[1];
            if (gEditingEncounterIndex >= 0 &&
                gEditingEncounterIndex < static_cast<int>(gEncounters.size())) {
                gEncounters[static_cast<size_t>(gEditingEncounterIndex)] = std::move(encounter);
                LOG_INFO("Encounter '%s' updated at (%d, %d)",
                         gEncounters[static_cast<size_t>(gEditingEncounterIndex)].name.c_str(),
                         gModalCol, gModalRow);
            } else {
                gEncounters.push_back(std::move(encounter));
                LOG_INFO("Encounter '%s' placed at (%d, %d)", gEncounters.back().name.c_str(),
                         gModalCol, gModalRow);
            }
        } else if (gModalType == ModalType::Route) {
            Route route;
            route.kind = gModalRouteKind;
            route.name = gModalFields[0].empty() ? RouteKindName(route.kind) : gModalFields[0];
            route.points = gRoutePoints;
            gRoutes.push_back(std::move(route));
            gRoutePoints.clear();
            LOG_INFO("%s '%s' created", RouteKindName(gModalRouteKind), gRoutes.back().name.c_str());
        }
        ++gSceneRevision;
        MarkProjectDirty();
    } else if (gModalType == ModalType::Route) {
        gRoutePoints.clear();
    }
    gModalType = ModalType::None;
    gModalFields.clear();
    gInfoTitle.clear();
    gInfoLines.clear();
    gModalField = 0;
    gEditingEncounterIndex = -1;
    UpdateWindowTitle();
}

// Removes whichever world marker sits at the cursor tile.
void RemoveMarkerAtCursor() {
    auto [col, row] = CursorTile();
    auto cityIt = std::find_if(gCities.begin(), gCities.end(),
                               [&](const City &c) { return c.col == col && c.row == row; });
    if (cityIt != gCities.end()) {
        LOG_INFO("Removed city '%s'", cityIt->name.c_str());
        gCities.erase(cityIt);
        ++gSceneRevision;
        MarkProjectDirty();
        return;
    }
    auto poiIt = std::find_if(gPois.begin(), gPois.end(),
                              [&](const PointOfInterest &poi) {
                                  return poi.col == col && poi.row == row;
                              });
    if (poiIt != gPois.end()) {
        LOG_INFO("Removed %s '%s'", PoiKindName(poiIt->kind), poiIt->name.c_str());
        gPois.erase(poiIt);
        ++gSceneRevision;
        MarkProjectDirty();
        return;
    }
    auto encounterIt = std::find_if(gEncounters.begin(), gEncounters.end(),
                                     [&](const Encounter &encounter) {
                                         return encounter.col == col && encounter.row == row;
                                     });
    if (encounterIt != gEncounters.end()) {
        LOG_INFO("Removed encounter '%s'", encounterIt->name.c_str());
        gEncounters.erase(encounterIt);
        ++gSceneRevision;
        MarkProjectDirty();
        return;
    }
    LOG_WARN("No city, point of interest, or encounter at (%d, %d)", col, row);
}

// Prints a summary of all regions, cities, and points of interest to the console/log.
void PrintWorldInfo() {
    LOG_INFO("--- Regions (%zu) ---", gRegions.size());
    std::vector<int> ids;
    ids.reserve(gRegions.size());
    for (const auto &entry : gRegions) ids.push_back(entry.first);
    std::sort(ids.begin(), ids.end());
    for (int id : ids) {
        size_t tiles = 0;
        for (const auto &entry : gRegionData) {
            if (entry.second == id) ++tiles;
        }
        const Region &region = gRegions[id];
        LOG_INFO("  #%d '%s' - ruler: %s (%zu tiles)%s", id, region.name.c_str(), region.ruler.c_str(),
                  tiles, id == gActiveRegionId ? " [active]" : "");
    }
    LOG_INFO("--- Cities (%zu) ---", gCities.size());
    for (const auto &city : gCities) {
        LOG_INFO("  '%s' - ruler: %s at (%d, %d)", city.name.c_str(), city.ruler.c_str(), city.col, city.row);
    }
    LOG_INFO("--- Points of Interest (%zu) ---", gPois.size());
    for (const auto &poi : gPois) {
        LOG_INFO("  [%s] '%s' at (%d, %d)%s%s", PoiKindName(poi.kind), poi.name.c_str(), poi.col, poi.row,
                  poi.description.empty() ? "" : " - ", poi.description.c_str());
    }
    LOG_INFO("--- Encounters (%zu) ---", gEncounters.size());
    for (const auto &encounter : gEncounters) {
        LOG_INFO("  '%s' at (%d, %d)%s%s", encounter.name.c_str(), encounter.col, encounter.row,
                 encounter.description.empty() ? "" : " - ", encounter.description.c_str());
    }
    LOG_INFO("--- Routes (%zu) ---", gRoutes.size());
    for (const auto &route : gRoutes) {
        double length = 0.0;
        for (size_t i = 0; i + 1 < route.points.size(); ++i) {
            length += std::hypot(route.points[i + 1].first - route.points[i].first,
                                 route.points[i + 1].second - route.points[i].second);
        }
        LOG_INFO("  [%s] '%s' (%zu points, length ~%.0f tiles)", RouteKindName(route.kind),
                  route.name.c_str(), route.points.size(), length / kTileSize);
    }
    LOG_INFO("--- Fog of War: %zu hidden tiles; player view %s ---", gFogData.size(),
             gPlayerView ? "on" : "off");
}

void ToggleEncounterPlacement() {
    gPlacementMode = gPlacementMode == PlacementMode::Encounter ? PlacementMode::None
                                                                 : PlacementMode::Encounter;
    LOG_INFO("Encounter placement: %s",
             gPlacementMode == PlacementMode::Encounter ? "click a map tile" : "cancelled");
}

// Dumps the map viewport (excluding the GUI sidebar) to a 24-bit BMP file.
void ExportScreenshot() {
    int sidebarPixels = std::min(static_cast<int>(kUiWidth), gWindowWidth - 1);
    int width = gWindowWidth - sidebarPixels, height = gWindowHeight;
    if (width <= 0 || height <= 0) return;

    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(sidebarPixels, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

    int rowSize = width * 3;
    int padding = (4 - (rowSize % 4)) % 4;
    int32_t dataSize = (rowSize + padding) * height;
    int32_t fileSize = 54 + dataSize;

    FILE *f = nullptr;
#ifdef _MSC_VER
    fopen_s(&f, kScreenshotFile, "wb");
#else
    f = std::fopen(kScreenshotFile, "wb");
#endif
    if (!f) {
        LOG_ERROR("Failed to open %s for writing", kScreenshotFile);
        return;
    }

    unsigned char header[54] = {0};
    header[0] = 'B';
    header[1] = 'M';
    std::memcpy(&header[2], &fileSize, 4);
    int32_t dataOffset = 54, infoHeaderSize = 40;
    int16_t planes = 1, bpp = 24;
    std::memcpy(&header[10], &dataOffset, 4);
    std::memcpy(&header[14], &infoHeaderSize, 4);
    std::memcpy(&header[18], &width, 4);
    std::memcpy(&header[22], &height, 4);
    std::memcpy(&header[26], &planes, 2);
    std::memcpy(&header[28], &bpp, 2);
    std::memcpy(&header[34], &dataSize, 4);
    std::fwrite(header, 1, sizeof(header), f);

    // BMP rows are bottom-up, which already matches glReadPixels' row order.
    std::vector<unsigned char> row(static_cast<size_t>(rowSize) + padding, 0);
    for (int y = 0; y < height; ++y) {
        const unsigned char *src = pixels.data() + static_cast<size_t>(y) * rowSize;
        for (int x = 0; x < width; ++x) {
            row[static_cast<size_t>(x) * 3 + 0] = src[x * 3 + 2]; // B
            row[static_cast<size_t>(x) * 3 + 1] = src[x * 3 + 1]; // G
            row[static_cast<size_t>(x) * 3 + 2] = src[x * 3 + 0]; // R
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    LOG_INFO("Exported screenshot to %s (%dx%d)", kScreenshotFile, width, height);
}

void UpdateWindowTitle() {
    auto [col, row] = CursorTile();
    char title[320];
    const char *modeLabel = PaintModeName(gPaintMode);
    char paintInfo[96];
    if (gPaintMode == PaintMode::Terrain)
        std::snprintf(paintInfo, sizeof(paintInfo), "%s", kTerrainNames[gBrush]);
    else if (gPaintMode == PaintMode::Elevation)
        std::snprintf(paintInfo, sizeof(paintInfo), "%s %+d (%+d m)",
                      ElevationEditModeName(gElevationEditMode), gElevationBrush,
                      (gElevationBrush - gSeaLevel) * gMetresPerElevationLevel);
    else if (gPaintMode == PaintMode::Fog)
        std::snprintf(paintInfo, sizeof(paintInfo), "hide / reveal");
    else
        std::snprintf(paintInfo, sizeof(paintInfo), "region");
    char regionInfo[96] = "none";
    if (gPaintMode == PaintMode::Region && gActiveRegionId != 0) {
        std::snprintf(regionInfo, sizeof(regionInfo), "#%d %s", gActiveRegionId,
                      gRegions[gActiveRegionId].name.c_str());
    }
    std::snprintf(title, sizeof(title),
                  "DND Map Drawer%s - Tool: %s%s | Mode: %s | Brush: %s x%d | Region: %s | Grid: %s %s | "
                  "Zoom: %.0f%% | Undo: %zu Redo: %zu | (%d, %d)",
                  gProjectDirty ? " *" : "", ToolName(gToolMode), gRoundBrush ? " (round)" : "", modeLabel, paintInfo,
                  gBrushRadius * 2 + 1, regionInfo, gShowGrid ? "on" : "off",
                  gHexGrid ? "(hex)" : "(square)", gZoom * 100.0,
                  gUndoStack.size(), gRedoStack.size(), col, row);
    glfwSetWindowTitle(gWindow, title);
}

void SaveMap() {
    std::ofstream out(kMapFile);
    if (!out) {
        LOG_ERROR("Failed to open %s for writing", kMapFile);
        return;
    }
    out << gMapData.size() << '\n';
    for (const auto &entry : gMapData) {
        int32_t col = static_cast<int32_t>(entry.first >> 32);
        int32_t row = static_cast<int32_t>(entry.first & 0xFFFFFFFFu);
        out << col << ' ' << row << ' ' << static_cast<int>(entry.second) << '\n';
    }
    LOG_INFO("Map saved to %s (%zu tiles)", kMapFile, gMapData.size());
}

void LoadMap() {
    std::ifstream in(kMapFile);
    if (!in) {
        LOG_ERROR("Failed to open %s for reading", kMapFile);
        return;
    }
    size_t count = 0;
    in >> count;
    gMapData.clear();
    gMapData.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        int32_t col = 0, row = 0;
        int terrain = 0;
        in >> col >> row >> terrain;
        terrain = std::clamp(terrain, 0, kTerrainCount - 1);
        if (terrain != 0) {
            gMapData[TileKey(col, row)] = static_cast<uint8_t>(terrain);
        }
    }
    LOG_INFO("Map loaded from %s (%zu tiles)", kMapFile, gMapData.size());
}

void SaveElevation() {
    std::ofstream out(kElevationFile);
    if (!out) {
        LOG_ERROR("Failed to open %s for writing", kElevationFile);
        return;
    }
    out << gElevationData.size() << '\n';
    for (const auto &entry : gElevationData) {
        int32_t col = static_cast<int32_t>(entry.first >> 32);
        int32_t row = static_cast<int32_t>(entry.first & 0xFFFFFFFFu);
        out << col << ' ' << row << ' ' << static_cast<int>(entry.second) << '\n';
    }
    LOG_INFO("Elevation saved to %s (%zu tiles)", kElevationFile, gElevationData.size());
}

void LoadElevation() {
    std::ifstream in(kElevationFile);
    if (!in) {
        gElevationData.clear();
        LOG_WARN("No %s found; using zero elevation", kElevationFile);
        return;
    }
    size_t count = 0;
    in >> count;
    gElevationData.clear();
    gElevationData.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        int32_t col = 0, row = 0;
        int elevation = 0;
        in >> col >> row >> elevation;
        elevation = std::clamp(elevation, kMinElevation, kMaxElevation);
        if (elevation != 0) {
            gElevationData[TileKey(col, row)] = static_cast<int8_t>(elevation);
        }
    }
    LOG_INFO("Elevation loaded from %s (%zu tiles)", kElevationFile, gElevationData.size());
}

void SaveRegions() {
    std::ofstream out(kRegionsFile);
    if (!out) {
        LOG_ERROR("Failed to open %s for writing", kRegionsFile);
        return;
    }
    out << gRegions.size() << '\n';
    for (const auto &entry : gRegions) {
        const Region &r = entry.second;
        out << r.id << ' ' << r.color.r << ' ' << r.color.g << ' ' << r.color.b << '\n';
        out << r.name << '\n' << r.ruler << '\n';
    }
    out << gRegionData.size() << '\n';
    for (const auto &entry : gRegionData) {
        int32_t col = static_cast<int32_t>(entry.first >> 32);
        int32_t row = static_cast<int32_t>(entry.first & 0xFFFFFFFFu);
        out << col << ' ' << row << ' ' << static_cast<int>(entry.second) << '\n';
    }
    LOG_INFO("Regions saved to %s (%zu regions, %zu tiles)", kRegionsFile, gRegions.size(),
              gRegionData.size());
}

void LoadRegions() {
    std::ifstream in(kRegionsFile);
    if (!in) {
        LOG_ERROR("Failed to open %s for reading", kRegionsFile);
        return;
    }
    gRegions.clear();
    gRegionData.clear();
    gNextRegionId = 1;
    gActiveRegionId = 0;

    size_t regionCount = 0;
    in >> regionCount;
    in.ignore();
    for (size_t i = 0; i < regionCount; ++i) {
        Region region;
        in >> region.id >> region.color.r >> region.color.g >> region.color.b;
        in.ignore();
        std::getline(in, region.name);
        std::getline(in, region.ruler);
        gRegions[region.id] = region;
        gNextRegionId = std::max(gNextRegionId, region.id + 1);
    }

    size_t tileCount = 0;
    in >> tileCount;
    for (size_t i = 0; i < tileCount; ++i) {
        int32_t col = 0, row = 0;
        int regionId = 0;
        in >> col >> row >> regionId;
        if (regionId != 0) gRegionData[TileKey(col, row)] = static_cast<uint8_t>(regionId);
    }
    LOG_INFO("Regions loaded from %s (%zu regions, %zu tiles)", kRegionsFile, gRegions.size(),
              gRegionData.size());
}

void SaveCities() {
    std::ofstream out(kCitiesFile);
    if (!out) {
        LOG_ERROR("Failed to open %s for writing", kCitiesFile);
        return;
    }
    out << gCities.size() << '\n';
    for (const auto &city : gCities) {
        out << city.col << ' ' << city.row << '\n' << city.name << '\n' << city.ruler << '\n';
    }
    LOG_INFO("Cities saved to %s (%zu cities)", kCitiesFile, gCities.size());
}

void LoadCities() {
    std::ifstream in(kCitiesFile);
    if (!in) {
        LOG_ERROR("Failed to open %s for reading", kCitiesFile);
        return;
    }
    gCities.clear();
    size_t count = 0;
    in >> count;
    in.ignore();
    for (size_t i = 0; i < count; ++i) {
        City city;
        in >> city.col >> city.row;
        in.ignore();
        std::getline(in, city.name);
        std::getline(in, city.ruler);
        gCities.push_back(city);
    }
    LOG_INFO("Cities loaded from %s (%zu cities)", kCitiesFile, gCities.size());
}

void SavePois() {
    std::ofstream out(kPoisFile);
    if (!out) {
        LOG_ERROR("Failed to open %s for writing", kPoisFile);
        return;
    }
    out << gPois.size() << '\n';
    for (const auto &poi : gPois) {
        out << poi.col << ' ' << poi.row << ' ' << static_cast<int>(poi.kind) << '\n' << poi.name << '\n'
            << poi.description << '\n';
    }
    LOG_INFO("Points of interest saved to %s (%zu)", kPoisFile, gPois.size());
}

void LoadPois() {
    std::ifstream in(kPoisFile);
    if (!in) {
        LOG_ERROR("Failed to open %s for reading", kPoisFile);
        return;
    }
    gPois.clear();
    size_t count = 0;
    in >> count;
    in.ignore();
    for (size_t i = 0; i < count; ++i) {
        PointOfInterest poi;
        int kindValue = 0;
        in >> poi.col >> poi.row >> kindValue;
        in.ignore();
        poi.kind = static_cast<PoiKind>(std::clamp(kindValue, 0, kPoiKindCount - 1));
        std::getline(in, poi.name);
        std::getline(in, poi.description);
        gPois.push_back(poi);
    }
    LOG_INFO("Points of interest loaded from %s (%zu)", kPoisFile, gPois.size());
}

void SaveRoutes() {
    std::ofstream out(kRoutesFile);
    if (!out) {
        LOG_ERROR("Failed to open %s for writing", kRoutesFile);
        return;
    }
    out << gRoutes.size() << '\n';
    for (const auto &route : gRoutes) {
        out << static_cast<int>(route.kind) << ' ' << route.points.size() << '\n' << route.name << '\n';
        for (const auto &p : route.points) out << p.first << ' ' << p.second << '\n';
    }
    LOG_INFO("Routes saved to %s (%zu)", kRoutesFile, gRoutes.size());
}

void LoadRoutes() {
    std::ifstream in(kRoutesFile);
    if (!in) {
        LOG_ERROR("Failed to open %s for reading", kRoutesFile);
        return;
    }
    gRoutes.clear();
    size_t count = 0;
    in >> count;
    for (size_t i = 0; i < count; ++i) {
        int kindValue = 0;
        size_t pointCount = 0;
        in >> kindValue >> pointCount;
        in.ignore();
        Route route;
        route.kind = (kindValue == 0) ? RouteKind::River : RouteKind::TradeRoute;
        std::getline(in, route.name);
        route.points.reserve(pointCount);
        for (size_t p = 0; p < pointCount; ++p) {
            double x = 0.0, y = 0.0;
            in >> x >> y;
            route.points.emplace_back(x, y);
        }
        gRoutes.push_back(std::move(route));
    }
    LOG_INFO("Routes loaded from %s (%zu)", kRoutesFile, gRoutes.size());
}

bool SaveProjectToPath(const std::string &path, bool announce) {
    std::string error;
    if (!SaveProjectDocument(path, gProjectDocument, error)) {
        LOG_ERROR("Failed to save %s: %s", path.c_str(), error.c_str());
        return false;
    }
    if (announce)
        LOG_INFO("Project saved to %s (version %d, %zu terrain, %zu elevation, %zu fog tiles)",
                 path.c_str(), kProjectVersion, gMapData.size(), gElevationData.size(), gFogData.size());
    return true;
}

void SaveProjectNow() {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(gProjectFile, ec)) {
        fs::create_directories(kBackupDirectory, ec);
        std::time_t now = std::time(nullptr);
        std::tm localTime{};
#ifdef _MSC_VER
        localtime_s(&localTime, &now);
#else
        localtime_r(&now, &localTime);
#endif
        char timestamp[32];
        std::strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &localTime);
        fs::path source(gProjectFile);
        fs::path backup = fs::path(kBackupDirectory) /
                          (source.stem().string() + "_" + timestamp + source.extension().string());
        ec.clear();
        fs::copy_file(source, backup, fs::copy_options::overwrite_existing, ec);
        if (ec) LOG_WARN("Could not create backup %s: %s", backup.string().c_str(), ec.message().c_str());
        else LOG_INFO("Backup created: %s", backup.string().c_str());
    }
    if (SaveProjectToPath(gProjectFile, true)) gProjectDirty = false;
}

void RequestProjectSave() {
    std::error_code ec;
    if (std::filesystem::exists(gProjectFile, ec)) {
        OpenConfirmation(ConfirmAction::OverwriteProject, "OVERWRITE PROJECT",
                         {"REPLACE: " + gProjectFile, "A TIMESTAMPED BACKUP WILL BE CREATED"});
    } else {
        SaveProjectNow();
    }
}

void SaveProjectConfig() {
    std::ofstream out(kConfigFile, std::ios::trunc);
    if (!out) {
        LOG_WARN("Could not save project filename preference to %s", kConfigFile);
        return;
    }
    out << gProjectFile << '\n';
}

void LoadProjectConfig() {
    std::ifstream in(kConfigFile);
    std::string filename;
    if (!in || !std::getline(in, filename) || filename.empty()) return;
    std::filesystem::path candidate(filename);
    if (candidate.has_parent_path() || candidate.extension() != ".txt") {
        LOG_WARN("Ignoring invalid project filename in %s", kConfigFile);
        return;
    }
    gProjectFile = candidate.filename().string();
}

bool LoadProjectFromPath(const std::string &path, bool allowLegacyFallback) {
    std::ifstream in(path);
    if (!in) {
        if (!allowLegacyFallback) {
            LOG_ERROR("Failed to open %s for reading", path.c_str());
            return false;
        }
        LOG_WARN("No %s found; loading legacy multi-file project", path.c_str());
        gFogData.clear();
        gMetresPerElevationLevel = kDefaultMetresPerElevationLevel;
        gSeaLevel = 0;
        gContourInterval = 1;
        gElevationView = false;
        gShowElevationContours = true;
        gShowHillshade = true;
        gElevationEditMode = ElevationEditMode::Set;
        LoadMap();
        LoadElevation();
        LoadRegions();
        LoadCities();
        LoadPois();
        LoadRoutes();
        ++gSceneRevision;
        gUndoStack.clear();
        gRedoStack.clear();
        gSelectionActive = false;
        gPlacementMode = PlacementMode::None;
        gTileClipboard.clear();
        gPlayerView = false;
        gMeasureStage = 0;
        gSearchMatches.clear();
        gLastSearchQuery.clear();
        gLastFoundLabel.clear();
        gProjectDirty = false;
        UpdateWindowTitle();
        return true;
    }

    in.close();
    ProjectDocument loadedDocument;
    int version = 0;
    std::string error;
    if (!LoadProjectDocument(path, loadedDocument, version, error)) {
        LOG_ERROR("Failed to load %s: %s", path.c_str(), error.c_str());
        return false;
    }
    gProjectDocument = std::move(loadedDocument);
    gElevationEditMode = ElevationEditMode::Set;
    gNextRegionId = 1;
    for (const auto &[regionId, region] : gRegions) {
        (void)region;
        gNextRegionId = std::max(gNextRegionId, regionId + 1);
    }
    gActiveRegionId = 0;
    gUndoStack.clear();
    gRedoStack.clear();
    gSelectionActive = false;
    gPlacementMode = PlacementMode::None;
    gTileClipboard.clear();
    gPlayerView = false;
    gMeasureStage = 0;
    gSearchMatches.clear();
    gLastSearchQuery.clear();
    gLastFoundLabel.clear();
    ++gSceneRevision;
    gProjectDirty = false;
    LOG_INFO("Project loaded from %s (version %d, %zu terrain tiles, %zu elevation tiles)",
             path.c_str(), version, gMapData.size(), gElevationData.size());
    UpdateWindowTitle();
    return true;
}

void ClearActiveLayerNow() {
    size_t removed = 0;
    if (gPaintMode == PaintMode::Terrain) {
        removed = gMapData.size();
        gMapData.clear();
    } else if (gPaintMode == PaintMode::Region) {
        removed = gRegionData.size();
        gRegionData.clear();
    } else if (gPaintMode == PaintMode::Elevation) {
        removed = gElevationData.size();
        gElevationData.clear();
    } else {
        removed = gFogData.size();
        gFogData.clear();
    }
    gUndoStack.clear();
    gRedoStack.clear();
    ++gSceneRevision;
    MarkProjectDirty();
    LOG_INFO("Cleared %zu tiles from the %s layer", removed, PaintModeName(gPaintMode));
}

void RequestClearActiveLayer() {
    OpenConfirmation(ConfirmAction::ClearLayer, "CLEAR LAYER",
                     {std::string("CLEAR ALL ") + PaintModeName(gPaintMode) + " TILES?",
                      "A BACKUP CAN RESTORE THE PREVIOUS PROJECT"});
}

void HideAllTerrainWithFog() {
    if (gMapData.empty()) {
        LOG_WARN("There are no terrain tiles to hide");
        return;
    }
    PaintMode previousMode = gPaintMode;
    gPaintMode = PaintMode::Fog;
    BeginStroke(gMapData.size(), 1);
    for (const auto &entry : gMapData) {
        int32_t col = static_cast<int32_t>(entry.first >> 32);
        int32_t row = static_cast<int32_t>(entry.first & 0xFFFFFFFFu);
        SetTileRecorded(col, row, 1);
    }
    EndStroke();
    gPaintMode = previousMode;
    LOG_INFO("Fogged all %zu terrain tiles", gMapData.size());
}

void GenerateTerrainRelief() {
    if (gMapData.empty()) {
        LOG_WARN("There are no terrain tiles to generate elevation from");
        return;
    }
    static constexpr int terrainHeights[kTerrainCount] = {0, 0, 1, -2, 5, 0, 2, 0};
    PaintMode previousPaintMode = gPaintMode;
    ElevationEditMode previousEditMode = gElevationEditMode;
    gPaintMode = PaintMode::Elevation;
    gElevationEditMode = ElevationEditMode::Set;
    BeginStroke(gMapData.size(), 1);
    for (const auto &entry : gMapData) {
        auto [col, row] = TileCoords(entry.first);
        SetTileRecorded(col, row, terrainHeights[entry.second]);
    }
    EndStroke();
    gPaintMode = previousPaintMode;
    gElevationEditMode = previousEditMode;
    LOG_INFO("Generated terrain-based relief for %zu map tiles", gMapData.size());
}

void RequestProjectLoad() {
    if (gProjectDirty) {
        OpenConfirmation(ConfirmAction::LoadProject, "DISCARD CHANGES",
                         {"LOAD: " + gProjectFile, "UNSAVED CHANGES WILL BE LOST"});
    } else {
        LoadProjectFromPath(gProjectFile, true);
    }
}

void CheckForRecoveryAutosave() {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(kAutosaveFile, ec)) return;
    const bool projectMissing = !fs::exists(gProjectFile, ec);
    ec.clear();
    const auto autosaveTime = fs::last_write_time(kAutosaveFile, ec);
    if (ec) return;
    ec.clear();
    const auto projectTime = projectMissing ? fs::file_time_type::min() : fs::last_write_time(gProjectFile, ec);
    if (!ec && (projectMissing || autosaveTime > projectTime)) {
        OpenConfirmation(ConfirmAction::RecoverAutosave, "RECOVER AUTOSAVE",
                         {"NEWER AUTOSAVED WORK WAS FOUND", "RECOVER IT NOW?"});
    }
}

void PaintTileAtWorld(double worldX, double worldY, int value) {
    if (value == kNoPaintValue) return;
    StampBrushAtWorld(worldX, worldY, value);
}

void PaintAtCursor(int value) {
    double mouseX = 0.0, mouseY = 0.0;
    glfwGetCursorPos(gWindow, &mouseX, &mouseY);

    double worldX = gCameraX + mouseX / gZoom;
    double worldY = gCameraY + mouseY / gZoom;
    PaintTileAtWorld(worldX, worldY, value);
}

void SelectionBounds(int32_t &minCol, int32_t &minRow, int32_t &maxCol, int32_t &maxRow) {
    minCol = std::min(gSelectionStartCol, gSelectionEndCol);
    minRow = std::min(gSelectionStartRow, gSelectionEndRow);
    maxCol = std::max(gSelectionStartCol, gSelectionEndCol);
    maxRow = std::max(gSelectionStartRow, gSelectionEndRow);
}

uint64_t SelectionTileCount() {
    if (!gSelectionActive && !gSelectionDragging) return 0;
    int32_t minCol = 0, minRow = 0, maxCol = 0, maxRow = 0;
    SelectionBounds(minCol, minRow, maxCol, maxRow);
    return static_cast<uint64_t>(static_cast<int64_t>(maxCol) - minCol + 1) *
           static_cast<uint64_t>(static_cast<int64_t>(maxRow) - minRow + 1);
}

template <typename Transform>
void TransformSelection(PaintMode layer, int reserveValue, Transform transform) {
    if (!gSelectionActive) return;
    int32_t minCol = 0, minRow = 0, maxCol = 0, maxRow = 0;
    SelectionBounds(minCol, minRow, maxCol, maxRow);
    PaintMode previousMode = gPaintMode;
    gPaintMode = layer;
    BeginStroke(EstimatedTileCount(minCol, minRow, maxCol, maxRow), reserveValue);
    for (int32_t row = minRow; row <= maxRow; ++row) {
        for (int32_t col = minCol; col <= maxCol; ++col) {
            int oldValue = GetLayerValue(layer, TileKey(col, row));
            int newValue = transform(oldValue);
            if (newValue != oldValue) SetTileRecorded(col, row, newValue);
        }
    }
    EndStroke();
    gPaintMode = previousMode;
}

void ClearSelectionLayer(PaintMode layer) {
    if (!gSelectionActive) return;
    int32_t minCol = 0, minRow = 0, maxCol = 0, maxRow = 0;
    SelectionBounds(minCol, minRow, maxCol, maxRow);
    std::vector<uint64_t> keys;
    size_t selectionEstimate = EstimatedTileCount(minCol, minRow, maxCol, maxRow);
    auto gather = [&](const auto &tileLayer) {
        keys.reserve(std::min(tileLayer.size(), selectionEstimate));
        if (static_cast<uint64_t>(tileLayer.size()) < SelectionTileCount()) {
            for (const auto &entry : tileLayer) {
                auto [col, row] = TileCoords(entry.first);
                if (col >= minCol && col <= maxCol && row >= minRow && row <= maxRow)
                    keys.push_back(entry.first);
            }
        } else {
            for (int32_t row = minRow; row <= maxRow; ++row) {
                for (int32_t col = minCol; col <= maxCol; ++col) {
                    uint64_t key = TileKey(col, row);
                    if (tileLayer.find(key) != tileLayer.end()) keys.push_back(key);
                }
            }
        }
    };
    if (layer == PaintMode::Terrain) gather(gMapData);
    else if (layer == PaintMode::Region) gather(gRegionData);
    else if (layer == PaintMode::Elevation) gather(gElevationData);
    else gather(gFogData);
    PaintMode previousMode = gPaintMode;
    gPaintMode = layer;
    BeginStroke(keys.size(), 0);
    for (uint64_t key : keys) {
        auto [col, row] = TileCoords(key);
        SetTileRecorded(col, row, 0);
    }
    EndStroke();
    gPaintMode = previousMode;
}

void CopySelection(bool cut) {
    if (!gSelectionActive) return;
    int32_t minCol = 0, minRow = 0, maxCol = 0, maxRow = 0;
    SelectionBounds(minCol, minRow, maxCol, maxRow);
    gTileClipboard.clear();
    gClipboardLayer = gPaintMode;
    gClipboardWidth = maxCol - minCol + 1;
    gClipboardHeight = maxRow - minRow + 1;
    size_t reserveCount = EstimatedTileCount(minCol, minRow, maxCol, maxRow);
    auto copyLayer = [&](const auto &layer) {
        gTileClipboard.reserve(std::min(reserveCount, layer.size()));
        if (static_cast<uint64_t>(layer.size()) < SelectionTileCount()) {
            for (const auto &entry : layer) {
                auto [col, row] = TileCoords(entry.first);
                if (col >= minCol && col <= maxCol && row >= minRow && row <= maxRow)
                    gTileClipboard.push_back({col - minCol, row - minRow,
                                              static_cast<int16_t>(entry.second)});
            }
        } else {
            for (int32_t row = minRow; row <= maxRow; ++row) {
                for (int32_t col = minCol; col <= maxCol; ++col) {
                    auto it = layer.find(TileKey(col, row));
                    if (it != layer.end())
                        gTileClipboard.push_back({col - minCol, row - minRow,
                                                  static_cast<int16_t>(it->second)});
                }
            }
        }
    };
    if (gClipboardLayer == PaintMode::Terrain) copyLayer(gMapData);
    else if (gClipboardLayer == PaintMode::Region) copyLayer(gRegionData);
    else if (gClipboardLayer == PaintMode::Elevation) copyLayer(gElevationData);
    else copyLayer(gFogData);
    if (cut) ClearSelectionLayer(gClipboardLayer);
    LOG_INFO("%s %zu non-empty %s tiles from a %llu-tile selection", cut ? "Cut" : "Copied",
             gTileClipboard.size(), PaintModeName(gClipboardLayer),
             static_cast<unsigned long long>(SelectionTileCount()));
}

void PasteSelection() {
    if (gTileClipboard.empty()) {
        LOG_WARN("Tile clipboard is empty");
        return;
    }
    auto [anchorCol, anchorRow] = CursorTile();
    PaintMode previousMode = gPaintMode;
    ElevationEditMode previousElevationMode = gElevationEditMode;
    gPaintMode = gClipboardLayer;
    if (gClipboardLayer == PaintMode::Elevation) gElevationEditMode = ElevationEditMode::Set;
    BeginStroke(gTileClipboard.size(), 1);
    for (const auto &tile : gTileClipboard)
        SetTileRecorded(anchorCol + tile.dc, anchorRow + tile.dr, tile.value);
    EndStroke();
    gPaintMode = previousMode;
    gElevationEditMode = previousElevationMode;
    gSelectionStartCol = anchorCol;
    gSelectionStartRow = anchorRow;
    gSelectionEndCol = anchorCol + gClipboardWidth - 1;
    gSelectionEndRow = anchorRow + gClipboardHeight - 1;
    gSelectionActive = true;
    LOG_INFO("Pasted %zu %s tiles", gTileClipboard.size(), PaintModeName(gClipboardLayer));
}

void PaintSelection() {
    int value = ActivePaintValue();
    if (value == kNoPaintValue) return;
    PaintMode layer = gPaintMode;
    TransformSelection(layer, value, [=](int /*oldValue*/) { return value; });
}

void AdjustSelectionElevation(int delta) {
    ElevationEditMode previousMode = gElevationEditMode;
    gElevationEditMode = ElevationEditMode::Set;
    TransformSelection(PaintMode::Elevation, delta > 0 ? 1 : kNoPaintValue, [=](int oldValue) {
        return std::clamp(oldValue + delta, kMinElevation, kMaxElevation);
    });
    gElevationEditMode = previousMode;
}

void AppendUiRect(std::vector<float> &vertices, float x, float y, float w, float h,
                  const Vec3 &color, float alpha = 1.0f) {
    const float quad[] = {
        x, y, color.r, color.g, color.b, alpha, x + w, y, color.r, color.g, color.b, alpha,
        x + w, y + h, color.r, color.g, color.b, alpha,
        x, y, color.r, color.g, color.b, alpha, x + w, y + h, color.r, color.g, color.b, alpha,
        x, y + h, color.r, color.g, color.b, alpha,
    };
    vertices.insert(vertices.end(), std::begin(quad), std::end(quad));
}

void AppendUiText(std::vector<float> &vertices, std::string text, float x, float y, float scale,
                  const Vec3 &color, size_t maxChars = 64) {
    if (text.size() > maxChars) text = text.substr(0, maxChars);
    for (char c : text) {
        Glyph3x5 glyph = GetGlyph(c);
        for (int row = 0; row < 5; ++row) {
            for (int col = 0; col < 3; ++col) {
                if ((glyph.rows[row] & (1u << (2 - col))) == 0) continue;
                AppendUiRect(vertices, x + col * scale, y + row * scale, scale, scale, color);
            }
        }
        x += scale * 4.0f;
    }
}

void AddUiButton(std::vector<float> &vertices, float x, float y, float w, float h,
                 const std::string &label, UiAction action, int value = 0, bool selected = false) {
    Vec3 fill = selected ? Vec3{0.30f, 0.48f, 0.68f} : Vec3{0.18f, 0.20f, 0.24f};
    AppendUiRect(vertices, x, y, w, h, fill);
    AppendUiRect(vertices, x, y + h - 1.0f, w, 1.0f, {0.38f, 0.41f, 0.47f});
    AppendUiText(vertices, label, x + 6.0f, y + 7.0f, 1.5f, {0.92f, 0.93f, 0.95f},
                 static_cast<size_t>(std::max(1.0f, (w - 10.0f) / 6.0f)));
    gUiHits.push_back({x, y, w, h, action, value});
}

void RebuildGuiMesh(GLuint vbo, GLsizei &outVertexCount) {
    std::vector<float> vertices;
    gUiHits.clear();
    double cursorX = 0.0, cursorY = 0.0;
    glfwGetCursorPos(gWindow, &cursorX, &cursorY);
    if (gModalType == ModalType::None && cursorX >= kUiWidth) {
        gLastCanvasWorldX = gCameraX + cursorX / gZoom;
        gLastCanvasWorldY = gCameraY + cursorY / gZoom;
    }
    const Vec3 panel{0.075f, 0.085f, 0.105f};
    const Vec3 heading{0.62f, 0.68f, 0.76f};
    AppendUiRect(vertices, 0.0f, 0.0f, kUiWidth, static_cast<float>(gWindowHeight), panel, 0.98f);
    AppendUiRect(vertices, kUiWidth - 2.0f, 0.0f, 2.0f, static_cast<float>(gWindowHeight),
                 {0.28f, 0.34f, 0.42f});
    AppendUiText(vertices, "MAP DRAWER", 12.0f, 12.0f, 2.5f, {0.90f, 0.76f, 0.35f});
    AddUiButton(vertices, kUiWidth - 69.0f, 8.0f, 59.0f, 24.0f, "HELP", UiAction::Help);

    auto headingText = [&](const char *text, float y) { AppendUiText(vertices, text, 10.0f, y, 1.5f, heading); };
    constexpr float x0 = 10.0f, h = 24.0f;

    headingText("PAINT LAYER", 40.0f);
    AddUiButton(vertices, x0, 54.0f, 57.0f, h, "LAND", UiAction::SetMode,
                static_cast<int>(PaintMode::Terrain), gPaintMode == PaintMode::Terrain);
    AddUiButton(vertices, x0 + 62.0f, 54.0f, 57.0f, h, "REGION", UiAction::SetMode,
                static_cast<int>(PaintMode::Region), gPaintMode == PaintMode::Region);
    AddUiButton(vertices, x0 + 124.0f, 54.0f, 57.0f, h, "HEIGHT", UiAction::SetMode,
                static_cast<int>(PaintMode::Elevation), gPaintMode == PaintMode::Elevation);
    AddUiButton(vertices, x0 + 186.0f, 54.0f, 58.0f, h, "FOG", UiAction::SetMode,
                static_cast<int>(PaintMode::Fog), gPaintMode == PaintMode::Fog);

    headingText("TOOLS", 88.0f);
    const char *toolLabels[] = {"BRUSH", "FILL", "LINE", "CURVE", "POLYGON", "CIRCLE",
                                "SCATTER", "RIVER", "TRADE", "SELECT", "MEASURE"};
    for (int i = 0; i < 11; ++i) {
        float x = x0 + (i % 3) * 83.0f;
        float y = 102.0f + (i / 3) * 28.0f;
        AddUiButton(vertices, x, y, 78.0f, h, toolLabels[i], UiAction::SetTool, i,
                    static_cast<int>(gToolMode) == i);
    }

    headingText(gPaintMode == PaintMode::Elevation ? "ELEVATION LEVELS" : "TERRAIN", 218.0f);
    if (gPaintMode == PaintMode::Elevation) {
        for (int elevation = kMinElevation; elevation <= kMaxElevation; ++elevation) {
            int index = elevation - kMinElevation;
            float x = x0 + (index % 7) * 35.0f;
            float y = 232.0f + (index / 7) * 28.0f;
            std::string label = elevation > 0 ? "+" + std::to_string(elevation) : std::to_string(elevation);
            AddUiButton(vertices, x, y, 31.0f, h, label, UiAction::SetElevationValue, elevation,
                        gElevationBrush == elevation);
            Vec3 band = ElevationBandColor(elevation);
            AppendUiRect(vertices, x + 3.0f, y + h - 5.0f, 25.0f, 3.0f, band);
        }
        const char *editLabels[] = {"SET", "RAISE", "LOWER", "FLAT", "AVG"};
        for (int i = 0; i < 5; ++i)
            AddUiButton(vertices, x0 + i * 50.0f, 288.0f, i == 4 ? 44.0f : 45.0f, h,
                        editLabels[i], UiAction::SetElevationTool, i,
                        static_cast<int>(gElevationEditMode) == i);
        AddUiButton(vertices, x0, 316.0f, 78.0f, h, "RELIEF", UiAction::GenerateRelief);
        AppendUiText(vertices, std::to_string(gMetresPerElevationLevel) + " M / LEVEL",
                     x0 + 90.0f, 323.0f, 1.4f, {0.72f, 0.77f, 0.84f}, 24);
    } else {
        for (int i = 0; i < kTerrainCount; ++i) {
            float x = x0 + (i % 2) * 124.0f;
            float y = 232.0f + (i / 2) * 28.0f;
            AddUiButton(vertices, x, y, 119.0f, h, kTerrainNames[i], UiAction::SetTerrain, i,
                        gBrush == i);
            AppendUiRect(vertices, x + 105.0f, y + 7.0f, 8.0f, 10.0f, kTerrainColors[i]);
        }
    }

    headingText("BRUSH", 350.0f);
    AddUiButton(vertices, x0, 364.0f, 45.0f, h, "-", UiAction::BrushDown);
    AddUiButton(vertices, x0 + 50.0f, 364.0f, 94.0f, h,
                "SIZE " + std::to_string(gBrushRadius * 2 + 1), UiAction::None);
    AddUiButton(vertices, x0 + 149.0f, 364.0f, 45.0f, h, "+", UiAction::BrushUp);
    AddUiButton(vertices, x0 + 199.0f, 364.0f, 45.0f, h,
                gRoundBrush ? "ROUND" : "BLOCK", UiAction::ToggleShape, 0, gRoundBrush);
    if (gPaintMode == PaintMode::Fog) {
        AddUiButton(vertices, x0, 392.0f, 119.0f, h, "HIDE ALL", UiAction::FogHideAll);
        AddUiButton(vertices, x0 + 124.0f, 392.0f, 120.0f, h, "REVEAL ALL", UiAction::FogRevealAll);
    } else if (gPaintMode == PaintMode::Elevation) {
        AddUiButton(vertices, x0, 392.0f, 78.0f, h, "ELEV VIEW", UiAction::ToggleElevationView, 0,
                    gElevationView);
        AddUiButton(vertices, x0 + 83.0f, 392.0f, 78.0f, h, "CONTOURS", UiAction::ToggleContours, 0,
                    gShowElevationContours);
        AddUiButton(vertices, x0 + 166.0f, 392.0f, 78.0f, h, "SHADE", UiAction::ToggleHillshade, 0,
                    gShowHillshade);
    } else {
        std::string elevationLabel = "ELEVATION " + std::to_string(gElevationBrush);
        AddUiButton(vertices, x0, 392.0f, 45.0f, h, "-", UiAction::ElevationDown);
        AddUiButton(vertices, x0 + 50.0f, 392.0f, 144.0f, h, elevationLabel, UiAction::SetMode,
                    static_cast<int>(PaintMode::Elevation), gPaintMode == PaintMode::Elevation);
        AddUiButton(vertices, x0 + 199.0f, 392.0f, 45.0f, h, "+", UiAction::ElevationUp);
    }

    headingText("WORLD", 426.0f);
    AddUiButton(vertices, x0, 440.0f, 78.0f, h, "REGION", UiAction::NewRegion);
    AddUiButton(vertices, x0 + 83.0f, 440.0f, 78.0f, h, "CITY", UiAction::NewCity, 0,
                gPlacementMode == PlacementMode::City);
    AddUiButton(vertices, x0 + 166.0f, 440.0f, 78.0f, h, "POI", UiAction::NewPoi, 0,
                gPlacementMode == PlacementMode::Poi);
    AddUiButton(vertices, x0, 468.0f, 78.0f, h, "CYCLE", UiAction::CycleRegion);
    AddUiButton(vertices, x0 + 83.0f, 468.0f, 78.0f, h, "DEL MARK", UiAction::DeleteMarker);
    AddUiButton(vertices, x0 + 166.0f, 468.0f, 78.0f, h, "DEL ROUTE", UiAction::DeleteRoute);
    AddUiButton(vertices, x0, 496.0f, 78.0f, h, "INFO", UiAction::WorldInfo);
    AddUiButton(vertices, x0 + 83.0f, 496.0f, 78.0f, h, "FIND", UiAction::Find);
    AddUiButton(vertices, x0 + 166.0f, 496.0f, 78.0f, h, "ENCOUNTER", UiAction::NewEncounter,
                0, gPlacementMode == PlacementMode::Encounter);

    headingText("PROJECT", 530.0f);
    std::string projectLabel = gProjectFile;
    if (projectLabel.size() > 24) projectLabel = projectLabel.substr(0, 21) + "...";
    AppendUiText(vertices, projectLabel, x0 + 78.0f, 532.0f, 1.2f, {0.62f, 0.67f, 0.74f}, 24);
    AddUiButton(vertices, x0, 544.0f, 57.0f, h, "SAVE", UiAction::Save);
    AddUiButton(vertices, x0 + 62.0f, 544.0f, 57.0f, h, "LOAD", UiAction::Load);
    AddUiButton(vertices, x0 + 124.0f, 544.0f, 57.0f, h, "UNDO", UiAction::Undo);
    AddUiButton(vertices, x0 + 186.0f, 544.0f, 58.0f, h, "REDO", UiAction::Redo);
    AddUiButton(vertices, x0, 572.0f, 78.0f, h, "CLEAR", UiAction::Clear);
    AddUiButton(vertices, x0 + 83.0f, 572.0f, 78.0f, h, "EXPORT", UiAction::Export);
    AddUiButton(vertices, x0 + 166.0f, 572.0f, 78.0f, h, "NAME", UiAction::ProjectName);

    headingText("VIEW", 606.0f);
    AddUiButton(vertices, x0, 620.0f, 78.0f, h, "GRID", UiAction::ToggleGrid, 0, gShowGrid);
    AddUiButton(vertices, x0 + 83.0f, 620.0f, 78.0f, h, gHexGrid ? "HEX" : "SQUARE",
                UiAction::ToggleGeometry, 0, gHexGrid);
    AddUiButton(vertices, x0 + 166.0f, 620.0f, 78.0f, h, "REGIONS", UiAction::ToggleRegions, 0,
                gShowRegions);
    AddUiButton(vertices, x0, 648.0f, 57.0f, h, "LABELS", UiAction::ToggleLabels, 0, gShowLabels);
    AddUiButton(vertices, x0 + 62.0f, 648.0f, 57.0f, h, "PLAYER", UiAction::TogglePlayerView, 0,
                gPlayerView);
    AddUiButton(vertices, x0 + 124.0f, 648.0f, 57.0f, h, "RESET", UiAction::ResetCamera);
    AddUiButton(vertices, x0 + 186.0f, 648.0f, 58.0f, h, "FIT", UiAction::FitMap);

    auto [cursorCol, cursorRow] = WorldToTile(gLastCanvasWorldX, gLastCanvasWorldY);
    int cursorElevation = GetLayerValue(PaintMode::Elevation, TileKey(cursorCol, cursorRow));
    int cursorMetres = (cursorElevation - gSeaLevel) * gMetresPerElevationLevel;
    std::string coordinateText = "TILE " + std::to_string(cursorCol) + "  " + std::to_string(cursorRow) +
                                 "   HEIGHT " + (cursorMetres >= 0 ? "+" : "") +
                                 std::to_string(cursorMetres) + " M";
    float badgeWidth = 330.0f;
    float badgeX = static_cast<float>(gWindowWidth) - badgeWidth - 12.0f;
    float badgeY = gSelectionActive ? 84.0f : 10.0f;
    AppendUiRect(vertices, badgeX, badgeY, badgeWidth, 30.0f, {0.07f, 0.09f, 0.12f}, 0.94f);
    AppendUiText(vertices, coordinateText, badgeX + 10.0f, badgeY + 10.0f, 1.7f,
                 {0.92f, 0.82f, 0.43f}, 27);
    gUiHits.push_back({badgeX, badgeY, badgeWidth, 30.0f, UiAction::None, 0});
    if (gMeasureStage != 0) {
        double tiles = MeasuredTileDistance();
        double kilometres = tiles * kKilometresPerTile;
        double days = kilometres / kWalkingKilometresPerDay;
        char measureText[128];
        std::snprintf(measureText, sizeof(measureText), "%.1f TILES  %.0f KM  %.1f DAYS", tiles,
                      kilometres, days);
        float measureWidth = 300.0f;
        float measureX = static_cast<float>(gWindowWidth) - measureWidth - 12.0f;
        float measureY = badgeY + 38.0f;
        AppendUiRect(vertices, measureX, measureY, measureWidth, 30.0f, {0.07f, 0.09f, 0.12f}, 0.94f);
        AppendUiText(vertices, measureText, measureX + 10.0f, measureY + 10.0f, 1.5f,
                     {0.35f, 0.85f, 0.95f}, 40);
        gUiHits.push_back({measureX, measureY, measureWidth, 30.0f, UiAction::None, 0});
    }
    if (!gLastFoundLabel.empty()) {
        std::string shown = gLastFoundLabel;
        if (shown.size() > 44) shown = shown.substr(0, 41) + "...";
        float foundWidth = 350.0f;
        float foundX = static_cast<float>(gWindowWidth) - foundWidth - 12.0f;
        float foundY = badgeY + (gMeasureStage != 0 ? 76.0f : 38.0f);
        AppendUiRect(vertices, foundX, foundY, foundWidth, 30.0f, {0.07f, 0.09f, 0.12f}, 0.94f);
        AppendUiText(vertices, shown, foundX + 10.0f, foundY + 10.0f, 1.5f,
                     {0.55f, 0.90f, 0.60f}, 46);
        gUiHits.push_back({foundX, foundY, foundWidth, 30.0f, UiAction::None, 0});
    }
    AppendUiText(vertices, "STATUS", 10.0f, 686.0f, 1.5f, heading);
    AppendUiText(vertices, std::string("TOOL ") + ToolName(gToolMode), 10.0f, 704.0f, 1.5f,
                 {0.85f, 0.87f, 0.90f}, 35);
    AppendUiText(vertices, std::string("LAYER ") + PaintModeName(gPaintMode), 10.0f, 720.0f, 1.5f,
                 {0.85f, 0.87f, 0.90f}, 35);
    AppendUiText(vertices, "ZOOM " + std::to_string(static_cast<int>(gZoom * 100.0)) + " PERCENT",
                 10.0f, 736.0f, 1.5f, {0.85f, 0.87f, 0.90f}, 35);
    AppendUiText(vertices, "REGIONS " + std::to_string(gRegions.size()) + " CITIES " +
                               std::to_string(gCities.size()),
                 10.0f, 752.0f, 1.5f, {0.70f, 0.74f, 0.80f}, 35);
    AppendUiText(vertices, "POI " + std::to_string(gPois.size()) + " ENC " +
                               std::to_string(gEncounters.size()) + " ROUTES " +
                               std::to_string(gRoutes.size()),
                 10.0f, 768.0f, 1.5f, {0.70f, 0.74f, 0.80f}, 35);
    if (gActiveRegionId != 0 && gRegions.count(gActiveRegionId))
        AppendUiText(vertices, "ACTIVE " + gRegions[gActiveRegionId].name, 10.0f, 784.0f, 1.5f,
                     {0.80f, 0.70f, 0.45f}, 35);
    AppendUiText(vertices, "TERRAIN TILES " + std::to_string(gMapData.size()), 10.0f, 800.0f, 1.5f,
                 {0.62f, 0.67f, 0.74f}, 35);
    AppendUiText(vertices, "HEIGHT TILES " + std::to_string(gElevationData.size()), 10.0f, 816.0f, 1.5f,
                 {0.62f, 0.67f, 0.74f}, 35);
    AppendUiText(vertices, "FOGGED TILES " + std::to_string(gFogData.size()), 10.0f, 832.0f, 1.5f,
                 {0.62f, 0.67f, 0.74f}, 35);
    AppendUiText(vertices, gProjectDirty ? "UNSAVED CHANGES" : "ALL CHANGES SAVED",
                 10.0f, 848.0f, 1.5f,
                 gProjectDirty ? Vec3{0.95f, 0.58f, 0.30f} : Vec3{0.45f, 0.78f, 0.52f}, 35);
    if (gPlacementMode != PlacementMode::None)
        AppendUiText(vertices, gPlacementMode == PlacementMode::City
                                   ? "CLICK MAP FOR CITY"
                                   : (gPlacementMode == PlacementMode::Poi ? "CLICK MAP FOR POI"
                                                                           : "CLICK MAP FOR ENCOUNTER"),
                     10.0f, 864.0f, 1.5f, {0.95f, 0.65f, 0.30f}, 35);

    if (gSelectionActive) {
        float barX = kUiWidth + 12.0f, barY = 10.0f;
        AppendUiRect(vertices, barX - 6.0f, barY - 5.0f, 620.0f, 68.0f, {0.08f, 0.10f, 0.13f}, 0.96f);
        AppendUiText(vertices, "SELECTION " + std::to_string(SelectionTileCount()) + " TILES",
                     barX, barY + 3.0f, 1.5f, {0.95f, 0.80f, 0.35f}, 34);
        float by = barY + 25.0f;
        const char *labels[] = {"COPY", "CUT", "PASTE", "DELETE", "PAINT", "REGION", "H-", "H+", "CLOSE"};
        const UiAction actions[] = {UiAction::SelectionCopy, UiAction::SelectionCut,
            UiAction::SelectionPaste, UiAction::SelectionDelete, UiAction::SelectionPaint,
            UiAction::SelectionRegion, UiAction::SelectionElevationDown,
            UiAction::SelectionElevationUp, UiAction::SelectionClear};
        for (int i = 0; i < 9; ++i)
            AddUiButton(vertices, barX + i * 67.0f, by, 62.0f, 28.0f, labels[i], actions[i]);
    }

    if (gModalType != ModalType::None) {
        gUiHits.clear();
        AppendUiRect(vertices, 0.0f, 0.0f, static_cast<float>(gWindowWidth),
                     static_cast<float>(gWindowHeight), {0.01f, 0.01f, 0.02f}, 0.72f);
        const bool keybindHelp =
            gModalType == ModalType::Info && gInfoTitle == "KEYBOARD & MOUSE HELP";
        float mw = keybindHelp ? std::min(760.0f, static_cast<float>(gWindowWidth) - 60.0f)
                               : 560.0f;
        float mh = keybindHelp
                       ? std::min(760.0f, static_cast<float>(gWindowHeight) - 60.0f)
                       : ((gModalType == ModalType::Info || gModalType == ModalType::Confirm)
                              ? 320.0f
                              : (gModalFields.size() > 1 ? 290.0f : 220.0f));
        float mx = (gWindowWidth - mw) * 0.5f, my = (gWindowHeight - mh) * 0.5f;
        AppendUiRect(vertices, mx, my, mw, mh, {0.10f, 0.12f, 0.16f});
        AppendUiRect(vertices, mx, my, mw, 4.0f, {0.75f, 0.57f, 0.20f});
        const char *title = gModalType == ModalType::Region ? "NEW REGION" :
                            gModalType == ModalType::City ? "NEW CITY" :
                            gModalType == ModalType::Poi ? "NEW POINT OF INTEREST" :
                            gModalType == ModalType::Encounter
                                ? (gEditingEncounterIndex >= 0 ? "EDIT ENCOUNTER" : "NEW ENCOUNTER") :
                            gModalType == ModalType::Info ? gInfoTitle.c_str() :
                            gModalType == ModalType::Confirm ? gInfoTitle.c_str() :
                            gModalType == ModalType::ProjectName ? "PROJECT FILE" :
                            gModalType == ModalType::Search ? "FIND ON MAP" : "NAME ROUTE";
        AppendUiText(vertices, title, mx + 24.0f, my + 24.0f, 2.5f, {0.95f, 0.82f, 0.42f});
        if (gModalType == ModalType::City || gModalType == ModalType::Poi ||
            gModalType == ModalType::Encounter)
            AppendUiText(vertices, "TILE " + std::to_string(gModalCol) + " " + std::to_string(gModalRow),
                         mx + 350.0f, my + 29.0f, 1.5f, {0.72f, 0.77f, 0.84f}, 24);
        if (gModalType == ModalType::Info) {
            const float lineSpacing = keybindHelp ? 22.0f : 34.0f;
            for (size_t i = 0; i < gInfoLines.size(); ++i) {
                const bool sectionHeading =
                    keybindHelp && (gInfoLines[i] == "NAVIGATION" || gInfoLines[i] == "PAINTING" ||
                                    gInfoLines[i] == "TOOLS" ||
                                    gInfoLines[i] == "WORLD AND PROJECT");
                const float textScale = keybindHelp ? (sectionHeading ? 1.65f : 1.45f) : 1.8f;
                const Vec3 textColor = sectionHeading ? Vec3{0.90f, 0.76f, 0.35f}
                                                      : Vec3{0.88f, 0.90f, 0.94f};
                AppendUiText(vertices, gInfoLines[i], mx + 24.0f,
                             my + 72.0f + static_cast<float>(i) * lineSpacing, textScale,
                             textColor, keybindHelp ? 88 : 62);
            }
            AddUiButton(vertices, mx + mw - 112.0f, my + mh - 48.0f, 88.0f, 28.0f,
                        "CLOSE", UiAction::ModalCancel);
        } else if (gModalType == ModalType::Confirm) {
            for (size_t i = 0; i < gInfoLines.size(); ++i)
                AppendUiText(vertices, gInfoLines[i], mx + 24.0f,
                             my + 78.0f + static_cast<float>(i) * 36.0f,
                             1.8f, {0.88f, 0.90f, 0.94f}, 62);
            AddUiButton(vertices, mx + mw - 210.0f, my + mh - 48.0f, 86.0f, 28.0f,
                        "CANCEL", UiAction::ModalCancel);
            AddUiButton(vertices, mx + mw - 112.0f, my + mh - 48.0f, 88.0f, 28.0f,
                        "CONFIRM", UiAction::ModalAccept);
        } else {
            const char *fieldLabels[2] = {"NAME", "DETAILS"};
            if (gModalType == ModalType::Search) fieldLabels[0] = "SEARCH";
            if (gModalType == ModalType::Region || gModalType == ModalType::City) fieldLabels[1] = "RULER";
            if (gModalType == ModalType::Poi) {
                for (int i = 0; i < kPoiKindCount; ++i) {
                    float kindX = mx + 24.0f + i * 103.0f;
                    AddUiButton(vertices, kindX, my + 58.0f, 99.0f, 28.0f,
                                PoiKindName(static_cast<PoiKind>(i)), UiAction::ModalPoiKind, i,
                                static_cast<int>(gModalPoiKind) == i);
                }
            }
            float fieldsY = my + (gModalType == ModalType::Poi ? 98.0f : 62.0f);
            for (size_t i = 0; i < gModalFields.size(); ++i) {
                float fy = fieldsY + static_cast<float>(i) * 62.0f;
                AppendUiText(vertices, fieldLabels[i], mx + 24.0f, fy, 1.5f, heading);
                Vec3 fieldColor = static_cast<int>(i) == gModalField ? Vec3{0.20f, 0.28f, 0.38f}
                                                                      : Vec3{0.14f, 0.16f, 0.20f};
                AppendUiRect(vertices, mx + 24.0f, fy + 15.0f, mw - 48.0f, 32.0f, fieldColor);
                std::string shown = gModalFields[i];
                if (static_cast<int>(i) == gModalField) shown += "_";
                if (shown.size() > 68) shown = "< " + shown.substr(shown.size() - 66);
                AppendUiText(vertices, shown, mx + 32.0f, fy + 25.0f, 1.7f,
                             {0.95f, 0.95f, 0.96f}, 68);
                gUiHits.push_back({mx + 24.0f, fy + 15.0f, mw - 48.0f, 32.0f,
                                   i == 0 ? UiAction::ModalPrevious : UiAction::ModalNext,
                                   static_cast<int>(i)});
            }
            float buttonY = my + mh - 48.0f;
            AddUiButton(vertices, mx + mw - 210.0f, buttonY, 86.0f, 28.0f, "CANCEL", UiAction::ModalCancel);
            AddUiButton(vertices, mx + mw - 112.0f, buttonY, 88.0f, 28.0f,
                        gModalType == ModalType::ProjectName ? "APPLY" :
                        gModalType == ModalType::Search ? "FIND" :
                        (gModalType == ModalType::Encounter && gEditingEncounterIndex >= 0)
                            ? "SAVE" : "CREATE",
                        UiAction::ModalAccept);
            AppendUiText(vertices, "ENTER NEXT   ESC CANCEL", mx + 24.0f, buttonY + 9.0f, 1.3f,
                         {0.60f, 0.64f, 0.70f});
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                 vertices.data(), GL_DYNAMIC_DRAW);
    outVertexCount = static_cast<GLsizei>(vertices.size() / 6);
}

void HandleUiAction(const UiHit &hit) {
    gUseUiTarget = true;
    switch (hit.action) {
        case UiAction::None: break;
        case UiAction::SetMode: gPaintMode = static_cast<PaintMode>(hit.value); break;
        case UiAction::SetTool: SelectTool(static_cast<ToolMode>(hit.value)); break;
        case UiAction::SetTerrain: gBrush = hit.value; gPaintMode = PaintMode::Terrain; break;
        case UiAction::SetElevationValue:
            gElevationBrush = std::clamp(hit.value, kMinElevation, kMaxElevation);
            gElevationEditMode = ElevationEditMode::Set;
            gPaintMode = PaintMode::Elevation;
            break;
        case UiAction::SetElevationTool:
            gElevationEditMode = static_cast<ElevationEditMode>(std::clamp(hit.value, 0, 4));
            gPaintMode = PaintMode::Elevation;
            break;
        case UiAction::ElevationDown:
            gElevationBrush = std::max(kMinElevation, gElevationBrush - 1); gPaintMode = PaintMode::Elevation; break;
        case UiAction::ElevationUp:
            gElevationBrush = std::min(kMaxElevation, gElevationBrush + 1); gPaintMode = PaintMode::Elevation; break;
        case UiAction::BrushDown: gBrushRadius = std::max(0, gBrushRadius - 1); break;
        case UiAction::BrushUp: gBrushRadius = std::min(kMaxBrushRadius, gBrushRadius + 1); break;
        case UiAction::ToggleShape: gRoundBrush = !gRoundBrush; break;
        case UiAction::NewRegion: CreateRegion(); break;
        case UiAction::CycleRegion: CycleActiveRegion(); break;
        case UiAction::NewCity:
            gPlacementMode = gPlacementMode == PlacementMode::City ? PlacementMode::None : PlacementMode::City;
            LOG_INFO("City placement: %s", gPlacementMode == PlacementMode::City ? "click a map tile" : "cancelled");
            break;
        case UiAction::NewPoi:
            gPlacementMode = gPlacementMode == PlacementMode::Poi ? PlacementMode::None : PlacementMode::Poi;
            LOG_INFO("POI placement: %s", gPlacementMode == PlacementMode::Poi ? "click a map tile" : "cancelled");
            break;
        case UiAction::NewEncounter: ToggleEncounterPlacement(); break;
        case UiAction::DeleteMarker: RemoveMarkerAtCursor(); break;
        case UiAction::DeleteRoute: RemoveRouteAtCursor(); break;
        case UiAction::WorldInfo: PrintWorldInfo(); break;
        case UiAction::Help: OpenKeybindHelp(); break;
        case UiAction::Find: OpenModal(ModalType::Search, {gLastSearchQuery}); break;
        case UiAction::Undo: Undo(); break;
        case UiAction::Redo: Redo(); break;
        case UiAction::Save: RequestProjectSave(); break;
        case UiAction::Load: RequestProjectLoad(); break;
        case UiAction::Clear: RequestClearActiveLayer(); break;
        case UiAction::Export: ExportScreenshot(); break;
        case UiAction::ToggleGrid: gShowGrid = !gShowGrid; break;
        case UiAction::ToggleGeometry: gHexGrid = !gHexGrid; ++gSceneRevision; MarkProjectDirty(); break;
        case UiAction::ToggleRegions: gShowRegions = !gShowRegions; break;
        case UiAction::ToggleLabels: gShowLabels = !gShowLabels; break;
        case UiAction::ToggleElevationView:
            gElevationView = !gElevationView; ++gSceneRevision; MarkProjectDirty(); break;
        case UiAction::ToggleContours:
            gShowElevationContours = !gShowElevationContours; ++gSceneRevision; MarkProjectDirty(); break;
        case UiAction::ToggleHillshade:
            gShowHillshade = !gShowHillshade; ++gSceneRevision; MarkProjectDirty(); break;
        case UiAction::GenerateRelief:
            OpenConfirmation(ConfirmAction::GenerateRelief, "GENERATE RELIEF",
                             {"DERIVE HEIGHTS FROM ALL TERRAIN TILES?",
                              "EXISTING HEIGHTS ON THOSE TILES WILL CHANGE"});
            break;
        case UiAction::TogglePlayerView:
            gPlayerView = !gPlayerView; ++gSceneRevision;
            LOG_INFO("Player view: %s", gPlayerView ? "on" : "off"); break;
        case UiAction::FogHideAll: HideAllTerrainWithFog(); break;
        case UiAction::FogRevealAll:
            OpenConfirmation(ConfirmAction::ClearLayer, "REVEAL ENTIRE MAP",
                             {"REMOVE ALL FOG OF WAR?", "THIS ACTION CAN BE UNDONE FROM A BACKUP"});
            break;
        case UiAction::ResetCamera: gCameraX = -kUiWidth; gCameraY = 0.0; gZoom = 1.0; break;
        case UiAction::FitMap: FitMapToWindow(); break;
        case UiAction::ProjectName: {
            std::filesystem::path projectPath(gProjectFile);
            OpenModal(ModalType::ProjectName, {projectPath.stem().string()});
            break;
        }
        case UiAction::SelectionCopy: CopySelection(false); break;
        case UiAction::SelectionCut: CopySelection(true); break;
        case UiAction::SelectionPaste: PasteSelection(); break;
        case UiAction::SelectionDelete: ClearSelectionLayer(gPaintMode); break;
        case UiAction::SelectionPaint: PaintSelection(); break;
        case UiAction::SelectionRegion:
            if (gActiveRegionId == 0) LOG_WARN("No active region to assign");
            else TransformSelection(PaintMode::Region, gActiveRegionId,
                                    [](int /*oldValue*/) { return gActiveRegionId; });
            break;
        case UiAction::SelectionElevationDown: AdjustSelectionElevation(-1); break;
        case UiAction::SelectionElevationUp: AdjustSelectionElevation(1); break;
        case UiAction::SelectionClear: gSelectionActive = false; gSelectionDragging = false; break;
        case UiAction::ModalPrevious:
        case UiAction::ModalNext: gModalField = hit.value; break;
        case UiAction::ModalAccept: CloseModal(true); break;
        case UiAction::ModalCancel: CloseModal(false); break;
        case UiAction::ModalPoiKind:
            gModalPoiKind = static_cast<PoiKind>(std::clamp(hit.value, 0, kPoiKindCount - 1)); break;
    }
    gUseUiTarget = false;
    UpdateWindowTitle();
}

bool HandleGuiPress() {
    double mouseX = 0.0, mouseY = 0.0;
    glfwGetCursorPos(gWindow, &mouseX, &mouseY);
    for (auto it = gUiHits.rbegin(); it != gUiHits.rend(); ++it) {
        if (mouseX >= it->x && mouseX <= it->x + it->w && mouseY >= it->y && mouseY <= it->y + it->h) {
            HandleUiAction(*it);
            return true;
        }
    }
    return gModalType != ModalType::None || mouseX < kUiWidth;
}

bool CursorOverGui() {
    double mouseX = 0.0, mouseY = 0.0;
    glfwGetCursorPos(gWindow, &mouseX, &mouseY);
    if (gModalType != ModalType::None || mouseX < kUiWidth) return true;
    for (const auto &hit : gUiHits)
        if (mouseX >= hit.x && mouseX <= hit.x + hit.w && mouseY >= hit.y && mouseY <= hit.y + hit.h)
            return true;
    return false;
}

void CharacterCallback(GLFWwindow * /*window*/, unsigned int codepoint) {
    if (gModalType == ModalType::None || gModalFields.empty()) return;
    const size_t limit = gModalType == ModalType::Encounter && gModalField == 1 ? 240 : 80;
    if (codepoint >= 32 && codepoint <= 126 && gModalFields[gModalField].size() < limit)
        gModalFields[gModalField].push_back(static_cast<char>(codepoint));
}

void MouseButtonCallback(GLFWwindow * /*window*/, int button, int action, int mods) {
    if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
        if (action == GLFW_PRESS && !CursorOverGui()) {
            gMiddlePanning = true;
            glfwGetCursorPos(gWindow, &gPanLastX, &gPanLastY);
        } else if (action == GLFW_RELEASE) {
            gMiddlePanning = false;
        }
        return;
    }
    if (action == GLFW_PRESS && button == GLFW_MOUSE_BUTTON_LEFT && HandleGuiPress()) return;
    if (action == GLFW_PRESS && button == GLFW_MOUSE_BUTTON_RIGHT && CursorOverGui()) return;
    if (action == GLFW_RELEASE && CursorOverGui()) {
        gLineDragging = false;
        gCircleDragging = false;
        gRectDragging = false;
        if (gSelectionDragging) gSelectionActive = true;
        gSelectionDragging = false;
        if (gPaintingLeft || gPaintingRight) EndStroke();
        gPaintingLeft = false;
        gPaintingRight = false;
        return;
    }
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (action == GLFW_PRESS) {
            if (gPlacementMode == PlacementMode::City) {
                gPlacementMode = PlacementMode::None;
                PlaceCityAtCursor();
            } else if (gPlacementMode == PlacementMode::Poi) {
                gPlacementMode = PlacementMode::None;
                PlacePoiAtCursor();
            } else if (gPlacementMode == PlacementMode::Encounter) {
                gPlacementMode = PlacementMode::None;
                PlaceEncounterAtCursor();
            } else if (gToolMode == ToolMode::Measure) {
                auto [col, row] = CursorTile();
                if (gMeasureStage == 1) {
                    gMeasureEndCol = col;
                    gMeasureEndRow = row;
                    gMeasureStage = 2;
                    LOG_INFO("Measured %.1f tiles (%.0f km, %.1f walking days)", MeasuredTileDistance(),
                             MeasuredTileDistance() * kKilometresPerTile,
                             MeasuredTileDistance() * kKilometresPerTile / kWalkingKilometresPerDay);
                } else {
                    gMeasureStartCol = gMeasureEndCol = col;
                    gMeasureStartRow = gMeasureEndRow = row;
                    gMeasureStage = 1;
                }
            } else if (ShowMarkerInfoAtCursor()) {
                // Marker clicks open their read-only information card instead of painting through them.
            } else if (gToolMode == ToolMode::Selection) {
                gSelectionDragging = true;
                gSelectionActive = false;
                std::tie(gSelectionStartCol, gSelectionStartRow) = CursorTile();
                gSelectionEndCol = gSelectionStartCol;
                gSelectionEndRow = gSelectionStartRow;
            } else if (gToolMode == ToolMode::FloodFill) {
                auto [col, row] = CursorTile();
                FloodFill(col, row, ActivePaintValue());
            } else if (gToolMode == ToolMode::Line) {
                gLineDragging = true;
                gLineErase = false;
                std::tie(gLineStartWX, gLineStartWY) = CursorWorld();
            } else if (gToolMode == ToolMode::Circle) {
                gCircleDragging = true;
                gCircleErase = false;
                std::tie(gCircleCenterWX, gCircleCenterWY) = CursorWorld();
            } else if (gToolMode == ToolMode::Curve) {
                auto [wx, wy] = CursorWorld();
                if (gCurveStage == 0) {
                    gCurveP0X = wx; gCurveP0Y = wy;
                    gCurveStage = 1;
                } else if (gCurveStage == 1) {
                    gCurveP1X = wx; gCurveP1Y = wy;
                    gCurveStage = 2;
                } else {
                    RasterizeCurve(gCurveP0X, gCurveP0Y, gCurveP1X, gCurveP1Y, wx, wy, ActivePaintValue());
                    gCurveStage = 0;
                }
            } else if (gToolMode == ToolMode::Polygon) {
                gPolygonPoints.push_back(CursorWorld());
            } else if (gToolMode == ToolMode::River || gToolMode == ToolMode::TradeRoute) {
                gRoutePoints.push_back(CursorWorld());
            } else if (gToolMode == ToolMode::Scatter) {
                gPaintingLeft = true;
                BeginStroke();
                auto [wx, wy] = CursorWorld();
                ScatterAt(wx, wy, ActivePaintValue());
                if (gStrokeChanged) ++gSceneRevision;
            } else if (mods & GLFW_MOD_SHIFT) {
                gRectDragging = true;
                gRectErase = false;
                std::tie(gRectStartCol, gRectStartRow) = CursorTile();
                gRectEndCol = gRectStartCol;
                gRectEndRow = gRectStartRow;
            } else {
                gPaintingLeft = true;
                BeginStroke();
                PaintAtCursor(ActivePaintValue());
                if (gStrokeChanged) ++gSceneRevision;
            }
        } else if (action == GLFW_RELEASE) {
            if (gToolMode == ToolMode::Selection && gSelectionDragging) {
                std::tie(gSelectionEndCol, gSelectionEndRow) = CursorTile();
                gSelectionDragging = false;
                gSelectionActive = true;
            }
            if (gToolMode == ToolMode::Line && gLineDragging) {
                auto [wx, wy] = CursorWorld();
                int value = gLineErase ? 0 : ActivePaintValue();
                RasterizeLine(gLineStartWX, gLineStartWY, wx, wy, value);
                gLineDragging = false;
            }
            if (gToolMode == ToolMode::Circle && gCircleDragging) {
                auto [wx, wy] = CursorWorld();
                double radius = std::hypot(wx - gCircleCenterWX, wy - gCircleCenterWY);
                int value = gCircleErase ? 0 : ActivePaintValue();
                FillCircle(gCircleCenterWX, gCircleCenterWY, radius, value);
                gCircleDragging = false;
            }
            if (gRectDragging && !gRectErase) {
                CommitRectFill();
                gRectDragging = false;
            }
            if (gPaintingLeft) {
                gPaintingLeft = false;
                EndStroke();
            }
        }
    } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        if (action == GLFW_PRESS) {
            if (gPlacementMode != PlacementMode::None) {
                gPlacementMode = PlacementMode::None;
                LOG_INFO("Marker placement cancelled");
            } else if (gToolMode == ToolMode::Measure) {
                gMeasureStage = 0;
            } else if (gToolMode == ToolMode::Selection) {
                gSelectionActive = false;
                gSelectionDragging = false;
            } else if (gToolMode == ToolMode::FloodFill) {
                auto [col, row] = CursorTile();
                FloodFill(col, row, 0);
            } else if (gToolMode == ToolMode::Curve) {
                gCurveStage = 0; // cancel pending curve
            } else if (gToolMode == ToolMode::Polygon) {
                FillPolygon(gPolygonPoints, ActivePaintValue());
                gPolygonPoints.clear();
            } else if (gToolMode == ToolMode::River) {
                CommitRoute(RouteKind::River);
            } else if (gToolMode == ToolMode::TradeRoute) {
                CommitRoute(RouteKind::TradeRoute);
            } else if (gToolMode == ToolMode::Line) {
                gLineDragging = true;
                gLineErase = true;
                std::tie(gLineStartWX, gLineStartWY) = CursorWorld();
            } else if (gToolMode == ToolMode::Circle) {
                gCircleDragging = true;
                gCircleErase = true;
                std::tie(gCircleCenterWX, gCircleCenterWY) = CursorWorld();
            } else if (gToolMode == ToolMode::Scatter) {
                gPaintingRight = true;
                BeginStroke();
                auto [wx, wy] = CursorWorld();
                ScatterAt(wx, wy, 0);
                if (gStrokeChanged) ++gSceneRevision;
            } else if (mods & GLFW_MOD_SHIFT) {
                gRectDragging = true;
                gRectErase = true;
                std::tie(gRectStartCol, gRectStartRow) = CursorTile();
                gRectEndCol = gRectStartCol;
                gRectEndRow = gRectStartRow;
            } else {
                gPaintingRight = true;
                BeginStroke();
                PaintAtCursor(0);
                if (gStrokeChanged) ++gSceneRevision;
            }
        } else if (action == GLFW_RELEASE) {
            if (gRectDragging && gRectErase) {
                CommitRectFill();
                gRectDragging = false;
            }
            if (gPaintingRight) {
                gPaintingRight = false;
                EndStroke();
            }
        }
    }
}

void CursorPosCallback(GLFWwindow * /*window*/, double x, double y) {
    if (gMiddlePanning) {
        gCameraX -= (x - gPanLastX) / gZoom;
        gCameraY -= (y - gPanLastY) / gZoom;
        gPanLastX = x;
        gPanLastY = y;
        gLastCanvasWorldX = gCameraX + x / gZoom;
        gLastCanvasWorldY = gCameraY + y / gZoom;
        return;
    }
    if (CursorOverGui()) return;
    gLastCanvasWorldX = gCameraX + x / gZoom;
    gLastCanvasWorldY = gCameraY + y / gZoom;
    if (gToolMode == ToolMode::Measure && gMeasureStage == 1) {
        std::tie(gMeasureEndCol, gMeasureEndRow) = WorldToTile(gLastCanvasWorldX, gLastCanvasWorldY);
        return;
    }
    if (gSelectionDragging) {
        std::tie(gSelectionEndCol, gSelectionEndRow) = CursorTile();
        return;
    }
    if (gRectDragging) {
        std::tie(gRectEndCol, gRectEndRow) = CursorTile();
        return;
    }
    if (gToolMode == ToolMode::Line || gToolMode == ToolMode::Circle) return; // preview-only until release
    if (gToolMode == ToolMode::Scatter) {
        if (gPaintingLeft) { auto [wx, wy] = CursorWorld(); ScatterAt(wx, wy, ActivePaintValue()); }
        else if (gPaintingRight) { auto [wx, wy] = CursorWorld(); ScatterAt(wx, wy, 0); }
        if (gStrokeChanged) ++gSceneRevision;
        return;
    }
    if (gPaintingLeft) PaintAtCursor(ActivePaintValue());
    else if (gPaintingRight) PaintAtCursor(0);
    if ((gPaintingLeft || gPaintingRight) && gStrokeChanged) ++gSceneRevision;
}

// Zooms in/out, keeping the world point under the cursor stationary on screen.
void ScrollCallback(GLFWwindow * /*window*/, double /*xoffset*/, double yoffset) {
    if (CursorOverGui()) return;
    double mouseX = 0.0, mouseY = 0.0;
    glfwGetCursorPos(gWindow, &mouseX, &mouseY);

    double worldX = gCameraX + mouseX / gZoom;
    double worldY = gCameraY + mouseY / gZoom;

    gZoom = std::clamp(gZoom * std::pow(1.1, yoffset), kMinZoom, kMaxZoom);

    gCameraX = worldX - mouseX / gZoom;
    gCameraY = worldY - mouseY / gZoom;
    UpdateWindowTitle();
}

void SelectTool(ToolMode mode) {
    gToolMode = mode;
    gPlacementMode = PlacementMode::None;
    gCurveStage = 0;
    gPolygonPoints.clear();
    gRoutePoints.clear();
    gLineDragging = false;
    gCircleDragging = false;
    gSelectionDragging = false;
    gMeasureStage = 0;
    LOG_INFO("Tool: %s", ToolName(mode));
    UpdateWindowTitle();
}

void KeyCallback(GLFWwindow *window, int key, int /*scancode*/, int action, int mods) {
    if (gModalType != ModalType::None) {
        if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
        if (gModalType == ModalType::Info) {
            if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER ||
                key == GLFW_KEY_SPACE)
                CloseModal(false);
            return;
        }
        if (gModalType == ModalType::Confirm) {
            if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_N) CloseModal(false);
            else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER || key == GLFW_KEY_Y)
                CloseModal(true);
            return;
        }
        if (key == GLFW_KEY_ESCAPE) CloseModal(false);
        else if (key == GLFW_KEY_BACKSPACE && !gModalFields.empty() && !gModalFields[gModalField].empty())
            gModalFields[gModalField].pop_back();
        else if (key == GLFW_KEY_TAB && !gModalFields.empty())
            gModalField = (gModalField + 1) % static_cast<int>(gModalFields.size());
        else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) {
            if (gModalField + 1 < static_cast<int>(gModalFields.size())) ++gModalField;
            else CloseModal(true);
        } else if (gModalType == ModalType::Poi && (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT)) {
            int delta = key == GLFW_KEY_LEFT ? kPoiKindCount - 1 : 1;
            gModalPoiKind = static_cast<PoiKind>((static_cast<int>(gModalPoiKind) + delta) % kPoiKindCount);
        }
        return;
    }
    if (action != GLFW_PRESS) return;

    if (key == GLFW_KEY_SLASH && (mods & GLFW_MOD_SHIFT)) {
        OpenKeybindHelp();
    } else if (key == GLFW_KEY_F && (mods & GLFW_MOD_CONTROL)) {
        OpenModal(ModalType::Search, {gLastSearchQuery});
    } else if (key == GLFW_KEY_E && (mods & GLFW_MOD_CONTROL)) {
        ToggleEncounterPlacement();
    } else if (key == GLFW_KEY_C && (mods & GLFW_MOD_CONTROL) && gSelectionActive) {
        CopySelection(false);
    } else if (key == GLFW_KEY_X && (mods & GLFW_MOD_CONTROL) && gSelectionActive) {
        CopySelection(true);
    } else if (key == GLFW_KEY_V && (mods & GLFW_MOD_CONTROL)) {
        PasteSelection();
    } else if (key == GLFW_KEY_ESCAPE && gPlacementMode != PlacementMode::None) {
        gPlacementMode = PlacementMode::None;
        LOG_INFO("Marker placement cancelled");
    } else if (key == GLFW_KEY_ESCAPE && gSelectionActive) {
        gSelectionActive = false;
        gSelectionDragging = false;
    } else if (key == GLFW_KEY_ESCAPE && gMeasureStage != 0) {
        gMeasureStage = 0;
    } else if (key == GLFW_KEY_ESCAPE) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    } else if (key >= GLFW_KEY_0 && key <= GLFW_KEY_7) {
        gBrush = key - GLFW_KEY_0;
        UpdateWindowTitle();
    } else if (key == GLFW_KEY_G) {
        gShowGrid = !gShowGrid;
        UpdateWindowTitle();
    } else if (key == GLFW_KEY_Y && !(mods & GLFW_MOD_CONTROL)) {
        gHexGrid = !gHexGrid;
        ++gSceneRevision;
        MarkProjectDirty();
        LOG_INFO("Grid geometry: %s (stored tile coordinates are unchanged; existing maps are reinterpreted)",
                 gHexGrid ? "hex" : "square");
        UpdateWindowTitle();
    } else if (key == GLFW_KEY_F || key == GLFW_KEY_F2) {
        SelectTool(gToolMode == ToolMode::FloodFill ? ToolMode::Brush : ToolMode::FloodFill);
    } else if (key == GLFW_KEY_F1) {
        SelectTool(ToolMode::Brush);
    } else if (key == GLFW_KEY_F3) {
        SelectTool(ToolMode::Line);
    } else if (key == GLFW_KEY_F4) {
        SelectTool(ToolMode::Curve);
    } else if (key == GLFW_KEY_F5) {
        SelectTool(ToolMode::Polygon);
    } else if (key == GLFW_KEY_F6) {
        SelectTool(ToolMode::Circle);
    } else if (key == GLFW_KEY_F7) {
        SelectTool(ToolMode::Scatter);
    } else if (key == GLFW_KEY_F8) {
        SelectTool(ToolMode::River);
    } else if (key == GLFW_KEY_F9) {
        SelectTool(ToolMode::TradeRoute);
    } else if (key == GLFW_KEY_F10) {
        SelectTool(ToolMode::Selection);
    } else if (key == GLFW_KEY_F12) {
        SelectTool(ToolMode::Measure);
    } else if (key == GLFW_KEY_H) {
        gRoundBrush = !gRoundBrush;
        LOG_INFO("Brush shape: %s", gRoundBrush ? "round" : "square");
        UpdateWindowTitle();
    } else if (key == GLFW_KEY_BACKSPACE) {
        if (gToolMode == ToolMode::Polygon && !gPolygonPoints.empty()) gPolygonPoints.pop_back();
        else if ((gToolMode == ToolMode::River || gToolMode == ToolMode::TradeRoute) && !gRoutePoints.empty())
            gRoutePoints.pop_back();
        else gCurveStage = 0;
    } else if (key == GLFW_KEY_X && !(mods & GLFW_MOD_CONTROL)) {
        RemoveRouteAtCursor();
    } else if (key == GLFW_KEY_T) {
        if (gPaintMode == PaintMode::Terrain) gPaintMode = PaintMode::Region;
        else if (gPaintMode == PaintMode::Region) gPaintMode = PaintMode::Elevation;
        else if (gPaintMode == PaintMode::Elevation) gPaintMode = PaintMode::Fog;
        else gPaintMode = PaintMode::Terrain;
        LOG_INFO("Paint mode: %s", PaintModeName(gPaintMode));
        UpdateWindowTitle();
    } else if ((key == GLFW_KEY_Q || key == GLFW_KEY_E) && gPaintMode == PaintMode::Elevation) {
        int delta = (key == GLFW_KEY_Q) ? -1 : 1;
        gElevationBrush = std::clamp(gElevationBrush + delta, kMinElevation, kMaxElevation);
        gElevationEditMode = ElevationEditMode::Set;
        LOG_INFO("Elevation brush: %+d", gElevationBrush);
        UpdateWindowTitle();
    } else if (key == GLFW_KEY_F11) {
        gPlayerView = !gPlayerView;
        ++gSceneRevision;
        LOG_INFO("Player view: %s", gPlayerView ? "on" : "off");
        UpdateWindowTitle();
    } else if (key == GLFW_KEY_N) {
        CreateRegion();
        UpdateWindowTitle();
    } else if (key == GLFW_KEY_TAB) {
        CycleActiveRegion();
        UpdateWindowTitle();
    } else if (key == GLFW_KEY_V) {
        gShowRegions = !gShowRegions;
        LOG_INFO("Region overlay: %s", gShowRegions ? "on" : "off");
    } else if (key == GLFW_KEY_L && !(mods & GLFW_MOD_CONTROL)) {
        gShowLabels = !gShowLabels;
        LOG_INFO("Labels: %s", gShowLabels ? "on" : "off");
    } else if (key == GLFW_KEY_M) {
        gPlacementMode = PlacementMode::City;
        LOG_INFO("City placement: click a map tile");
    } else if (key == GLFW_KEY_K) {
        gPlacementMode = PlacementMode::Poi;
        LOG_INFO("POI placement: click a map tile");
    } else if (key == GLFW_KEY_DELETE) {
        if (gSelectionActive) ClearSelectionLayer(gPaintMode);
        else RemoveMarkerAtCursor();
    } else if (key == GLFW_KEY_I) {
        PrintWorldInfo();
    } else if (key == GLFW_KEY_LEFT_BRACKET) {
        gBrushRadius = std::max(0, gBrushRadius - 1);
        UpdateWindowTitle();
    } else if (key == GLFW_KEY_RIGHT_BRACKET) {
        gBrushRadius = std::min(kMaxBrushRadius, gBrushRadius + 1);
        UpdateWindowTitle();
    } else if (key == GLFW_KEY_Z && (mods & GLFW_MOD_CONTROL)) {
        Undo();
        UpdateWindowTitle();
    } else if (key == GLFW_KEY_Y && (mods & GLFW_MOD_CONTROL)) {
        Redo();
        UpdateWindowTitle();
    } else if (key == GLFW_KEY_C && !(mods & GLFW_MOD_CONTROL)) {
        RequestClearActiveLayer();
    } else if (key == GLFW_KEY_HOME) {
        FitMapToWindow();
        UpdateWindowTitle();
    } else if (key == GLFW_KEY_R) {
        gCameraX = -kUiWidth;
        gCameraY = 0.0;
        gZoom = 1.0;
        UpdateWindowTitle();
    } else if (key == GLFW_KEY_P) {
        ExportScreenshot();
    } else if (key == GLFW_KEY_S && (mods & GLFW_MOD_CONTROL)) {
        RequestProjectSave();
    } else if (key == GLFW_KEY_L && (mods & GLFW_MOD_CONTROL)) {
        RequestProjectLoad();
    }
}

// Continuous WASD/arrow-key panning; call once per frame with the elapsed time.
void UpdateCameraPan(double deltaSeconds) {
    if (gModalType != ModalType::None) return;
    double distance = (kPanSpeed / gZoom) * deltaSeconds;
    if (glfwGetKey(gWindow, GLFW_KEY_W) == GLFW_PRESS || glfwGetKey(gWindow, GLFW_KEY_UP) == GLFW_PRESS)
        gCameraY -= distance;
    if (glfwGetKey(gWindow, GLFW_KEY_S) == GLFW_PRESS || glfwGetKey(gWindow, GLFW_KEY_DOWN) == GLFW_PRESS)
        gCameraY += distance;
    if (glfwGetKey(gWindow, GLFW_KEY_A) == GLFW_PRESS || glfwGetKey(gWindow, GLFW_KEY_LEFT) == GLFW_PRESS)
        gCameraX -= distance;
    if (glfwGetKey(gWindow, GLFW_KEY_D) == GLFW_PRESS || glfwGetKey(gWindow, GLFW_KEY_RIGHT) == GLFW_PRESS)
        gCameraX += distance;
}

void AppendTileTriangles(std::vector<float> &vertices, int32_t col, int32_t row,
                         const Vec3 &color, float alpha) {
    auto [worldCx, worldCy] = TileCenterWorld(col, row);
    float cx = static_cast<float>((worldCx - gCameraX) * gZoom);
    float cy = static_cast<float>((worldCy - gCameraY) * gZoom);
    if (kTileSize * gZoom < 3.0) {
        float half = std::max(0.6f, static_cast<float>(kTileSize * gZoom * 0.5));
        const float quad[] = {
            cx - half, cy - half, color.r, color.g, color.b, alpha,
            cx + half, cy - half, color.r, color.g, color.b, alpha,
            cx + half, cy + half, color.r, color.g, color.b, alpha,
            cx - half, cy - half, color.r, color.g, color.b, alpha,
            cx + half, cy + half, color.r, color.g, color.b, alpha,
            cx - half, cy + half, color.r, color.g, color.b, alpha,
        };
        vertices.insert(vertices.end(), std::begin(quad), std::end(quad));
        return;
    }
    int cornerCount = gHexGrid ? 6 : 4;
    std::array<std::pair<float, float>, 6> corners{};
    if (gHexGrid) {
        float radius = static_cast<float>(kTileSize * 0.5 * gZoom);
        for (int i = 0; i < cornerCount; ++i) {
            double angle = i * kPi / 3.0;
            corners[static_cast<size_t>(i)] =
                {cx + radius * static_cast<float>(std::cos(angle)),
                 cy + radius * static_cast<float>(std::sin(angle))};
        }
    } else {
        float half = static_cast<float>(kTileSize * 0.5 * gZoom);
        corners[0] = {cx - half, cy - half};
        corners[1] = {cx + half, cy - half};
        corners[2] = {cx + half, cy + half};
        corners[3] = {cx - half, cy + half};
    }
    for (int i = 0; i < cornerCount; ++i) {
        const auto &a = corners[static_cast<size_t>(i)];
        const auto &b = corners[static_cast<size_t>((i + 1) % cornerCount)];
        vertices.insert(vertices.end(),
                        {cx, cy, color.r, color.g, color.b, alpha,
                         a.first, a.second, color.r, color.g, color.b, alpha,
                         b.first, b.second, color.r, color.g, color.b, alpha});
    }
}

void AppendTileOutline(std::vector<float> &vertices, int32_t col, int32_t row,
                       float r, float g, float b) {
    auto [worldCx, worldCy] = TileCenterWorld(col, row);
    float cx = static_cast<float>((worldCx - gCameraX) * gZoom);
    float cy = static_cast<float>((worldCy - gCameraY) * gZoom);
    int cornerCount = gHexGrid ? 6 : 4;
    std::array<std::pair<float, float>, 6> corners{};
    if (gHexGrid) {
        float radius = static_cast<float>(kTileSize * 0.5 * gZoom);
        for (int i = 0; i < cornerCount; ++i) {
            double angle = i * kPi / 3.0;
            corners[static_cast<size_t>(i)] =
                {cx + radius * static_cast<float>(std::cos(angle)),
                 cy + radius * static_cast<float>(std::sin(angle))};
        }
    } else {
        float half = static_cast<float>(kTileSize * 0.5 * gZoom);
        corners[0] = {cx - half, cy - half};
        corners[1] = {cx + half, cy - half};
        corners[2] = {cx + half, cy + half};
        corners[3] = {cx - half, cy + half};
    }
    for (int i = 0; i < cornerCount; ++i) {
        const auto &a = corners[static_cast<size_t>(i)];
        const auto &next = corners[static_cast<size_t>((i + 1) % cornerCount)];
        vertices.insert(vertices.end(), {a.first, a.second, r, g, b, 1.0f,
                                         next.first, next.second, r, g, b, 1.0f});
    }
}

template <typename Layer, typename Callback>
void ForEachVisibleStoredTile(const Layer &layer, int32_t minCol, int32_t minRow,
                              int32_t maxCol, int32_t maxRow, Callback callback) {
    uint64_t colCount = static_cast<uint64_t>(static_cast<int64_t>(maxCol) - minCol + 1);
    uint64_t rowCount = static_cast<uint64_t>(static_cast<int64_t>(maxRow) - minRow + 1);
    uint64_t visibleCellCount = colCount > std::numeric_limits<uint64_t>::max() / rowCount
                                    ? std::numeric_limits<uint64_t>::max()
                                    : colCount * rowCount;
    if (static_cast<uint64_t>(layer.size()) < visibleCellCount) {
        for (const auto &entry : layer) {
            auto [col, row] = TileCoords(entry.first);
            if (col >= minCol && col <= maxCol && row >= minRow && row <= maxRow)
                callback(col, row, entry.second);
        }
    } else {
        for (int32_t row = minRow; row <= maxRow; ++row) {
            for (int32_t col = minCol; col <= maxCol; ++col) {
                auto it = layer.find(TileKey(col, row));
                if (it != layer.end()) callback(col, row, it->second);
            }
        }
    }
}

// Rebuilds the tile mesh for only the currently visible tiles.
void RebuildVisibleTileMesh(GLuint vbo, GLsizei &outVertexCount) {
    int32_t minCol = 0, minRow = 0, maxCol = 0, maxRow = 0;
    VisibleTileBounds(minCol, minRow, maxCol, maxRow);

    std::vector<float> vertices;
    size_t floatsPerTile = kTileSize * gZoom < 3.0 ? 36 : (gHexGrid ? 108 : 72);
    vertices.reserve(std::min(gMapData.size(), static_cast<size_t>(32768)) * floatsPerTile);
    auto appendElevationTile = [&](int32_t col, int32_t row, uint8_t terrain) {
        int elevation = GetLayerValue(PaintMode::Elevation, TileKey(col, row));
        Vec3 baseColor = gElevationView ? ElevationBandColor(elevation) : kTerrainColors[terrain];
        float levelBrightness = gElevationView ? 1.0f : 1.0f + static_cast<float>(elevation) * 0.075f;
        float shade = ElevationHillshade(col, row);
        Vec3 color{std::clamp(baseColor.r * levelBrightness * shade, 0.0f, 1.0f),
                   std::clamp(baseColor.g * levelBrightness * shade, 0.0f, 1.0f),
                   std::clamp(baseColor.b * levelBrightness * shade, 0.0f, 1.0f)};
        AppendTileTriangles(vertices, col, row, color, 1.0f);
    };
    ForEachVisibleStoredTile(gMapData, minCol, minRow, maxCol, maxRow,
        [&](int32_t col, int32_t row, uint8_t terrain) { appendElevationTile(col, row, terrain); });
    if (gElevationView) {
        ForEachVisibleStoredTile(gElevationData, minCol, minRow, maxCol, maxRow,
            [&](int32_t col, int32_t row, int8_t /*elevation*/) {
                if (gMapData.count(TileKey(col, row)) == 0) appendElevationTile(col, row, 0);
            });
    }

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                 vertices.data(), GL_DYNAMIC_DRAW);
    outVertexCount = static_cast<GLsizei>(vertices.size() / 6);
}

// Rebuilds the translucent region-ownership overlay for the visible tiles.
void RebuildVisibleRegionOverlay(GLuint vbo, GLsizei &outVertexCount) {
    int32_t minCol = 0, minRow = 0, maxCol = 0, maxRow = 0;
    VisibleTileBounds(minCol, minRow, maxCol, maxRow);

    std::vector<float> vertices;
    size_t floatsPerTile = kTileSize * gZoom < 3.0 ? 36 : (gHexGrid ? 108 : 72);
    vertices.reserve(std::min(gRegionData.size(), static_cast<size_t>(32768)) * floatsPerTile);
    ForEachVisibleStoredTile(gRegionData, minCol, minRow, maxCol, maxRow,
        [&](int32_t col, int32_t row, uint8_t regionId) {
            auto regionIt = gRegions.find(regionId);
            if (regionIt == gRegions.end()) return;

            const Vec3 &c = regionIt->second.color;
            AppendTileTriangles(vertices, col, row, c, kRegionOverlayAlpha);
        });

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                 vertices.data(), GL_DYNAMIC_DRAW);
    outVertexCount = static_cast<GLsizei>(vertices.size() / 6);
}

void RebuildElevationContours(GLuint vbo, GLsizei &outVertexCount) {
    std::vector<float> vertices;
    if (!gShowElevationContours || kTileSize * gZoom < 4.0) {
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
        outVertexCount = 0;
        return;
    }
    int32_t minCol = 0, minRow = 0, maxCol = 0, maxRow = 0;
    VisibleTileBounds(minCol, minRow, maxCol, maxRow);
    auto contourBand = [](int elevation) {
        int relative = elevation - gSeaLevel;
        if (relative >= 0) return relative / gContourInterval;
        return -((-relative + gContourInterval - 1) / gContourInterval);
    };
    auto appendIfBoundary = [&](int32_t col, int32_t row) {
        uint64_t key = TileKey(col, row);
        int elevation = GetLayerValue(PaintMode::Elevation, key);
        int band = contourBand(elevation);
        int neighborCount = 0;
        auto neighbors = NeighborTiles(col, row, neighborCount);
        bool boundary = false;
        bool seaBoundary = false;
        for (int i = 0; i < neighborCount; ++i) {
            uint64_t neighborKey = TileKey(neighbors[i].first, neighbors[i].second);
            if (gMapData.count(neighborKey) == 0 && gElevationData.count(neighborKey) == 0) continue;
            int neighborElevation = GetLayerValue(PaintMode::Elevation, neighborKey);
            if (contourBand(neighborElevation) != band) boundary = true;
            if ((elevation <= gSeaLevel) != (neighborElevation <= gSeaLevel)) seaBoundary = true;
        }
        if (boundary) {
            if (seaBoundary) AppendTileOutline(vertices, col, row, 0.15f, 0.78f, 0.95f);
            else AppendTileOutline(vertices, col, row, 0.08f, 0.09f, 0.11f);
        }
    };
    ForEachVisibleStoredTile(gMapData, minCol, minRow, maxCol, maxRow,
        [&](int32_t col, int32_t row, uint8_t /*terrain*/) { appendIfBoundary(col, row); });
    ForEachVisibleStoredTile(gElevationData, minCol, minRow, maxCol, maxRow,
        [&](int32_t col, int32_t row, int8_t /*elevation*/) {
            if (gMapData.count(TileKey(col, row)) == 0) appendIfBoundary(col, row);
        });
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                 vertices.data(), GL_DYNAMIC_DRAW);
    outVertexCount = static_cast<GLsizei>(vertices.size() / 6);
}

// Fog is drawn after all map content so obscured cities, POIs, routes and labels remain hidden.
void RebuildVisibleFogOverlay(GLuint vbo, GLsizei &outVertexCount) {
    int32_t minCol = 0, minRow = 0, maxCol = 0, maxRow = 0;
    VisibleTileBounds(minCol, minRow, maxCol, maxRow);
    std::vector<float> vertices;
    size_t floatsPerTile = kTileSize * gZoom < 3.0 ? 36 : (gHexGrid ? 108 : 72);
    vertices.reserve(std::min(gFogData.size(), static_cast<size_t>(32768)) * floatsPerTile);
    const float alpha = gPlayerView ? 0.985f : 0.58f;
    ForEachVisibleStoredTile(gFogData, minCol, minRow, maxCol, maxRow,
        [&](int32_t col, int32_t row, uint8_t /*hidden*/) {
            AppendTileTriangles(vertices, col, row, {0.015f, 0.018f, 0.025f}, alpha);
        });
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                 vertices.data(), GL_DYNAMIC_DRAW);
    outVertexCount = static_cast<GLsizei>(vertices.size() / 6);
}

// Rebuilds city markers (a bordered diamond) for cities currently in view.
void RebuildCityMarkers(GLuint vbo, GLsizei &outVertexCount) {
    std::vector<float> vertices;
    const float borderR = 0.05f, borderG = 0.05f, borderB = 0.05f;
    const float fillR = 0.95f, fillG = 0.85f, fillB = 0.15f;

    for (const auto &city : gCities) {
        if (gPlayerView && gFogData.count(TileKey(city.col, city.row)) != 0) continue;
        auto [worldCx, worldCy] = TileCenterWorld(city.col, city.row);
        float cx = static_cast<float>((worldCx - gCameraX) * gZoom);
        float cy = static_cast<float>((worldCy - gCameraY) * gZoom);
        if (cx < -kTileSize || cy < -kTileSize || cx > gWindowWidth + kTileSize ||
            cy > gWindowHeight + kTileSize)
            continue;

        float outer = static_cast<float>(kTileSize * gZoom * 0.4);
        float inner = static_cast<float>(kTileSize * gZoom * 0.28);

        auto addDiamond = [&](float radius, float r, float g, float b) {
            // clang-format off
            const float quad[] = {
                cx, cy - radius, r, g, b, 1.0f,
                cx + radius, cy, r, g, b, 1.0f,
                cx, cy + radius, r, g, b, 1.0f,

                cx, cy - radius, r, g, b, 1.0f,
                cx, cy + radius, r, g, b, 1.0f,
                cx - radius, cy, r, g, b, 1.0f,
            };
            // clang-format on
            vertices.insert(vertices.end(), std::begin(quad), std::end(quad));
        };
        addDiamond(outer, borderR, borderG, borderB);
        addDiamond(inner, fillR, fillG, fillB);
    }

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                 vertices.data(), GL_DYNAMIC_DRAW);
    outVertexCount = static_cast<GLsizei>(vertices.size() / 6);
}

// Appends a filled regular polygon (triangle fan) centered at (cx, cy) in screen space.
void AppendRegularPolygon(std::vector<float> &vertices, float cx, float cy, float radius, int sides,
                          float rotation, float r, float g, float b) {
    for (int i = 0; i < sides; ++i) {
        float a0 = rotation + static_cast<float>(2.0 * kPi * i / sides);
        float a1 = rotation + static_cast<float>(2.0 * kPi * (i + 1) / sides);
        // clang-format off
        const float tri[] = {
            cx, cy, r, g, b, 1.0f,
            cx + radius * std::cos(a0), cy + radius * std::sin(a0), r, g, b, 1.0f,
            cx + radius * std::cos(a1), cy + radius * std::sin(a1), r, g, b, 1.0f,
        };
        // clang-format on
        vertices.insert(vertices.end(), std::begin(tri), std::end(tri));
    }
}

// Rebuilds POI and DM-only encounter markers currently in view.
void RebuildPoiMarkers(GLuint vbo, GLsizei &outVertexCount) {
    std::vector<float> vertices;

    for (const auto &poi : gPois) {
        if (gPlayerView && gFogData.count(TileKey(poi.col, poi.row)) != 0) continue;
        auto [worldCx, worldCy] = TileCenterWorld(poi.col, poi.row);
        float cx = static_cast<float>((worldCx - gCameraX) * gZoom);
        float cy = static_cast<float>((worldCy - gCameraY) * gZoom);
        if (cx < -kTileSize || cy < -kTileSize || cx > gWindowWidth + kTileSize ||
            cy > gWindowHeight + kTileSize)
            continue;

        PoiVisual visual = GetPoiVisual(poi.kind);
        float outer = static_cast<float>(kTileSize * gZoom * 0.38);
        float inner = static_cast<float>(kTileSize * gZoom * 0.27);
        AppendRegularPolygon(vertices, cx, cy, outer, visual.sides, visual.rotation, 0.05f, 0.05f, 0.05f);
        AppendRegularPolygon(vertices, cx, cy, inner, visual.sides, visual.rotation, visual.color.r,
                            visual.color.g, visual.color.b);
    }

    if (!gPlayerView) {
        for (const auto &encounter : gEncounters) {
            auto [worldCx, worldCy] = TileCenterWorld(encounter.col, encounter.row);
            float cx = static_cast<float>((worldCx - gCameraX) * gZoom);
            float cy = static_cast<float>((worldCy - gCameraY) * gZoom);
            if (cx < -kTileSize || cy < -kTileSize || cx > gWindowWidth + kTileSize ||
                cy > gWindowHeight + kTileSize)
                continue;

            float outer = static_cast<float>(kTileSize * gZoom * 0.42);
            float inner = static_cast<float>(kTileSize * gZoom * 0.31);
            float center = static_cast<float>(kTileSize * gZoom * 0.11);
            AppendRegularPolygon(vertices, cx, cy, outer, 6, 0.0f, 0.05f, 0.05f, 0.05f);
            AppendRegularPolygon(vertices, cx, cy, inner, 6, 0.0f, 0.95f, 0.32f, 0.12f);
            AppendRegularPolygon(vertices, cx, cy, center, 6, 0.0f, 0.16f, 0.05f, 0.03f);
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                 vertices.data(), GL_DYNAMIC_DRAW);
    outVertexCount = static_cast<GLsizei>(vertices.size() / 6);
}

// Appends a thick line segment (a screen-space quad) between two world-space points.
void AppendThickLineWorld(std::vector<float> &vertices, double x0, double y0, double x1, double y1,
                          float thicknessWorld, Vec3 color) {
    double dx = x1 - x0, dy = y1 - y0;
    double len = std::hypot(dx, dy);
    if (len < 1e-6) return;
    double nx = -dy / len * thicknessWorld * 0.5;
    double ny = dx / len * thicknessWorld * 0.5;

    auto toScreen = [&](double wx, double wy) {
        return std::pair<float, float>(static_cast<float>((wx - gCameraX) * gZoom),
                                       static_cast<float>((wy - gCameraY) * gZoom));
    };
    auto [ax, ay] = toScreen(x0 + nx, y0 + ny);
    auto [bx, by] = toScreen(x1 + nx, y1 + ny);
    auto [cx, cy] = toScreen(x1 - nx, y1 - ny);
    auto [dxs, dys] = toScreen(x0 - nx, y0 - ny);

    // clang-format off
    const float quad[] = {
        ax, ay, color.r, color.g, color.b, 1.0f,
        bx, by, color.r, color.g, color.b, 1.0f,
        cx, cy, color.r, color.g, color.b, 1.0f,

        ax, ay, color.r, color.g, color.b, 1.0f,
        cx, cy, color.r, color.g, color.b, 1.0f,
        dxs, dys, color.r, color.g, color.b, 1.0f,
    };
    // clang-format on
    vertices.insert(vertices.end(), std::begin(quad), std::end(quad));
}

// Rebuilds ribbon meshes for all rivers/trade routes (drawn as thick lines, not painted tiles).
// Trade routes are dashed (alternating on/off sub-segments) to read differently from rivers.
void RebuildRouteMesh(GLuint vbo, GLsizei &outVertexCount) {
    std::vector<float> vertices;
    const double dashLength = kTileSize * 0.6;

    for (const auto &route : gRoutes) {
        Vec3 color = RouteColor(route.kind);
        float thickness = RouteThickness(route.kind);
        for (size_t i = 0; i + 1 < route.points.size(); ++i) {
            double x0 = route.points[i].first, y0 = route.points[i].second;
            double x1 = route.points[i + 1].first, y1 = route.points[i + 1].second;
            double viewMinX = gCameraX - thickness;
            double viewMinY = gCameraY - thickness;
            double viewMaxX = gCameraX + gWindowWidth / gZoom + thickness;
            double viewMaxY = gCameraY + gWindowHeight / gZoom + thickness;
            if (std::max(x0, x1) < viewMinX || std::min(x0, x1) > viewMaxX ||
                std::max(y0, y1) < viewMinY || std::min(y0, y1) > viewMaxY)
                continue;
            if (route.kind == RouteKind::River) {
                AppendThickLineWorld(vertices, x0, y0, x1, y1, thickness, color);
                continue;
            }
            double segLen = std::hypot(x1 - x0, y1 - y0);
            int dashCount = std::max(1, static_cast<int>(segLen / dashLength));
            for (int d = 0; d < dashCount; ++d) {
                if (d % 2 != 0) continue; // skip every other sub-segment for a dashed look
                double t0 = static_cast<double>(d) / dashCount;
                double t1 = static_cast<double>(d + 1) / dashCount;
                AppendThickLineWorld(vertices, x0 + (x1 - x0) * t0, y0 + (y1 - y0) * t0,
                                    x0 + (x1 - x0) * t1, y0 + (y1 - y0) * t1, thickness, color);
            }
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                 vertices.data(), GL_DYNAMIC_DRAW);
    outVertexCount = static_cast<GLsizei>(vertices.size() / 6);
}

// Appends filled-quad text (in world space, centered horizontally on worldCenterX at worldTopY)
// using the tiny 3x5 pixel font.
void AppendLabelText(std::vector<float> &vertices, const std::string &text, double worldCenterX,
                     double worldTopY, Vec3 color) {
    if (text.empty()) return;
    float width = static_cast<float>(text.size()) * kLabelGlyphAdvance - kLabelPixelSize;
    double startX = worldCenterX - width * 0.5;

    for (char ch : text) {
        Glyph3x5 glyph = GetGlyph(ch);
        for (int row = 0; row < 5; ++row) {
            for (int col = 0; col < 3; ++col) {
                if (!((glyph.rows[row] >> (2 - col)) & 1)) continue;
                double wx0 = startX + col * kLabelPixelSize;
                double wy0 = worldTopY + row * kLabelPixelSize;
                float x0 = static_cast<float>((wx0 - gCameraX) * gZoom);
                float y0 = static_cast<float>((wy0 - gCameraY) * gZoom);
                float x1 = static_cast<float>((wx0 + kLabelPixelSize - gCameraX) * gZoom);
                float y1 = static_cast<float>((wy0 + kLabelPixelSize - gCameraY) * gZoom);

                // clang-format off
                const float quad[] = {
                    x0, y0, color.r, color.g, color.b, 1.0f,
                    x1, y0, color.r, color.g, color.b, 1.0f,
                    x1, y1, color.r, color.g, color.b, 1.0f,

                    x0, y0, color.r, color.g, color.b, 1.0f,
                    x1, y1, color.r, color.g, color.b, 1.0f,
                    x0, y1, color.r, color.g, color.b, 1.0f,
                };
                // clang-format on
                vertices.insert(vertices.end(), std::begin(quad), std::end(quad));
            }
        }
        startX += kLabelGlyphAdvance;
    }
}

// Rebuilds city-name and region-name labels for whatever is currently in view.
void RebuildLabelMesh(GLuint vbo, GLsizei &outVertexCount) {
    std::vector<float> vertices;
    const Vec3 cityLabelColor{0.95f, 0.85f, 0.15f};
    const Vec3 regionLabelColor{0.95f, 0.95f, 0.95f};

    if (gElevationView) {
        if (kTileSize * gZoom >= 20.0) {
            int32_t minCol = 0, minRow = 0, maxCol = 0, maxRow = 0;
            VisibleTileBounds(minCol, minRow, maxCol, maxRow);
            auto appendHeight = [&](int32_t col, int32_t row) {
                int elevation = GetLayerValue(PaintMode::Elevation, TileKey(col, row));
                auto [worldX, worldY] = TileCenterWorld(col, row);
                std::string label = elevation > 0 ? "+" + std::to_string(elevation) : std::to_string(elevation);
                Vec3 color = elevation >= 2 ? Vec3{0.08f, 0.09f, 0.11f} : Vec3{0.96f, 0.97f, 1.0f};
                AppendLabelText(vertices, label, worldX, worldY - kLabelPixelSize * 2.5, color);
            };
            ForEachVisibleStoredTile(gMapData, minCol, minRow, maxCol, maxRow,
                [&](int32_t col, int32_t row, uint8_t /*terrain*/) { appendHeight(col, row); });
            ForEachVisibleStoredTile(gElevationData, minCol, minRow, maxCol, maxRow,
                [&](int32_t col, int32_t row, int8_t /*elevation*/) {
                    if (gMapData.count(TileKey(col, row)) == 0) appendHeight(col, row);
                });
        }
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                     vertices.data(), GL_DYNAMIC_DRAW);
        outVertexCount = static_cast<GLsizei>(vertices.size() / 6);
        return;
    }

    for (const auto &city : gCities) {
        if (gPlayerView && gFogData.count(TileKey(city.col, city.row)) != 0) continue;
        auto [worldCenterX, worldCenterY] = TileCenterWorld(city.col, city.row);
        float cx = static_cast<float>((worldCenterX - gCameraX) * gZoom);
        float cy = static_cast<float>((worldCenterY - gCameraY) * gZoom);
        if (cx < -kTileSize || cy < -kTileSize || cx > gWindowWidth + kTileSize ||
            cy > gWindowHeight + kTileSize)
            continue;
        double worldTopY = worldCenterY + (gHexGrid ? kHexRowHeight : kTileSize) * 0.5 + kLabelPixelSize;
        AppendLabelText(vertices, city.name, worldCenterX, worldTopY, cityLabelColor);
    }

    for (const auto &poi : gPois) {
        if (gPlayerView && gFogData.count(TileKey(poi.col, poi.row)) != 0) continue;
        auto [worldCenterX, worldCenterY] = TileCenterWorld(poi.col, poi.row);
        float cx = static_cast<float>((worldCenterX - gCameraX) * gZoom);
        float cy = static_cast<float>((worldCenterY - gCameraY) * gZoom);
        if (cx < -kTileSize || cy < -kTileSize || cx > gWindowWidth + kTileSize ||
            cy > gWindowHeight + kTileSize)
            continue;
        double worldTopY = worldCenterY + (gHexGrid ? kHexRowHeight : kTileSize) * 0.5 + kLabelPixelSize;
        AppendLabelText(vertices, poi.name, worldCenterX, worldTopY, GetPoiVisual(poi.kind).color);
    }

    if (!gPlayerView) {
        for (const auto &encounter : gEncounters) {
            auto [worldCenterX, worldCenterY] = TileCenterWorld(encounter.col, encounter.row);
            float cx = static_cast<float>((worldCenterX - gCameraX) * gZoom);
            float cy = static_cast<float>((worldCenterY - gCameraY) * gZoom);
            if (cx < -kTileSize || cy < -kTileSize || cx > gWindowWidth + kTileSize ||
                cy > gWindowHeight + kTileSize)
                continue;
            double worldTopY =
                worldCenterY + (gHexGrid ? kHexRowHeight : kTileSize) * 0.5 + kLabelPixelSize;
            AppendLabelText(vertices, encounter.name, worldCenterX, worldTopY,
                            {0.95f, 0.32f, 0.12f});
        }
    }

    if (gShowRegions) {
        // Average the visible tiles of each region to find a reasonable label position.
        int32_t minCol = 0, minRow = 0, maxCol = 0, maxRow = 0;
        VisibleTileBounds(minCol, minRow, maxCol, maxRow);

        std::unordered_map<int, std::tuple<double, double, int>> sums; // regionId -> (sumX, sumY, count)
        ForEachVisibleStoredTile(gRegionData, minCol, minRow, maxCol, maxRow,
            [&](int32_t col, int32_t row, uint8_t regionId) {
                if (gPlayerView && gFogData.count(TileKey(col, row)) != 0) return;
                auto &entry = sums[regionId];
                auto [worldX, worldY] = TileCenterWorld(col, row);
                std::get<0>(entry) += worldX;
                std::get<1>(entry) += worldY;
                std::get<2>(entry) += 1;
            });
        for (const auto &entry : sums) {
            int count = std::get<2>(entry.second);
            if (count < 4) continue; // skip slivers too small to label usefully
            auto regionIt = gRegions.find(entry.first);
            if (regionIt == gRegions.end()) continue;
            double cx = std::get<0>(entry.second) / count;
            double cy = std::get<1>(entry.second) / count;
            AppendLabelText(vertices, regionIt->second.name, cx, cy, regionLabelColor);
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                 vertices.data(), GL_DYNAMIC_DRAW);
    outVertexCount = static_cast<GLsizei>(vertices.size() / 6);
}

// Rebuilds grid lines spanning the current viewport only.
void RebuildVisibleGridLines(GLuint vbo, GLsizei &outVertexCount) {
    int32_t minCol = 0, minRow = 0, maxCol = 0, maxRow = 0;
    VisibleTileBounds(minCol, minRow, maxCol, maxRow);

    const float r = 0.35f, g = 0.35f, b = 0.38f;
    std::vector<float> vertices;

    if (gHexGrid) {
        // Dense hex outlines become visual noise and expensive below this screen size.
        if (kTileSize * gZoom >= 8.0) {
            for (int32_t row = minRow; row <= maxRow; ++row)
                for (int32_t col = minCol; col <= maxCol; ++col)
                    AppendTileOutline(vertices, col, row, r, g, b);
        }
    } else {
        for (int32_t col = minCol; col <= maxCol; ++col) {
            float x = static_cast<float>((col * kTileSize - gCameraX) * gZoom);
            vertices.insert(vertices.end(),
                            {x, 0.0f, r, g, b, 1.0f, x, static_cast<float>(gWindowHeight), r, g, b, 1.0f});
        }
        for (int32_t row = minRow; row <= maxRow; ++row) {
            float y = static_cast<float>((row * kTileSize - gCameraY) * gZoom);
            vertices.insert(vertices.end(),
                            {0.0f, y, r, g, b, 1.0f, static_cast<float>(gWindowWidth), y, r, g, b, 1.0f});
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                 vertices.data(), GL_DYNAMIC_DRAW);
    outVertexCount = static_cast<GLsizei>(vertices.size() / 6);
}

// Rebuilds the outline of the in-progress Shift+drag rectangle fill, if any.
void RebuildRectPreview(GLuint vbo, GLsizei &outVertexCount) {
    bool showSelection = gSelectionDragging || gSelectionActive;
    if (!gRectDragging && !showSelection) {
        outVertexCount = 0;
        return;
    }

    int32_t startCol = gRectDragging ? gRectStartCol : gSelectionStartCol;
    int32_t startRow = gRectDragging ? gRectStartRow : gSelectionStartRow;
    int32_t endCol = gRectDragging ? gRectEndCol : gSelectionEndCol;
    int32_t endRow = gRectDragging ? gRectEndRow : gSelectionEndRow;
    int32_t minCol = std::min(startCol, endCol);
    int32_t maxCol = std::max(startCol, endCol);
    int32_t minRow = std::min(startRow, endRow);
    int32_t maxRow = std::max(startRow, endRow);
    const float r = gRectDragging ? 1.0f : 0.2f;
    const float g = gRectDragging ? 0.9f : 0.85f;
    const float b = gRectDragging ? 0.2f : 1.0f;

    if (gHexGrid) {
        std::vector<float> vertices;
        for (int32_t col = minCol; col <= maxCol; ++col) {
            AppendTileOutline(vertices, col, minRow, r, g, b);
            if (maxRow != minRow) AppendTileOutline(vertices, col, maxRow, r, g, b);
        }
        for (int32_t row = minRow + 1; row < maxRow; ++row) {
            AppendTileOutline(vertices, minCol, row, r, g, b);
            if (maxCol != minCol) AppendTileOutline(vertices, maxCol, row, r, g, b);
        }
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                     vertices.data(), GL_DYNAMIC_DRAW);
        outVertexCount = static_cast<GLsizei>(vertices.size() / 6);
        return;
    }

    float x0 = static_cast<float>((minCol * kTileSize - gCameraX) * gZoom);
    float y0 = static_cast<float>((minRow * kTileSize - gCameraY) * gZoom);
    float x1 = static_cast<float>(((maxCol + 1) * kTileSize - gCameraX) * gZoom);
    float y1 = static_cast<float>(((maxRow + 1) * kTileSize - gCameraY) * gZoom);

    // clang-format off
    const float lines[] = {
        x0, y0, r, g, b, 1.0f,  x1, y0, r, g, b, 1.0f,
        x1, y0, r, g, b, 1.0f,  x1, y1, r, g, b, 1.0f,
        x1, y1, r, g, b, 1.0f,  x0, y1, r, g, b, 1.0f,
        x0, y1, r, g, b, 1.0f,  x0, y0, r, g, b, 1.0f,
    };
    // clang-format on

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(lines), lines, GL_DYNAMIC_DRAW);
    outVertexCount = static_cast<GLsizei>(sizeof(lines) / (6 * sizeof(float)));
}

// Rebuilds the live preview for whichever line/curve/polygon/circle tool is active.
void RebuildToolPreview(GLuint vbo, GLsizei &outVertexCount) {
    std::vector<float> vertices;
    const float r = 1.0f, g = 0.9f, b = 0.2f, a = 1.0f;

    auto toScreen = [&](double wx, double wy) {
        return std::pair<float, float>(static_cast<float>((wx - gCameraX) * gZoom),
                                       static_cast<float>((wy - gCameraY) * gZoom));
    };
    auto addLine = [&](double x0, double y0, double x1, double y1) {
        auto [sx0, sy0] = toScreen(x0, y0);
        auto [sx1, sy1] = toScreen(x1, y1);
        vertices.insert(vertices.end(), {sx0, sy0, r, g, b, a, sx1, sy1, r, g, b, a});
    };

    auto [curWX, curWY] = CursorWorld();

    if (gPlacementMode != PlacementMode::None) {
        auto [col, row] = WorldToTile(curWX, curWY);
        AppendTileOutline(vertices, col, row, 1.0f, 0.55f, 0.15f);
    } else if (gToolMode == ToolMode::Measure && gMeasureStage != 0) {
        auto [startX, startY] = TileCenterWorld(gMeasureStartCol, gMeasureStartRow);
        auto [endX, endY] = TileCenterWorld(gMeasureEndCol, gMeasureEndRow);
        addLine(startX, startY, endX, endY);
        AppendTileOutline(vertices, gMeasureStartCol, gMeasureStartRow, 0.20f, 0.85f, 1.0f);
        AppendTileOutline(vertices, gMeasureEndCol, gMeasureEndRow, 0.20f, 0.85f, 1.0f);
    } else if (gToolMode == ToolMode::Line && gLineDragging) {
        addLine(gLineStartWX, gLineStartWY, curWX, curWY);
    } else if (gToolMode == ToolMode::Circle && gCircleDragging) {
        double radius = std::hypot(curWX - gCircleCenterWX, curWY - gCircleCenterWY);
        constexpr int segs = 32;
        for (int i = 0; i < segs; ++i) {
            double a0 = 2.0 * kPi * i / segs, a1 = 2.0 * kPi * (i + 1) / segs;
            addLine(gCircleCenterWX + radius * std::cos(a0), gCircleCenterWY + radius * std::sin(a0),
                    gCircleCenterWX + radius * std::cos(a1), gCircleCenterWY + radius * std::sin(a1));
        }
    } else if (gToolMode == ToolMode::Curve && gCurveStage > 0) {
        if (gCurveStage == 1) {
            addLine(gCurveP0X, gCurveP0Y, curWX, curWY);
        } else {
            constexpr int segs = 24;
            double px = gCurveP0X, py = gCurveP0Y;
            for (int i = 1; i <= segs; ++i) {
                double t = static_cast<double>(i) / segs, mt = 1.0 - t;
                double x = mt * mt * gCurveP0X + 2.0 * mt * t * gCurveP1X + t * t * curWX;
                double y = mt * mt * gCurveP0Y + 2.0 * mt * t * gCurveP1Y + t * t * curWY;
                addLine(px, py, x, y);
                px = x;
                py = y;
            }
            addLine(gCurveP0X, gCurveP0Y, gCurveP1X, gCurveP1Y);
            addLine(gCurveP1X, gCurveP1Y, curWX, curWY);
        }
    } else if (gToolMode == ToolMode::Polygon && !gPolygonPoints.empty()) {
        for (size_t i = 0; i + 1 < gPolygonPoints.size(); ++i) {
            addLine(gPolygonPoints[i].first, gPolygonPoints[i].second, gPolygonPoints[i + 1].first,
                    gPolygonPoints[i + 1].second);
        }
        addLine(gPolygonPoints.back().first, gPolygonPoints.back().second, curWX, curWY);
        if (gPolygonPoints.size() >= 2) {
            addLine(curWX, curWY, gPolygonPoints.front().first, gPolygonPoints.front().second);
        }
    } else if ((gToolMode == ToolMode::River || gToolMode == ToolMode::TradeRoute) &&
              !gRoutePoints.empty()) {
        for (size_t i = 0; i + 1 < gRoutePoints.size(); ++i) {
            addLine(gRoutePoints[i].first, gRoutePoints[i].second, gRoutePoints[i + 1].first,
                    gRoutePoints[i + 1].second);
        }
        addLine(gRoutePoints.back().first, gRoutePoints.back().second, curWX, curWY);
    }

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                 vertices.data(), GL_DYNAMIC_DRAW);
    outVertexCount = static_cast<GLsizei>(vertices.size() / 6);
}

} // namespace

int main() {
    LoggerInit(kLogFile);
    LOG_INFO("Starting DND Map Drawer, log file: %s", kLogFile);

    glfwSetErrorCallback(GlfwErrorCallback);

    if (!glfwInit()) {
        LOG_ERROR("Failed to initialize GLFW");
        LoggerShutdown();
        return EXIT_FAILURE;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 4);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    gWindow = glfwCreateWindow(gWindowWidth, gWindowHeight, "DND Map Drawer", nullptr, nullptr);
    if (!gWindow) {
        LOG_ERROR("Failed to create GLFW window");
        glfwTerminate();
        LoggerShutdown();
        return EXIT_FAILURE;
    }
    glfwSetWindowSizeLimits(gWindow, 900, 880, GLFW_DONT_CARE, GLFW_DONT_CARE);

    glfwMakeContextCurrent(gWindow);
    glfwSetFramebufferSizeCallback(gWindow, FramebufferSizeCallback);
    glfwSetWindowSizeCallback(gWindow, WindowSizeCallback);
    glfwSetMouseButtonCallback(gWindow, MouseButtonCallback);
    glfwSetCursorPosCallback(gWindow, CursorPosCallback);
    glfwSetScrollCallback(gWindow, ScrollCallback);
    glfwSetKeyCallback(gWindow, KeyCallback);
    glfwSetCharCallback(gWindow, CharacterCallback);

    if (!LoadGLFunctions(reinterpret_cast<void *(*)(const char *)>(glfwGetProcAddress))) {
        LOG_ERROR("Failed to load required OpenGL functions");
        glfwDestroyWindow(gWindow);
        glfwTerminate();
        LoggerShutdown();
        return EXIT_FAILURE;
    }

    glEnable(GL_MULTISAMPLE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    GLuint shaderProgram = CreateShaderProgram(kVertexShaderSource, kFragmentShaderSource);
    glUseProgram(shaderProgram);
    GLint resolutionLoc = glGetUniformLocation(shaderProgram, "uResolution");

    MeshBuffer tileMesh, gridMesh, selectionMesh, regionMesh, contourMesh, fogMesh;
    MeshBuffer cityMesh, poiMesh, routeMesh, toolMesh, labelMesh, guiMesh;
    std::array<MeshBuffer *, 12> meshes{
        &tileMesh, &gridMesh, &selectionMesh, &regionMesh, &contourMesh, &fogMesh,
        &cityMesh, &poiMesh, &routeMesh, &toolMesh, &labelMesh, &guiMesh,
    };
    for (MeshBuffer *mesh : meshes) mesh->Initialize();

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    std::srand(static_cast<unsigned>(std::time(nullptr)));

    LoadProjectConfig();
    UpdateWindowTitle();
    CheckForRecoveryAutosave();
    LOG_INFO("DND Overworld Map Drawer (infinite canvas)");
    LOG_INFO("  GUI sidebar       : tools, terrain, layers, world objects, project and view controls");
    LOG_INFO("  Left click / drag : use the active tool (paint / fill / draw / scatter)");
    LOG_INFO("  Right click / drag: erase with the active tool");
    LOG_INFO("  Middle drag       : pan quickly across large maps");
    LOG_INFO("  Shift + drag      : (Brush tool) fill/erase a rectangle");
    LOG_INFO("  F1                : Brush tool");
    LOG_INFO("  F2 / F            : Flood Fill tool (click fills a contiguous region)");
    LOG_INFO("  F3                : Line tool (drag between two points)");
    LOG_INFO("  F4                : Curve tool (click start, control, end)");
    LOG_INFO("  F5                : Polygon tool (click vertices, right-click to fill)");
    LOG_INFO("  F6                : Circle tool (drag from center to edge)");
    LOG_INFO("  F7                : Scatter tool (randomly speckles tiles - good for forests)");
    LOG_INFO("  F8                : River tool (click points, right-click to name & commit)");
    LOG_INFO("  F9                : Trade Route tool (click points, right-click to name & commit)");
    LOG_INFO("  F10               : Selection tool (drag a tile area, right-click to clear)");
    LOG_INFO("  F12               : Measure tool (click start/end; right-click clears)");
    LOG_INFO("                      Reports tiles, km at 10 km/tile, and 40-km walking days");
    LOG_INFO("  Ctrl+C/X/V        : copy / cut / paste the active tile layer selection");
    LOG_INFO("  Delete            : clear the active layer inside a selection");
    LOG_INFO("  H                 : toggle round/square brush shape (round = more organic edges)");
    LOG_INFO("  Backspace         : cancel pending curve/polygon/route (or remove last point)");
    LOG_INFO("  T                 : cycle paint mode: Terrain / Region / Elevation / Fog");
    LOG_INFO("                      In Fog mode, left paints darkness and right reveals tiles");
    LOG_INFO("  Elevation layer   : Set, Raise, Lower, Flatten and Smooth editing modes");
    LOG_INFO("                      Levels -4 to +8 represent %d metres each", gMetresPerElevationLevel);
    LOG_INFO("  Elevation view    : discrete height colors, contour boundaries and hill shading");
    LOG_INFO("  Relief            : derive starter heights from terrain after confirmation");
    LOG_INFO("  Q / E             : lower / raise the exact elevation value (-4 to +8)");
    LOG_INFO("  N                 : found a new region (opens an in-window form)");
    LOG_INFO("  Tab               : cycle the active region");
    LOG_INFO("  V                 : toggle region overlay visibility");
    LOG_INFO("  L                 : toggle city/region name labels");
    LOG_INFO("  M                 : arm city placement, then click its tile and complete the form");
    LOG_INFO("  K                 : arm POI placement, then click its tile and complete the form");
    LOG_INFO("  Click city / POI  : open its information card");
    LOG_INFO("  Click encounter   : edit its name and description");
    LOG_INFO("  Ctrl+F / Find     : search and jump to cities, POIs, encounters, regions or routes");
    LOG_INFO("                      Repeat a search to cycle through matching results");
    LOG_INFO("  Ctrl+E / Encounter: arm encounter placement, then click its tile and complete the form");
    LOG_INFO("  Delete            : otherwise remove the city, POI, or encounter at the cursor");
    LOG_INFO("  X                 : remove the river/trade route nearest the cursor");
    LOG_INFO("  I                 : print region/city/POI/route info to the console/log");
    LOG_INFO("  [ / ]             : shrink / grow brush size");
    LOG_INFO("  Ctrl+Z / Ctrl+Y   : undo / redo");
    LOG_INFO("  Keys 0-7          : select terrain brush (0=Empty 1=Plains 2=Forest 3=Water");
    LOG_INFO("                      4=Mountain 5=Desert 6=Hills 7=Road)");
    LOG_INFO("  WASD / Arrows     : pan the camera");
    LOG_INFO("  Mouse wheel       : zoom in/out (centered on cursor)");
    LOG_INFO("  R                 : reset camera/zoom");
    LOG_INFO("  Home / Fit Map    : frame all map content in the viewport");
    LOG_INFO("  F11 / Player      : toggle near-opaque player-view fog preview");
    LOG_INFO("  Fog controls      : Hide All covers terrain; Reveal All removes all fog");
    LOG_INFO("  G                 : toggle grid lines");
    LOG_INFO("  Y                 : toggle hex/square grid geometry (hex is the default)");
    LOG_INFO("                      Existing maps are geometrically reinterpreted when toggled");
    LOG_INFO("  C                 : clear the active paint layer after confirmation");
    LOG_INFO("  P                 : export screenshot (map_export.bmp)");
    LOG_INFO("  Ctrl+S / Ctrl+L   : save / load project (map_drawer_project.txt)");
    LOG_INFO("                      Load falls back to legacy multi-file projects when needed");
    LOG_INFO("  Autosave          : recovery copy written every five minutes while modified");
    LOG_INFO("  Help / ?          : show the in-application keybind reference");
    LOG_INFO("  Escape            : quit");

    double lastTime = glfwGetTime();
    double lastAutosaveTime = lastTime;
    double lastRenderCameraX = std::numeric_limits<double>::quiet_NaN();
    double lastRenderCameraY = std::numeric_limits<double>::quiet_NaN();
    double lastRenderZoom = std::numeric_limits<double>::quiet_NaN();
    int lastRenderWidth = 0, lastRenderHeight = 0;
    uint64_t lastSceneRevision = 0;

    while (!glfwWindowShouldClose(gWindow)) {
        double now = glfwGetTime();
        double deltaTime = now - lastTime;
        lastTime = now;

        if (gProjectDirty && now - lastAutosaveTime >= kAutosaveIntervalSeconds) {
            if (SaveProjectToPath(kAutosaveFile, false))
                LOG_INFO("Autosaved recovery copy to %s", kAutosaveFile);
            lastAutosaveTime = now;
        }

        UpdateCameraPan(deltaTime);
        UpdateWindowTitle();

        bool staticSceneChanged = gCameraX != lastRenderCameraX || gCameraY != lastRenderCameraY ||
                                  gZoom != lastRenderZoom || gWindowWidth != lastRenderWidth ||
                                  gWindowHeight != lastRenderHeight || gSceneRevision != lastSceneRevision;
        if (staticSceneChanged) {
            RebuildVisibleTileMesh(tileMesh.Buffer(), tileMesh.VertexCount());
            RebuildVisibleRegionOverlay(regionMesh.Buffer(), regionMesh.VertexCount());
            RebuildElevationContours(contourMesh.Buffer(), contourMesh.VertexCount());
            RebuildVisibleFogOverlay(fogMesh.Buffer(), fogMesh.VertexCount());
            RebuildVisibleGridLines(gridMesh.Buffer(), gridMesh.VertexCount());
            RebuildCityMarkers(cityMesh.Buffer(), cityMesh.VertexCount());
            RebuildPoiMarkers(poiMesh.Buffer(), poiMesh.VertexCount());
            RebuildRouteMesh(routeMesh.Buffer(), routeMesh.VertexCount());
            RebuildLabelMesh(labelMesh.Buffer(), labelMesh.VertexCount());
            lastRenderCameraX = gCameraX;
            lastRenderCameraY = gCameraY;
            lastRenderZoom = gZoom;
            lastRenderWidth = gWindowWidth;
            lastRenderHeight = gWindowHeight;
            lastSceneRevision = gSceneRevision;
        }
        RebuildRectPreview(selectionMesh.Buffer(), selectionMesh.VertexCount());
        RebuildToolPreview(toolMesh.Buffer(), toolMesh.VertexCount());
        RebuildGuiMesh(guiMesh.Buffer(), guiMesh.VertexCount());

        glClearColor(0.05f, 0.05f, 0.06f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(shaderProgram);
        glUniform2f(resolutionLoc, static_cast<float>(gWindowWidth), static_cast<float>(gWindowHeight));

        tileMesh.Draw(GL_TRIANGLES);
        if (gShowRegions && !gElevationView) regionMesh.Draw(GL_TRIANGLES);
        if (gShowGrid) gridMesh.Draw(GL_LINES);
        contourMesh.Draw(GL_LINES);
        routeMesh.Draw(GL_TRIANGLES);
        cityMesh.Draw(GL_TRIANGLES);
        poiMesh.Draw(GL_TRIANGLES);
        if (gShowLabels) labelMesh.Draw(GL_TRIANGLES);
        fogMesh.Draw(GL_TRIANGLES);
        selectionMesh.Draw(GL_LINES);
        toolMesh.Draw(GL_LINES);
        guiMesh.Draw(GL_TRIANGLES);

        glfwSwapBuffers(gWindow);
        glfwPollEvents();
    }

    if (gProjectDirty && SaveProjectToPath(kAutosaveFile, false))
        LOG_INFO("Saved final recovery copy to %s", kAutosaveFile);

    for (MeshBuffer *mesh : meshes) mesh->Release();
    glDeleteProgram(shaderProgram);

    glfwDestroyWindow(gWindow);
    glfwTerminate();

    LOG_INFO("Shutting down");
    LoggerShutdown();
    return EXIT_SUCCESS;
}
