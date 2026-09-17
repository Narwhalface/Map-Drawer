// DND Overworld Map Drawer
// An infinite, pannable/zoomable tile-based map painter: pick a terrain brush with
// number keys, paint tiles with the mouse, pan with WASD/arrows, zoom with the
// scroll wheel, and save/load the world as one versioned project file.
#include "app_config.h"
#include "app_types.h"
#include "campaign_tools.h"
#include "editor_commands.h"
#include "encounter_builder.h"
#include "editor_history.h"
#include "editor_state.h"
#include "logger.h"
#include "grid_geometry.h"
#include "mesh_buffer.h"
#include "project_document.h"
#include "render_geometry.h"
#include "shader_program.h"
#include "tiny_font.h"
#include "ui_geometry.h"

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <commdlg.h>

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
constexpr int kTerrainPageSize = 9;

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
void RequestProjectSaveAs();
bool LoadProjectFromPath(const std::string &path, bool allowLegacyFallback);
bool RequestProjectLoad();
void RequestClearActiveLayer();
void ClearActiveLayerNow();
void SaveProjectConfig();
void LoadProjectConfig();
void CheckForRecoveryAutosave();
void GenerateTerrainRelief();
void OpenKeybindHelp();
void MarkProjectDirty();
void StartStandaloneEncounter();
void StartStandaloneCreature();
void OpenModal(ModalType type, std::vector<std::string> fields);
std::string ChooseProjectFileToOpen(const char *title);
std::string ChooseProjectFileToSave(const char *title, const std::string &suggestedName);

// Persisted world data lives together; aliases keep the editing code concise while
// making the ownership boundary explicit for save/load operations.
EditorState gEditor;
auto &gProjectDocument = gEditor.document;
auto &gTerrainDefinitions = gProjectDocument.terrainDefinitions;
auto &gMapData = gProjectDocument.terrain;
auto &gRegionData = gProjectDocument.regionsByTile;
auto &gElevationData = gProjectDocument.elevation;
auto &gFogData = gProjectDocument.fog;
auto &gRegions = gProjectDocument.regions;
auto &gCities = gProjectDocument.cities;
auto &gPois = gProjectDocument.pointsOfInterest;
auto &gEncounters = gProjectDocument.encounters;
auto &gDungeons = gProjectDocument.dungeons;
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
int gTerrainPage = 0;
int gEditingTerrainIndex = 0;
bool gCreatingTerrain = false;
int gElevationBrush = 1;
ElevationEditMode gElevationEditMode = ElevationEditMode::Set;
bool gFlattenHeightCaptured = false;
int gFlattenHeight = 0;
int gBrushRadius = 0; // 0 = single tile, N = (2N+1)x(2N+1) square
bool gRoundBrush = false; // false = square stamp, true = circular stamp (more organic edges)
bool gShowGrid = true;
bool gPlayerView = false;
EditorTab gEditorTab = EditorTab::World;
int gActiveDungeonIndex = -1;
int gDungeonManagerIndex = -1;
int gSelectedDungeonPoiIndex = -1;
int gEditingDungeonIndex = -1;
int gDungeonBrush = static_cast<int>(DungeonTileKind::Floor);
DungeonPlacementMode gDungeonPlacementMode = DungeonPlacementMode::None;
DungeonMarkerKind gDungeonMarkerKind = DungeonMarkerKind::Note;
bool gEditingDungeonTerrain = false;
double gWorldCameraX = -kUiWidth;
double gWorldCameraY = 0.0;
double gWorldZoom = 1.0;
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
std::string gModalError;
std::string gInfoTitle;
std::vector<std::string> gInfoLines;
ConfirmAction gConfirmAction = ConfirmAction::None;
std::string gProjectFile = kDefaultProjectFile;
std::string gPendingLoadFile;
bool &gProjectDirty = gEditor.dirty;
int gModalField = 0;
int32_t gModalCol = 0, gModalRow = 0;
PoiKind gModalPoiKind = PoiKind::Landmark;
RouteKind gModalRouteKind = RouteKind::River;
int gEditingEncounterIndex = -1;
double gLastCanvasWorldX = 0.0, gLastCanvasWorldY = 0.0;
bool gUseUiTarget = false;
uint64_t &gSceneRevision = gEditor.sceneRevision;
bool gMainMenuOpen = true;
bool gEditorSessionStarted = false;
std::unordered_map<std::string, std::string> gEncounterCreatureSources;
struct EncounterTableRowDraft {
    std::string minimumRoll = "1";
    std::string maximumRoll = "1";
    std::string result;
};
struct EncounterTableDraft {
    std::string name = "New Table";
    std::string dieSides = "6";
    std::vector<EncounterTableRowDraft> rows;
};
std::vector<EncounterTableDraft> gEncounterTableDrafts;
int gEncounterActiveTable = 0;
int gEncounterTableRowPage = 0;
int gEncounterSelectedTableRow = -1;
Encounter gEncounterWorkingDraft;
std::string gEncounterRollResult;
int gRunningEncounterIndex = -1;

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
auto &gHistory = gEditor.history;

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
auto &gLastSearchQuery = gEditor.lastSearchQuery;
auto &gLastFoundLabel = gEditor.lastFoundLabel;
auto &gSearchMatches = gEditor.searchMatches;
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

std::string ChooseProjectFileToOpen(const char *title) {
    wchar_t filePath[32768]{};
    wchar_t wideTitle[128]{};
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wideTitle,
                        static_cast<int>(std::size(wideTitle)));
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = glfwGetWin32Window(gWindow);
    dialog.lpstrFilter = L"Map Drawer files (*.txt)\0*.txt\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = filePath;
    dialog.nMaxFile = static_cast<DWORD>(std::size(filePath));
    dialog.lpstrTitle = wideTitle;
    dialog.lpstrDefExt = L"txt";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return {};
    return std::filesystem::path(filePath).string();
}

std::string ChooseProjectFileToSave(const char *title, const std::string &suggestedName) {
    wchar_t filePath[32768]{};
    std::filesystem::path suggested(suggestedName);
    std::wstring wideSuggested = suggested.filename().wstring();
    std::copy_n(wideSuggested.c_str(),
                std::min(wideSuggested.size(), std::size(filePath) - 1), filePath);
    wchar_t wideTitle[128]{};
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wideTitle,
                        static_cast<int>(std::size(wideTitle)));
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = glfwGetWin32Window(gWindow);
    dialog.lpstrFilter = L"Map Drawer files (*.txt)\0*.txt\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = filePath;
    dialog.nMaxFile = static_cast<DWORD>(std::size(filePath));
    dialog.lpstrTitle = wideTitle;
    dialog.lpstrDefExt = L"txt";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog)) return {};
    return std::filesystem::path(filePath).string();
}

uint64_t TileKey(int32_t col, int32_t row) {
    return grid_geometry::Pack(col, row);
}

std::pair<double, double> TileCenterWorld(int32_t col, int32_t row) {
    return grid_geometry::TileCenter(col, row, gEditorTab == EditorTab::Dungeon ? false : gHexGrid);
}

std::pair<int32_t, int32_t> TileCoords(uint64_t key) {
    return grid_geometry::Unpack(key);
}

std::pair<int32_t, int32_t> WorldToTile(double worldX, double worldY) {
    return grid_geometry::WorldToTile(worldX, worldY,
                                      gEditorTab == EditorTab::Dungeon ? false : gHexGrid);
}

double MeasuredTileDistance() {
    return grid_geometry::TileDistance(gMeasureStartCol, gMeasureStartRow,
                                       gMeasureEndCol, gMeasureEndRow, gHexGrid);
}

void TileBoundsForWorldRect(double minX, double minY, double maxX, double maxY,
                            int32_t &minCol, int32_t &minRow, int32_t &maxCol, int32_t &maxRow) {
    grid_geometry::BoundsForWorldRect(minX, minY, maxX, maxY,
                                      gEditorTab == EditorTab::Dungeon ? false : gHexGrid,
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

Dungeon *ActiveDungeon() {
    if (gActiveDungeonIndex < 0 || gActiveDungeonIndex >= static_cast<int>(gDungeons.size()))
        return nullptr;
    return &gDungeons[static_cast<size_t>(gActiveDungeonIndex)];
}

std::pair<int32_t, int32_t> DungeonWorldToTile(double worldX, double worldY) {
    return grid_geometry::WorldToTile(worldX, worldY, false);
}

std::pair<int32_t, int32_t> DungeonCursorTile() {
    auto [worldX, worldY] = CursorWorld();
    return DungeonWorldToTile(worldX, worldY);
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
    return grid_geometry::Neighbors(col, row,
                                    gEditorTab == EditorTab::Dungeon ? false : gHexGrid, count);
}

void FitDungeonToWindow() {
    Dungeon *dungeon = ActiveDungeon();
    if (!dungeon) return;
    bool haveBounds = false;
    int32_t minCol = 0, minRow = 0, maxCol = 0, maxRow = 0;
    auto includeTile = [&](int32_t col, int32_t row) {
        if (!haveBounds) {
            minCol = maxCol = col;
            minRow = maxRow = row;
            haveBounds = true;
        } else {
            minCol = std::min(minCol, col);
            minRow = std::min(minRow, row);
            maxCol = std::max(maxCol, col);
            maxRow = std::max(maxRow, row);
        }
    };
    for (const auto &[key, value] : dungeon->tiles) {
        (void)value;
        auto [col, row] = TileCoords(key);
        includeTile(col, row);
    }
    for (const auto &[key, value] : dungeon->elevation) {
        (void)value;
        auto [col, row] = TileCoords(key);
        includeTile(col, row);
    }
    for (const auto &[key, value] : dungeon->fog) {
        (void)value;
        auto [col, row] = TileCoords(key);
        includeTile(col, row);
    }
    if (dungeon->hasEntrance) includeTile(dungeon->entranceCol, dungeon->entranceRow);
    if (dungeon->hasExit) includeTile(dungeon->exitCol, dungeon->exitRow);
    if (!haveBounds) {
        includeTile(0, 0);
        includeTile(12, 8);
    }
    double worldWidth = std::max(static_cast<double>(kTileSize),
                                 static_cast<double>(maxCol - minCol + 1) * kTileSize);
    double worldHeight = std::max(static_cast<double>(kTileSize),
                                  static_cast<double>(maxRow - minRow + 1) * kTileSize);
    double canvasWidth = std::max(100.0, static_cast<double>(gWindowWidth) - kUiWidth - 60.0);
    double canvasHeight = std::max(100.0, static_cast<double>(gWindowHeight) - 60.0);
    gZoom = std::clamp(std::min(canvasWidth / worldWidth, canvasHeight / worldHeight),
                       kMinZoom, kMaxZoom);
    double centerX = (static_cast<double>(minCol + maxCol + 1) * kTileSize) * 0.5;
    double centerY = (static_cast<double>(minRow + maxRow + 1) * kTileSize) * 0.5;
    double screenCenterX = kUiWidth + (gWindowWidth - kUiWidth) * 0.5;
    gCameraX = centerX - screenCenterX / gZoom;
    gCameraY = centerY - (gWindowHeight * 0.5) / gZoom;
}

void OpenDungeonTab(int index) {
    if (index < 0 || index >= static_cast<int>(gDungeons.size())) return;
    if (gEditorTab == EditorTab::World) {
        gWorldCameraX = gCameraX;
        gWorldCameraY = gCameraY;
        gWorldZoom = gZoom;
    }
    gEditorTab = EditorTab::Dungeon;
    gActiveDungeonIndex = index;
    gDungeonPlacementMode = DungeonPlacementMode::None;
    gPaintMode = PaintMode::Terrain;
    gToolMode = ToolMode::Brush;
    gDungeonBrush = std::clamp(gDungeonBrush, 1,
                               static_cast<int>(gDungeons[static_cast<size_t>(index)]
                                                    .terrainDefinitions.size()) - 1);
    gTerrainPage = gDungeonBrush / kTerrainPageSize;
    gPaintingLeft = false;
    gPaintingRight = false;
    gMeasureStage = 0;
    gLastFoundLabel.clear();
    gHistory.Clear();
    FitDungeonToWindow();
    ++gSceneRevision;
    UpdateWindowTitle();
}

void StartStandaloneDungeon() {
    gProjectDocument = ProjectDocument{};
    gProjectDocument.standaloneDungeon = true;
    Dungeon dungeon;
    dungeon.name = "Untitled Dungeon";
    gDungeons.push_back(std::move(dungeon));
    gProjectFile.clear();
    gEditor.ResetForLoadedDocument();
    gEditorTab = EditorTab::World;
    gActiveDungeonIndex = -1;
    gSelectedDungeonPoiIndex = -1;
    gDungeonManagerIndex = -1;
    gPlacementMode = PlacementMode::None;
    OpenDungeonTab(0);
    MarkProjectDirty();
    LOG_INFO("Created a new standalone dungeon map");
}

void StartWorldDocument() {
    gProjectDocument = ProjectDocument{};
    gProjectFile = kDefaultProjectFile;
    gEditor.ResetForLoadedDocument();
    gEditorTab = EditorTab::World;
    gActiveDungeonIndex = -1;
    gSelectedDungeonPoiIndex = -1;
    gDungeonManagerIndex = -1;
    gPlacementMode = PlacementMode::None;
    gCameraX = -kUiWidth;
    gCameraY = 0.0;
    gZoom = 1.0;
    SaveProjectConfig();
    ++gSceneRevision;
    UpdateWindowTitle();
}

void ReturnToWorldTab() {
    if (gEditorTab == EditorTab::World) return;
    if (gProjectDocument.standaloneDungeon) {
        gMainMenuOpen = true;
        UpdateWindowTitle();
        return;
    }
    gEditorTab = EditorTab::World;
    gDungeonPlacementMode = DungeonPlacementMode::None;
    gHistory.Clear();
    gCameraX = gWorldCameraX;
    gCameraY = gWorldCameraY;
    gZoom = gWorldZoom;
    ++gSceneRevision;
    UpdateWindowTitle();
}

int GetLayerValue(PaintMode layer, uint64_t key) {
    Dungeon *dungeon = gEditorTab == EditorTab::Dungeon ? ActiveDungeon() : nullptr;
    return editor_commands::GetLayerValue(
        {gProjectDocument, dungeon, gEditorTab == EditorTab::Dungeon}, layer, key);
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
    gEditor.MarkDirty();
}

void PlaceDungeonSpecialAtCursor() {
    Dungeon *dungeon = ActiveDungeon();
    if (!dungeon || gDungeonPlacementMode == DungeonPlacementMode::None) return;
    auto [col, row] = DungeonCursorTile();
    if (gDungeonPlacementMode == DungeonPlacementMode::Marker) {
        gModalCol = col;
        gModalRow = row;
        gDungeonMarkerKind = DungeonMarkerKind::Note;
        gDungeonPlacementMode = DungeonPlacementMode::None;
        OpenModal(ModalType::DungeonMarker, {"", ""});
        return;
    }
    if (gDungeonPlacementMode == DungeonPlacementMode::Encounter) {
        std::string path = ChooseProjectFileToOpen("Select an encounter for this dungeon tile");
        if (!path.empty()) {
            ProjectDocument source;
            int version = 0;
            std::string error;
            if (LoadProjectDocument(path, source, version, error) && source.standaloneEncounter &&
                source.encounters.size() == 1) {
                campaign_tools::AddDungeonMarker(
                    *dungeon, {col, row, DungeonMarkerKind::Encounter,
                               source.encounters.front().name,
                               source.encounters.front().description, path, true});
                MarkProjectDirty();
                ++gSceneRevision;
            } else {
                LOG_ERROR("Could not add dungeon encounter: %s",
                          error.empty() ? "not a standalone encounter" : error.c_str());
            }
        }
        gDungeonPlacementMode = DungeonPlacementMode::None;
        return;
    }
    if (dungeon->tiles.count(TileKey(col, row)) == 0)
        dungeon->tiles[TileKey(col, row)] = static_cast<uint8_t>(std::max(1, gDungeonBrush));
    if (gDungeonPlacementMode == DungeonPlacementMode::Entrance) {
        dungeon->hasEntrance = true;
        dungeon->entranceCol = col;
        dungeon->entranceRow = row;
        LOG_INFO("Dungeon entrance placed at (%d, %d)", col, row);
    } else if (gDungeonPlacementMode == DungeonPlacementMode::Exit) {
        dungeon->hasExit = true;
        dungeon->exitCol = col;
        dungeon->exitRow = row;
        LOG_INFO("Dungeon exit placed at (%d, %d)", col, row);
    }
    gDungeonPlacementMode = DungeonPlacementMode::None;
    ++gSceneRevision;
    MarkProjectDirty();
}

void SetLayerValue(PaintMode layer, uint64_t key, int value) {
    Dungeon *dungeon = gEditorTab == EditorTab::Dungeon ? ActiveDungeon() : nullptr;
    editor_commands::SetLayerValue(
        {gProjectDocument, dungeon, gEditorTab == EditorTab::Dungeon}, layer, key, value);
}

// Returns the value the active tool should paint with, or -1 if painting isn't possible
// right now (e.g. region mode with no active region selected).
int ActivePaintValue() {
    if (gPaintMode == PaintMode::Terrain)
        return gEditorTab == EditorTab::Dungeon ? gDungeonBrush : gBrush;
    if (gPaintMode == PaintMode::Elevation)
        return gElevationEditMode == ElevationEditMode::Set ? gElevationBrush : 1;
    if (gPaintMode == PaintMode::Fog) return 1;
    if (gEditorTab == EditorTab::Dungeon) return kNoPaintValue;
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
    gHistory.BeginStroke(gPaintMode, expectedTiles);
    gFlattenHeightCaptured = false;
    expectedTiles = std::min(expectedTiles, kBulkReserveLimit);
    if (expectedTiles > 0) {
        if (paintValue != 0) {
            Dungeon *dungeon = ActiveDungeon();
            editor_commands::ReserveLayer(
                {gProjectDocument, dungeon, gEditorTab == EditorTab::Dungeon},
                gHistory.StrokeLayer(), expectedTiles);
        }
    }
}

// Sets (or, for value 0, erases) a tile in the active layer, recording its pre-stroke value.
void SetTileRecorded(int32_t col, int32_t row, int value) {
    uint64_t key = TileKey(col, row);
    PaintMode layer = gHistory.StrokeActive() ? gHistory.StrokeLayer() : gPaintMode;
    if (layer == PaintMode::Elevation && value != 0 && gElevationEditMode != ElevationEditMode::Set) {
        if (gHistory.StrokeActive() && gHistory.HasRecorded(key)) return;
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
    Dungeon *dungeon = ActiveDungeon();
    EditTarget target{gProjectDocument, dungeon, gEditorTab == EditorTab::Dungeon};
    int oldValue = editor_commands::GetLayerValue(target, layer, key);
    bool changed = editor_commands::SetLayerValue(target, layer, key, value);
    if (changed) {
        if (gHistory.StrokeActive()) gHistory.RecordOriginal(key, oldValue);
        MarkProjectDirty();
        if (!gHistory.StrokeActive()) ++gSceneRevision;
    }
}

void EndStroke() {
    if (!gHistory.EndStroke(GetLayerValue)) return;
    ++gSceneRevision;
    MarkProjectDirty();
}

void Undo() {
    auto result = gHistory.Undo(SetLayerValue);
    if (!result) return;
    ++gSceneRevision;
    MarkProjectDirty();
    LOG_INFO("Undo (%zu tiles, %s layer)", result->tileCount,
             PaintModeName(result->layer));
}

void Redo() {
    auto result = gHistory.Redo(SetLayerValue);
    if (!result) return;
    ++gSceneRevision;
    MarkProjectDirty();
    LOG_INFO("Redo (%zu tiles, %s layer)", result->tileCount,
             PaintModeName(result->layer));
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
        bool hexGrid = gEditorTab == EditorTab::World && gHexGrid;
        int neighborCount = hexGrid ? 6 : 4;
        for (int i = 0; i < neighborCount; ++i) {
            int32_t nc = c + (hexGrid ? hexDc[i] : squareDc[i]);
            int32_t nr = r + (hexGrid ? ((c % 2 != 0) ? oddHexDr[i] : evenHexDr[i]) : squareDr[i]);
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
    if (gEditorTab == EditorTab::World && gHexGrid) {
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
        bool hexGrid = gEditorTab == EditorTab::World && gHexGrid;
        int parityPasses = hexGrid ? 2 : 1;
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
                if (hexGrid && ((col % 2 != 0) ? 1 : 0) != parity) continue;
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
    gModalError.clear();
}

int FindDungeonAtWorldTile(int32_t col, int32_t row) {
    auto it = std::find_if(gDungeons.begin(), gDungeons.end(), [&](const Dungeon &dungeon) {
        return dungeon.worldCol == col && dungeon.worldRow == row;
    });
    return it == gDungeons.end() ? -1 : static_cast<int>(it - gDungeons.begin());
}

int FindDungeonPoiAtWorldTile(int32_t col, int32_t row) {
    auto it = std::find_if(gPois.begin(), gPois.end(), [&](const PointOfInterest &poi) {
        return poi.kind == PoiKind::Dungeon && poi.col == col && poi.row == row;
    });
    return it == gPois.end() ? -1 : static_cast<int>(it - gPois.begin());
}

void OpenDungeonManagerForPoi(int poiIndex) {
    if (poiIndex < 0 || poiIndex >= static_cast<int>(gPois.size()) ||
        gPois[static_cast<size_t>(poiIndex)].kind != PoiKind::Dungeon)
        return;
    const PointOfInterest &poi = gPois[static_cast<size_t>(poiIndex)];
    gSelectedDungeonPoiIndex = poiIndex;
    gModalCol = poi.col;
    gModalRow = poi.row;
    gDungeonManagerIndex = FindDungeonAtWorldTile(poi.col, poi.row);
    OpenModal(ModalType::DungeonManager, {});
}

void OpenDungeonManager() {
    if (gEditorTab != EditorTab::World || gSelectedDungeonPoiIndex < 0 ||
        gSelectedDungeonPoiIndex >= static_cast<int>(gPois.size()) ||
        gPois[static_cast<size_t>(gSelectedDungeonPoiIndex)].kind != PoiKind::Dungeon) {
        gInfoTitle = "SELECT A DUNGEON";
        gInfoLines = {"CLICK A DUNGEON POI ON THE OVERWORLD FIRST",
                      "THEN CREATE A MAP OR LINK A STANDALONE DUNGEON FILE"};
        OpenModal(ModalType::Info, {});
        return;
    }
    OpenDungeonManagerForPoi(gSelectedDungeonPoiIndex);
}

void LinkDungeonFileToSelectedPoi() {
    if (gSelectedDungeonPoiIndex < 0 ||
        gSelectedDungeonPoiIndex >= static_cast<int>(gPois.size()))
        return;
    std::string path = ChooseProjectFileToOpen("Select a dungeon map");
    if (path.empty()) return;

    ProjectDocument source;
    int version = 0;
    std::string error;
    if (!LoadProjectDocument(path, source, version, error) || !source.standaloneDungeon ||
        source.dungeons.size() != 1) {
        LOG_ERROR("Could not link dungeon file %s: %s", path.c_str(),
                  error.empty() ? "the file is not a standalone dungeon map" : error.c_str());
        gInfoTitle = "CANNOT LINK DUNGEON";
        gInfoLines = {error.empty() ? "SELECT A STANDALONE DUNGEON MAP FILE"
                                    : error};
        OpenModal(ModalType::Info, {});
        return;
    }

    const PointOfInterest &poi = gPois[static_cast<size_t>(gSelectedDungeonPoiIndex)];
    Dungeon linked = source.dungeons.front();
    linked.worldCol = poi.col;
    linked.worldRow = poi.row;
    linked.sourceFile = path;
    int dungeonIndex = FindDungeonAtWorldTile(poi.col, poi.row);
    if (dungeonIndex >= 0)
        gDungeons[static_cast<size_t>(dungeonIndex)] = std::move(linked);
    else {
        gDungeons.push_back(std::move(linked));
        dungeonIndex = static_cast<int>(gDungeons.size()) - 1;
    }
    gPois[static_cast<size_t>(gSelectedDungeonPoiIndex)].name =
        gDungeons[static_cast<size_t>(dungeonIndex)].name;
    gPois[static_cast<size_t>(gSelectedDungeonPoiIndex)].description =
        gDungeons[static_cast<size_t>(dungeonIndex)].description;
    gDungeonManagerIndex = dungeonIndex;
    gModalType = ModalType::None;
    ++gSceneRevision;
    MarkProjectDirty();
    LOG_INFO("Linked dungeon '%s' from %s",
             gDungeons[static_cast<size_t>(dungeonIndex)].name.c_str(), path.c_str());
    OpenDungeonTab(dungeonIndex);
}

void OpenNewDungeonForm() {
    if (gSelectedDungeonPoiIndex < 0 ||
        gSelectedDungeonPoiIndex >= static_cast<int>(gPois.size()))
        return;
    const PointOfInterest &poi = gPois[static_cast<size_t>(gSelectedDungeonPoiIndex)];
    gModalCol = poi.col;
    gModalRow = poi.row;
    gEditingDungeonIndex = -1;
    OpenModal(ModalType::DungeonDetails, {poi.name, poi.description});
}

void OpenDungeonDetailsForm() {
    Dungeon *dungeon = ActiveDungeon();
    if (!dungeon) return;
    gModalCol = dungeon->worldCol;
    gModalRow = dungeon->worldRow;
    gSelectedDungeonPoiIndex =
        FindDungeonPoiAtWorldTile(dungeon->worldCol, dungeon->worldRow);
    gEditingDungeonIndex = gActiveDungeonIndex;
    OpenModal(ModalType::DungeonDetails, {dungeon->name, dungeon->description});
}

int TerrainColorByte(float value) {
    return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

std::vector<TerrainDefinition> &ActiveTerrainDefinitions() {
    Dungeon *dungeon = ActiveDungeon();
    if (gEditorTab == EditorTab::Dungeon && dungeon) return dungeon->terrainDefinitions;
    return gTerrainDefinitions;
}

std::vector<TerrainDefinition> &EditedTerrainDefinitions() {
    if (gEditingDungeonTerrain) {
        Dungeon *dungeon = ActiveDungeon();
        if (dungeon) return dungeon->terrainDefinitions;
    }
    return gTerrainDefinitions;
}

void LoadTerrainEditorFields(int terrainIndex) {
    std::vector<TerrainDefinition> &definitions = EditedTerrainDefinitions();
    if (definitions.empty())
        definitions = gEditingDungeonTerrain ? DefaultDungeonTerrainDefinitions()
                                             : DefaultTerrainDefinitions();
    int minimumIndex = gEditingDungeonTerrain ? 1 : 0;
    gEditingTerrainIndex = std::clamp(terrainIndex, 0,
                                     static_cast<int>(definitions.size()) - 1);
    gEditingTerrainIndex = std::max(minimumIndex, gEditingTerrainIndex);
    const TerrainDefinition &terrain = definitions[static_cast<size_t>(gEditingTerrainIndex)];
    gModalFields = {terrain.name, std::to_string(TerrainColorByte(terrain.color.r)),
                    std::to_string(TerrainColorByte(terrain.color.g)),
                    std::to_string(TerrainColorByte(terrain.color.b))};
    gModalField = 0;
    gCreatingTerrain = false;
    gModalError.clear();
}

void OpenTerrainEditor() {
    gEditingDungeonTerrain = gEditorTab == EditorTab::Dungeon;
    OpenModal(ModalType::TerrainEditor, {});
    LoadTerrainEditorFields(gEditingDungeonTerrain ? gDungeonBrush : gBrush);
}

void StartNewTerrain() {
    std::vector<TerrainDefinition> &definitions = EditedTerrainDefinitions();
    if (definitions.size() >= kMaxTerrainTypes) {
        LOG_WARN("Terrain limit reached (%d)", kMaxTerrainTypes);
        return;
    }
    gEditingTerrainIndex = static_cast<int>(definitions.size());
    gModalFields = {"New Terrain", "128", "128", "128"};
    gModalField = 0;
    gCreatingTerrain = true;
    gModalError.clear();
}

bool ParseColorChannel(const std::string &text, int &value) {
    if (text.empty()) return false;
    char *end = nullptr;
    long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || parsed < 0 || parsed > 255) return false;
    value = static_cast<int>(parsed);
    return true;
}

void OpenConfirmation(ConfirmAction action, const std::string &title,
                      std::vector<std::string> lines) {
    gConfirmAction = action;
    gInfoTitle = title;
    gInfoLines = std::move(lines);
    OpenModal(ModalType::Confirm, {});
}

void OpenKeybindHelp() {
    if (gEditorTab == EditorTab::Dungeon) {
        gInfoTitle = "DUNGEON MAPPER HELP";
        gInfoLines = {
            "DUNGEON TAB",
            "LEFT / RIGHT DRAG    PAINT / ERASE THE ACTIVE LAYER",
            "SHIFT + DRAG         FILL OR ERASE A RECTANGLE",
            "MIDDLE DRAG          PAN THE DUNGEON MAP",
            "MOUSE WHEEL          ZOOM AROUND THE CURSOR",
            "F1-F7                BRUSH / FILL / LINE / CURVE / POLYGON / CIRCLE / SCATTER",
            "T                    CYCLE TERRAIN / ELEVATION / FOG",
            "[ / ] AND H          BRUSH SIZE AND ROUND / SQUARE SHAPE",
            "Q / E                LOWER / RAISE THE ELEVATION BRUSH",
            "E (NON-HEIGHT)       PLACE OR MOVE THE ENTRANCE",
            "X                    PLACE OR MOVE THE EXIT",
            "F11                  PREVIEW DUNGEON FOG AS A PLAYER",
            "CTRL+Z / CTRL+Y      UNDO / REDO DUNGEON PAINTING",
            "D                    EDIT DUNGEON NAME AND DESCRIPTION",
            "HOME                 FIT THE DUNGEON MAP",
            "G                    TOGGLE THE SQUARE GRID",
            "CTRL+S / CTRL+L      SAVE / LOAD THE PROJECT",
            gProjectDocument.standaloneDungeon
                ? "ESC                  RETURN TO THE MAIN MENU"
                : "ESC                  RETURN TO THE WORLD TAB",
            "ENTRANCE AND EXIT TOOLS ONLY EXIST IN THE DUNGEON TAB",
        };
        OpenModal(ModalType::Info, {});
        return;
    }
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
        "0-9               SELECT THE FIRST TEN TERRAIN TYPES",
        "TERRAIN < >       CHANGE PALETTE PAGE   EDIT / NEW MANAGE TYPES",
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
        "CLICK DUNGEON POI TO CREATE OR OPEN ITS DUNGEON MAP",
        "CTRL+F FIND   CTRL+E PLACE ENCOUNTER   I WORLD INFO",
        "ENCOUNTERS CAN ALSO BE CREATED AS STANDALONE FILES FROM THE MAIN MENU",
        "DELETE MARKER/SELECTION   X DELETE ROUTE",
        "CTRL+C/X/V COPY / CUT / PASTE SELECTION",
        "CTRL+Z/Y UNDO / REDO   C CLEAR ACTIVE LAYER",
        "CTRL+S/L SAVE / LOAD   P EXPORT   ESC CLOSE / MENU",
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

void LoadEncounterTableDrafts(const Encounter &encounter) {
    gEncounterTableDrafts.clear();
    for (const EncounterRollTable &table : encounter.rollTables) {
        EncounterTableDraft draft;
        draft.name = table.name;
        draft.dieSides = std::to_string(table.dieSides);
        draft.rows.clear();
        for (const EncounterTableEntry &entry : table.entries)
            draft.rows.push_back({std::to_string(entry.minimumRoll),
                                  std::to_string(entry.maximumRoll), entry.result});
        gEncounterTableDrafts.push_back(std::move(draft));
    }
    gEncounterActiveTable = 0;
    gEncounterTableRowPage = 0;
    gEncounterSelectedTableRow = -1;
}

std::string *EncounterModalText(int field) {
    if (field >= 0 && field < static_cast<int>(gModalFields.size()))
        return &gModalFields[static_cast<size_t>(field)];
    if (gEncounterTableDrafts.empty() || gEncounterActiveTable < 0 ||
        gEncounterActiveTable >= static_cast<int>(gEncounterTableDrafts.size()))
        return nullptr;
    EncounterTableDraft &table = gEncounterTableDrafts[static_cast<size_t>(gEncounterActiveTable)];
    if (field == 100) return &table.name;
    if (field == 101) return &table.dieSides;
    if (field < 200) return nullptr;
    int encoded = field - 200;
    int row = encoded / 3;
    int column = encoded % 3;
    if (row < 0 || row >= static_cast<int>(table.rows.size())) return nullptr;
    EncounterTableRowDraft &draft = table.rows[static_cast<size_t>(row)];
    if (column == 0) return &draft.minimumRoll;
    if (column == 1) return &draft.maximumRoll;
    return &draft.result;
}

std::vector<int> EncounterEditableFields() {
    std::vector<int> fields;
    for (int index = 0; index < static_cast<int>(gModalFields.size()); ++index)
        fields.push_back(index);
    if (gEncounterTableDrafts.empty() || gEncounterActiveTable < 0 ||
        gEncounterActiveTable >= static_cast<int>(gEncounterTableDrafts.size()))
        return fields;
    fields.push_back(100);
    fields.push_back(101);
    const EncounterTableDraft &table =
        gEncounterTableDrafts[static_cast<size_t>(gEncounterActiveTable)];
    constexpr int rowsPerPage = 5;
    int firstRow = gEncounterTableRowPage * rowsPerPage;
    int lastRow = std::min(firstRow + rowsPerPage, static_cast<int>(table.rows.size()));
    for (int row = firstRow; row < lastRow; ++row)
        for (int column = 0; column < 3; ++column)
            fields.push_back(200 + row * 3 + column);
    return fields;
}

bool BuildEncounterTablesFromDrafts(Encounter &encounter, std::string &error) {
    encounter.rollTables.clear();
    auto parseNumber = [](const std::string &text, int &value) {
        if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char c) {
                return std::isdigit(c) != 0;
            }))
            return false;
        try { value = std::stoi(text); } catch (...) { return false; }
        return value > 0;
    };
    for (const EncounterTableDraft &draft : gEncounterTableDrafts) {
        EncounterRollTable table;
        table.name = draft.name;
        if (table.name.empty() || !parseNumber(draft.dieSides, table.dieSides) ||
            table.dieSides > 1000) {
            error = "EACH TABLE NEEDS A NAME AND A VALID DIE SIZE";
            return false;
        }
        if (draft.rows.empty()) {
            error = "EACH TABLE NEEDS AT LEAST ONE ROW";
            return false;
        }
        for (const EncounterTableRowDraft &row : draft.rows) {
            EncounterTableEntry entry;
            if (!parseNumber(row.minimumRoll, entry.minimumRoll) ||
                !parseNumber(row.maximumRoll, entry.maximumRoll) ||
                entry.minimumRoll > entry.maximumRoll || entry.maximumRoll > table.dieSides ||
                row.result.empty()) {
                error = "TABLE ROWS NEED VALID RANGES AND RESULTS";
                return false;
            }
            entry.result = row.result;
            table.entries.push_back(std::move(entry));
        }
        std::sort(table.entries.begin(), table.entries.end(), [](const auto &left, const auto &right) {
            return left.minimumRoll < right.minimumRoll;
        });
        for (size_t index = 1; index < table.entries.size(); ++index) {
            if (table.entries[index].minimumRoll <= table.entries[index - 1].maximumRoll) {
                error = "TABLE ROLL RANGES CANNOT OVERLAP";
                return false;
            }
        }
        encounter.rollTables.push_back(std::move(table));
    }
    return true;
}

// Opens an in-window form for a persistent DM encounter at the cursor.
void PlaceEncounterAtCursor() {
    std::tie(gModalCol, gModalRow) = CursorTile();
    gEditingEncounterIndex = -1;
    gEncounterCreatureSources.clear();
    gEncounterWorkingDraft = Encounter{};
    LoadEncounterTableDrafts(Encounter{});
    OpenModal(ModalType::Encounter, {"", "", "", "", "", "", "", "", "", "", "", ""});
}

void EditEncounter(size_t index) {
    if (index >= gEncounters.size()) return;
    const Encounter &encounter = gEncounters[index];
    gEncounterWorkingDraft = encounter;
    gModalCol = encounter.col;
    gModalRow = encounter.row;
    gEditingEncounterIndex = static_cast<int>(index);
    gEncounterCreatureSources.clear();
    for (const EncounterCreature &creature : encounter.creatures)
        if (!creature.sourceFile.empty())
            gEncounterCreatureSources[creature.name] = creature.sourceFile;
    LoadEncounterTableDrafts(encounter);
    OpenModal(ModalType::Encounter,
              {encounter.name, encounter.description, FormatEncounterCreatures(encounter),
               FormatEncounterEffects(encounter), encounter.trigger, encounter.objective,
               encounter.environment, encounter.gameMasterNotes, encounter.rewards,
               encounter.successOutcome, encounter.failureOutcome, encounter.sourceFile});
}

void StartStandaloneEncounter() {
    gProjectDocument = ProjectDocument{};
    gProjectDocument.standaloneEncounter = true;
    Encounter encounter;
    encounter.name = "Untitled Encounter";
    gEncounters.push_back(std::move(encounter));
    gProjectFile.clear();
    gEditor.ResetForLoadedDocument();
    gEditorTab = EditorTab::World;
    gActiveDungeonIndex = -1;
    gPlacementMode = PlacementMode::None;
    MarkProjectDirty();
    EditEncounter(0);
    LOG_INFO("Created a new standalone encounter");
}

std::string FormatCreatureSpecialAbilities(const CreatureStatBlock &creature) {
    std::string text;
    for (const CreatureStatBlock::Ability &ability : creature.specialAbilities) {
        if (!text.empty()) text += "; ";
        text += ability.name + "=" + ability.description;
    }
    return text;
}

bool ParseCreatureSpecialAbilities(const std::string &text, CreatureStatBlock &creature,
                                   std::string &error) {
    creature.specialAbilities.clear();
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(';', start);
        std::string entry = text.substr(start, end == std::string::npos ? end : end - start);
        auto trim = [](std::string value) {
            size_t first = value.find_first_not_of(" \t");
            if (first == std::string::npos) return std::string{};
            size_t last = value.find_last_not_of(" \t");
            return value.substr(first, last - first + 1);
        };
        entry = trim(entry);
        if (!entry.empty()) {
            size_t equals = entry.find('=');
            CreatureStatBlock::Ability ability;
            if (equals == std::string::npos ||
                (ability.name = trim(entry.substr(0, equals))).empty() ||
                (ability.description = trim(entry.substr(equals + 1))).empty()) {
                error = "ABILITIES USE: NAME=DESCRIPTION; NAME=DESCRIPTION";
                return false;
            }
            creature.specialAbilities.push_back(std::move(ability));
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

std::vector<std::string> CreatureBuilderFields(const CreatureStatBlock &creature) {
    std::vector<std::string> fields{creature.name, creature.classification,
                                    creature.armorClass, creature.hitPoints, creature.speed};
    for (int score : creature.abilityScores) fields.push_back(std::to_string(score));
    fields.insert(fields.end(), {creature.savesAndSkills, creature.sensesAndLanguages,
                                  creature.challenge, creature.traits,
                                  FormatCreatureSpecialAbilities(creature), creature.actions,
                                  creature.reactions, creature.legendaryActions,
                                  creature.damageVulnerabilities, creature.damageResistances,
                                  creature.damageImmunities, creature.conditionImmunities,
                                  creature.proficiencyBonus, creature.passivePerception,
                                  creature.spellcasting, creature.portraitFile});
    return fields;
}

void OpenCreatureBuilder() {
    if (gProjectDocument.creatures.empty()) return;
    OpenModal(ModalType::CreatureBuilder,
              CreatureBuilderFields(gProjectDocument.creatures.front()));
}

void StartStandaloneCreature() {
    gProjectDocument = ProjectDocument{};
    gProjectDocument.standaloneCreature = true;
    CreatureStatBlock creature;
    creature.name = "Untitled Creature";
    creature.classification = "Medium creature, unaligned";
    creature.armorClass = "10";
    creature.hitPoints = "1 (1d8-3)";
    creature.speed = "30 ft.";
    creature.challenge = "0";
    gProjectDocument.creatures.push_back(std::move(creature));
    gProjectFile.clear();
    gEditor.ResetForLoadedDocument();
    gEditorTab = EditorTab::World;
    gPlacementMode = PlacementMode::None;
    MarkProjectDirty();
    OpenCreatureBuilder();
    LOG_INFO("Created a new standalone creature stat block");
}

void AddCreatureFileToEncounter() {
    std::string path = ChooseProjectFileToOpen("Select a creature stat block");
    if (path.empty()) return;
    ProjectDocument source;
    int version = 0;
    std::string error;
    if (!LoadProjectDocument(path, source, version, error) || !source.standaloneCreature ||
        source.creatures.size() != 1) {
        gModalError = error.empty() ? "SELECT A STANDALONE CREATURE FILE" : error;
        return;
    }
    const std::string &name = source.creatures.front().name;
    if (!gModalFields[2].empty()) gModalFields[2] += "; ";
    gModalFields[2] += "1 " + name;
    gEncounterCreatureSources[name] = path;
    gModalError.clear();
}

void LoadEncounterFileIntoBuilder() {
    if (gModalType != ModalType::Encounter) return;
    std::string path = ChooseProjectFileToOpen("Select an encounter file");
    if (path.empty()) return;
    ProjectDocument source;
    int version = 0;
    std::string error;
    if (!LoadProjectDocument(path, source, version, error) || !source.standaloneEncounter ||
        source.encounters.size() != 1) {
        gModalError = error.empty() ? "SELECT A STANDALONE ENCOUNTER FILE" : error;
        return;
    }
    gEncounterWorkingDraft = source.encounters.front();
    gEncounterWorkingDraft.sourceFile = path;
    gModalFields = {gEncounterWorkingDraft.name, gEncounterWorkingDraft.description,
                    FormatEncounterCreatures(gEncounterWorkingDraft),
                    FormatEncounterEffects(gEncounterWorkingDraft), gEncounterWorkingDraft.trigger,
                    gEncounterWorkingDraft.objective, gEncounterWorkingDraft.environment,
                    gEncounterWorkingDraft.gameMasterNotes, gEncounterWorkingDraft.rewards,
                    gEncounterWorkingDraft.successOutcome, gEncounterWorkingDraft.failureOutcome,
                    gEncounterWorkingDraft.sourceFile};
    gEncounterCreatureSources.clear();
    for (const EncounterCreature &creature : gEncounterWorkingDraft.creatures)
        if (!creature.sourceFile.empty())
            gEncounterCreatureSources[creature.name] = creature.sourceFile;
    LoadEncounterTableDrafts(gEncounterWorkingDraft);
    gModalField = 0;
    gModalError.clear();
    LOG_INFO("Loaded encounter template from %s", path.c_str());
}

void RollActiveEncounterTable() {
    gEncounterRollResult.clear();
    if (gEncounterTableDrafts.empty()) return;
    Encounter preview;
    std::string error;
    if (!BuildEncounterTablesFromDrafts(preview, error)) {
        gModalError = error;
        return;
    }
    const EncounterRollTable &table =
        preview.rollTables[static_cast<size_t>(gEncounterActiveTable)];
    int roll = 1 + std::rand() % table.dieSides;
    std::string result = "NO MATCHING RESULT";
    if (const EncounterTableEntry *entry = campaign_tools::ResolveRoll(table, roll))
        result = entry->result;
    gEncounterRollResult = "ROLLED " + std::to_string(roll) + ": " + result;
    gModalError.clear();
}

void StartEncounterRunner() {
    if (gModalType != ModalType::Encounter || gModalFields.size() != 12) return;
    Encounter encounter = gEncounterWorkingDraft;
    encounter.col = gModalCol;
    encounter.row = gModalRow;
    encounter.name = gModalFields[0].empty() ? "Encounter" : gModalFields[0];
    encounter.description = gModalFields[1];
    encounter.trigger = gModalFields[4];
    encounter.objective = gModalFields[5];
    encounter.environment = gModalFields[6];
    encounter.gameMasterNotes = gModalFields[7];
    encounter.rewards = gModalFields[8];
    encounter.successOutcome = gModalFields[9];
    encounter.failureOutcome = gModalFields[10];
    encounter.sourceFile = gModalFields[11];
    if (!ParseEncounterBuilderFields(gModalFields[2], gModalFields[3], "", encounter,
                                     gModalError) ||
        !BuildEncounterTablesFromDrafts(encounter, gModalError))
        return;
    for (EncounterCreature &creature : encounter.creatures) {
        auto source = gEncounterCreatureSources.find(creature.name);
        if (source != gEncounterCreatureSources.end()) creature.sourceFile = source->second;
    }
    if (encounter.participants.empty()) {
        for (const EncounterCreature &group : encounter.creatures) {
            for (int number = 1; number <= group.count; ++number) {
                EncounterParticipant participant;
                participant.name = group.name;
                participant.initiative = 1 + std::rand() % 20;
                if (group.count > 1) participant.name += " " + std::to_string(number);
                if (!group.sourceFile.empty()) {
                    ProjectDocument creatureDocument;
                    int version = 0;
                    std::string error;
                    if (LoadProjectDocument(group.sourceFile, creatureDocument, version, error) &&
                        creatureDocument.standaloneCreature && !creatureDocument.creatures.empty()) {
                        const std::string &hp = creatureDocument.creatures.front().hitPoints;
                        try { participant.currentHitPoints = std::stoi(hp); } catch (...) {}
                    }
                }
                encounter.participants.push_back(std::move(participant));
            }
        }
    }
    if (encounter.currentRound == 0) encounter.currentRound = 1;
    if (gEditingEncounterIndex >= 0 &&
        gEditingEncounterIndex < static_cast<int>(gEncounters.size())) {
        gEncounters[static_cast<size_t>(gEditingEncounterIndex)] = std::move(encounter);
        gRunningEncounterIndex = gEditingEncounterIndex;
    } else {
        gEncounters.push_back(std::move(encounter));
        gRunningEncounterIndex = static_cast<int>(gEncounters.size()) - 1;
        gEditingEncounterIndex = gRunningEncounterIndex;
    }
    MarkProjectDirty();
    gModalType = ModalType::EncounterRunner;
    gModalError.clear();
}

void AppendCreatureAbilityTemplate() {
    if (gModalType != ModalType::CreatureBuilder || gModalFields.size() != 27) return;
    if (!gModalFields[15].empty()) gModalFields[15] += "; ";
    gModalFields[15] += "New Ability=Describe what this ability does";
    gModalField = 15;
}

void RemoveLastCreatureAbility() {
    if (gModalType != ModalType::CreatureBuilder || gModalFields.size() != 27) return;
    CreatureStatBlock creature;
    std::string error;
    if (!ParseCreatureSpecialAbilities(gModalFields[15], creature, error) ||
        creature.specialAbilities.empty()) {
        gModalError = creature.specialAbilities.empty() ? "NO ABILITY TO REMOVE" : error;
        return;
    }
    creature.specialAbilities.pop_back();
    gModalFields[15] = FormatCreatureSpecialAbilities(creature);
    gModalError.clear();
}

bool ShowMarkerInfoAtTile(int32_t col, int32_t row) {
    if (gPlayerView && gFogData.count(TileKey(col, row)) != 0) return false;
    auto cityIt = std::find_if(gCities.begin(), gCities.end(),
                               [&](const City &city) { return city.col == col && city.row == row; });
    if (cityIt != gCities.end()) {
        gSelectedDungeonPoiIndex = -1;
        gInfoTitle = "CITY DETAILS";
        gInfoLines = {"NAME: " + cityIt->name, "RULER: " + cityIt->ruler,
                      "TILE: " + std::to_string(col) + " " + std::to_string(row)};
        OpenModal(ModalType::Info, {});
        return true;
    }
    auto poiIt = std::find_if(gPois.begin(), gPois.end(),
                              [&](const PointOfInterest &poi) { return poi.col == col && poi.row == row; });
    if (poiIt != gPois.end()) {
        int poiIndex = static_cast<int>(poiIt - gPois.begin());
        if (poiIt->kind == PoiKind::Dungeon) {
            OpenDungeonManagerForPoi(poiIndex);
            return true;
        }
        gSelectedDungeonPoiIndex = -1;
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
    const bool standaloneEncounterBuilder =
        gModalType == ModalType::Encounter && gProjectDocument.standaloneEncounter;
    if (gModalType == ModalType::Confirm) {
        ConfirmAction action = gConfirmAction;
        gModalType = ModalType::None;
        gConfirmAction = ConfirmAction::None;
        gInfoTitle.clear();
        gInfoLines.clear();
        if (accept) {
            if (action == ConfirmAction::ClearLayer) ClearActiveLayerNow();
            else if (action == ConfirmAction::LoadProject) {
                if (LoadProjectFromPath(gPendingLoadFile, false)) {
                    gProjectFile = gPendingLoadFile;
                    SaveProjectConfig();
                }
                gPendingLoadFile.clear();
            }
            else if (action == ConfirmAction::OverwriteProject) SaveProjectNow();
            else if (action == ConfirmAction::GenerateRelief) GenerateTerrainRelief();
            else if (action == ConfirmAction::ReturnMainMenu) {
                gProjectDirty = false;
                gMainMenuOpen = true;
            }
            else if (action == ConfirmAction::RecoverAutosave) {
                if (LoadProjectFromPath(kAutosaveFile, false)) {
                    gProjectDirty = true;
                    LOG_INFO("Recovered autosaved work; save the project to keep it permanently");
                }
            }
        }
        if (action == ConfirmAction::LoadProject) gPendingLoadFile.clear();
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
    if (gModalType == ModalType::EncounterRunner) {
        int index = gRunningEncounterIndex;
        gRunningEncounterIndex = -1;
        if (index >= 0 && index < static_cast<int>(gEncounters.size())) {
            MarkProjectDirty();
            EditEncounter(static_cast<size_t>(index));
        } else {
            gModalType = ModalType::None;
        }
        UpdateWindowTitle();
        return;
    }
    if (gModalType == ModalType::DungeonManager) {
        gModalType = ModalType::None;
        UpdateWindowTitle();
        return;
    }
    if (gModalType == ModalType::DungeonDetails) {
        if (!accept) {
            gModalType = ModalType::None;
            gModalFields.clear();
            gEditingDungeonIndex = -1;
            UpdateWindowTitle();
            return;
        }
        if (gModalFields.size() != 2) return;
        int dungeonIndex = gEditingDungeonIndex;
        bool creating = dungeonIndex < 0;
        if (creating) {
            Dungeon dungeon;
            dungeon.worldCol = gModalCol;
            dungeon.worldRow = gModalRow;
            dungeon.name = gModalFields[0].empty() ? "Unnamed Dungeon" : gModalFields[0];
            dungeon.description = gModalFields[1];
            gDungeons.push_back(std::move(dungeon));
            dungeonIndex = static_cast<int>(gDungeons.size()) - 1;
            LOG_INFO("Created dungeon '%s' at world tile (%d, %d)",
                     gDungeons.back().name.c_str(), gModalCol, gModalRow);
        } else if (dungeonIndex < static_cast<int>(gDungeons.size())) {
            Dungeon &dungeon = gDungeons[static_cast<size_t>(dungeonIndex)];
            dungeon.name = gModalFields[0].empty() ? "Unnamed Dungeon" : gModalFields[0];
            dungeon.description = gModalFields[1];
            LOG_INFO("Updated dungeon '%s'", dungeon.name.c_str());
        }
        if (dungeonIndex >= 0 && dungeonIndex < static_cast<int>(gDungeons.size())) {
            Dungeon &dungeon = gDungeons[static_cast<size_t>(dungeonIndex)];
            int poiIndex = FindDungeonPoiAtWorldTile(dungeon.worldCol, dungeon.worldRow);
            if (poiIndex >= 0) {
                PointOfInterest &poi = gPois[static_cast<size_t>(poiIndex)];
                poi.name = dungeon.name;
                poi.description = dungeon.description;
                gSelectedDungeonPoiIndex = poiIndex;
            }
        }
        gModalType = ModalType::None;
        gModalFields.clear();
        gEditingDungeonIndex = -1;
        ++gSceneRevision;
        MarkProjectDirty();
        if (creating) OpenDungeonTab(dungeonIndex);
        else UpdateWindowTitle();
        return;
    }
    if (gModalType == ModalType::DungeonMarker) {
        if (accept && gModalFields.size() == 2 && !gModalFields[0].empty()) {
            Dungeon *dungeon = ActiveDungeon();
            if (dungeon) {
                campaign_tools::AddDungeonMarker(
                    *dungeon, {gModalCol, gModalRow, gDungeonMarkerKind,
                               gModalFields[0], gModalFields[1], "", true});
                MarkProjectDirty();
                ++gSceneRevision;
            }
        }
        gModalType = ModalType::None;
        gModalFields.clear();
        gModalError.clear();
        UpdateWindowTitle();
        return;
    }
    if (gModalType == ModalType::CreatureBuilder) {
        if (!accept) {
            gModalType = ModalType::None;
            gModalFields.clear();
            gMainMenuOpen = true;
            UpdateWindowTitle();
            return;
        }
        if (gModalFields.size() != 27 || gModalFields[0].empty()) {
            gModalError = "A CREATURE NAME IS REQUIRED";
            return;
        }
        CreatureStatBlock creature;
        creature.name = gModalFields[0];
        creature.classification = gModalFields[1];
        creature.armorClass = gModalFields[2];
        creature.hitPoints = gModalFields[3];
        creature.speed = gModalFields[4];
        for (size_t scoreIndex = 0; scoreIndex < creature.abilityScores.size(); ++scoreIndex) {
            const std::string &text = gModalFields[5 + scoreIndex];
            if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char c) {
                    return std::isdigit(c) != 0;
                })) {
                gModalError = "ABILITY SCORES MUST BE NUMBERS FROM 0 TO 99";
                return;
            }
            int score = std::stoi(text);
            if (score < 0 || score > 99) {
                gModalError = "ABILITY SCORES MUST BE NUMBERS FROM 0 TO 99";
                return;
            }
            creature.abilityScores[scoreIndex] = score;
        }
        creature.savesAndSkills = gModalFields[11];
        creature.sensesAndLanguages = gModalFields[12];
        creature.challenge = gModalFields[13];
        creature.traits = gModalFields[14];
        if (!ParseCreatureSpecialAbilities(gModalFields[15], creature, gModalError)) return;
        creature.actions = gModalFields[16];
        creature.reactions = gModalFields[17];
        creature.legendaryActions = gModalFields[18];
        creature.damageVulnerabilities = gModalFields[19];
        creature.damageResistances = gModalFields[20];
        creature.damageImmunities = gModalFields[21];
        creature.conditionImmunities = gModalFields[22];
        creature.proficiencyBonus = gModalFields[23];
        creature.passivePerception = gModalFields[24];
        creature.spellcasting = gModalFields[25];
        creature.portraitFile = gModalFields[26];
        gProjectDocument.creatures.front() = std::move(creature);
        gModalType = ModalType::None;
        gModalFields.clear();
        MarkProjectDirty();
        SaveProjectNow();
        gMainMenuOpen = true;
        UpdateWindowTitle();
        return;
    }
    if (gModalType == ModalType::TerrainEditor) {
        std::vector<TerrainDefinition> &definitions = EditedTerrainDefinitions();
        if (!accept) {
            gModalType = ModalType::None;
            gModalFields.clear();
            gModalError.clear();
            gCreatingTerrain = false;
            UpdateWindowTitle();
            return;
        }
        int red = 0, green = 0, blue = 0;
        if (gModalFields.size() != 4 || gModalFields[0].empty() ||
            !ParseColorChannel(gModalFields[1], red) ||
            !ParseColorChannel(gModalFields[2], green) ||
            !ParseColorChannel(gModalFields[3], blue)) {
            LOG_WARN("Terrain name is required and RGB values must be whole numbers from 0 to 255");
            gModalError = "NAME REQUIRED; RGB VALUES MUST BE 0-255";
            return;
        }
        TerrainDefinition terrain{
            gModalFields[0],
            {red / 255.0f, green / 255.0f, blue / 255.0f},
        };
        if (gCreatingTerrain) {
            definitions.push_back(std::move(terrain));
            gEditingTerrainIndex = static_cast<int>(definitions.size()) - 1;
            LOG_INFO("Created terrain #%d '%s'", gEditingTerrainIndex,
                     definitions.back().name.c_str());
        } else {
            definitions[static_cast<size_t>(gEditingTerrainIndex)] = std::move(terrain);
            LOG_INFO("Updated terrain #%d '%s'", gEditingTerrainIndex,
                     definitions[static_cast<size_t>(gEditingTerrainIndex)].name.c_str());
        }
        if (gEditingDungeonTerrain) gDungeonBrush = gEditingTerrainIndex;
        else gBrush = gEditingTerrainIndex;
        gTerrainPage = gEditingTerrainIndex / kTerrainPageSize;
        gPaintMode = PaintMode::Terrain;
        gModalType = ModalType::None;
        gModalFields.clear();
        gModalError.clear();
        gCreatingTerrain = false;
        ++gSceneRevision;
        MarkProjectDirty();
        UpdateWindowTitle();
        return;
    }
    bool openDungeonManager = false;
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
            if (poi.kind == PoiKind::Dungeon) {
                gSelectedDungeonPoiIndex = static_cast<int>(gPois.size()) - 1;
                openDungeonManager = true;
            }
        } else if (gModalType == ModalType::Encounter) {
            if (gModalFields.size() != 12) return;
            Encounter encounter = gEncounterWorkingDraft;
            encounter.col = gModalCol;
            encounter.row = gModalRow;
            encounter.name = gModalFields[0].empty() ? "Encounter" : gModalFields[0];
            encounter.description = gModalFields[1];
            encounter.trigger = gModalFields[4];
            encounter.objective = gModalFields[5];
            encounter.environment = gModalFields[6];
            encounter.gameMasterNotes = gModalFields[7];
            encounter.rewards = gModalFields[8];
            encounter.successOutcome = gModalFields[9];
            encounter.failureOutcome = gModalFields[10];
            encounter.sourceFile = gModalFields[11];
            if (!ParseEncounterBuilderFields(gModalFields[2], gModalFields[3], "",
                                             encounter, gModalError) ||
                !BuildEncounterTablesFromDrafts(encounter, gModalError))
                return;
            for (EncounterCreature &creature : encounter.creatures) {
                auto source = gEncounterCreatureSources.find(creature.name);
                if (source != gEncounterCreatureSources.end()) creature.sourceFile = source->second;
            }
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
    gEncounterTableDrafts.clear();
    gModalError.clear();
    gInfoTitle.clear();
    gInfoLines.clear();
    gModalField = 0;
    gEditingEncounterIndex = -1;
    if (standaloneEncounterBuilder) {
        if (accept) SaveProjectNow();
        gMainMenuOpen = true;
    }
    if (openDungeonManager) OpenDungeonManagerForPoi(gSelectedDungeonPoiIndex);
    else UpdateWindowTitle();
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
        if (poiIt->kind == PoiKind::Dungeon && FindDungeonAtWorldTile(col, row) >= 0) {
            LOG_WARN("Dungeon POI '%s' has a linked map and cannot be removed",
                     poiIt->name.c_str());
            return;
        }
        LOG_INFO("Removed %s '%s'", PoiKindName(poiIt->kind), poiIt->name.c_str());
        gPois.erase(poiIt);
        gSelectedDungeonPoiIndex = -1;
        gDungeonManagerIndex = -1;
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
    LOG_INFO("--- Dungeons (%zu) ---", gDungeons.size());
    for (const auto &dungeon : gDungeons) {
        LOG_INFO("  '%s' at world tile (%d, %d), %zu map tiles, entrance %s, exit %s",
                 dungeon.name.c_str(), dungeon.worldCol, dungeon.worldRow, dungeon.tiles.size(),
                 dungeon.hasEntrance ? "set" : "not set", dungeon.hasExit ? "set" : "not set");
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
    if (gMainMenuOpen) {
        glfwSetWindowTitle(gWindow, "DND Map Drawer - Main Menu");
        return;
    }
    if (gProjectDocument.standaloneEncounter) {
        const char *name = gEncounters.empty() ? "Untitled Encounter"
                                                : gEncounters.front().name.c_str();
        std::string title = std::string("DND Map Drawer") +
                            (gProjectDirty ? " * - Encounter: " : " - Encounter: ") + name;
        glfwSetWindowTitle(gWindow, title.c_str());
        return;
    }
    if (gProjectDocument.standaloneCreature) {
        const char *name = gProjectDocument.creatures.empty()
                               ? "Untitled Creature"
                               : gProjectDocument.creatures.front().name.c_str();
        std::string title = std::string("DND Map Drawer") +
                            (gProjectDirty ? " * - Creature: " : " - Creature: ") + name;
        glfwSetWindowTitle(gWindow, title.c_str());
        return;
    }
    if (gEditorTab == EditorTab::Dungeon) {
        Dungeon *dungeon = ActiveDungeon();
        auto [col, row] = DungeonCursorTile();
        char dungeonTitle[320];
        std::string brushName = "Erase";
        if (dungeon && gDungeonBrush >= 0 &&
            gDungeonBrush < static_cast<int>(dungeon->terrainDefinitions.size()))
            brushName = dungeon->terrainDefinitions[static_cast<size_t>(gDungeonBrush)].name;
        std::snprintf(dungeonTitle, sizeof(dungeonTitle),
                      "DND Map Drawer%s - Dungeon: %s | %s / %s | %s | Zoom: %.0f%% | Tile (%d, %d)",
                      gProjectDirty ? " *" : "", dungeon ? dungeon->name.c_str() : "None",
                      PaintModeName(gPaintMode),
                      gPaintMode == PaintMode::Terrain ? brushName.c_str() :
                      gPaintMode == PaintMode::Elevation ? ElevationEditModeName(gElevationEditMode) : "Fog",
                      ToolName(gToolMode), gZoom * 100.0, col, row);
        glfwSetWindowTitle(gWindow, dungeonTitle);
        return;
    }
    auto [col, row] = CursorTile();
    char title[320];
    const char *modeLabel = PaintModeName(gPaintMode);
    char paintInfo[96];
    if (gPaintMode == PaintMode::Terrain)
        std::snprintf(paintInfo, sizeof(paintInfo), "%s",
                      gTerrainDefinitions[static_cast<size_t>(gBrush)].name.c_str());
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
                  gHistory.UndoCount(), gHistory.RedoCount(), col, row);
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

bool SaveLinkedDungeonFiles() {
    if (gProjectDocument.standaloneDungeon) return true;
    bool allSaved = true;
    for (const Dungeon &dungeon : gDungeons) {
        if (dungeon.sourceFile.empty()) continue;
        ProjectDocument standalone;
        standalone.standaloneDungeon = true;
        Dungeon copy = dungeon;
        copy.sourceFile.clear();
        standalone.dungeons.push_back(std::move(copy));
        std::string error;
        if (!SaveProjectDocument(dungeon.sourceFile, standalone, error)) {
            LOG_ERROR("Could not update linked dungeon %s: %s",
                      dungeon.sourceFile.c_str(), error.c_str());
            allSaved = false;
        } else
            LOG_INFO("Updated linked dungeon file %s", dungeon.sourceFile.c_str());
    }
    return allSaved;
}

bool SaveLinkedEncounterFiles() {
    if (gProjectDocument.standaloneEncounter) return true;
    bool allSaved = true;
    for (const Encounter &encounter : gEncounters) {
        if (encounter.sourceFile.empty()) continue;
        ProjectDocument standalone;
        standalone.standaloneEncounter = true;
        Encounter copy = encounter;
        copy.sourceFile.clear();
        standalone.encounters.push_back(std::move(copy));
        std::string error;
        if (!SaveProjectDocument(encounter.sourceFile, standalone, error)) {
            LOG_ERROR("Could not update linked encounter %s: %s",
                      encounter.sourceFile.c_str(), error.c_str());
            allSaved = false;
        }
    }
    return allSaved;
}

void SaveProjectNow() {
    namespace fs = std::filesystem;
    if (gProjectFile.empty()) {
        gProjectFile = ChooseProjectFileToSave(
            gProjectDocument.standaloneDungeon ? "Save dungeon map" :
            gProjectDocument.standaloneEncounter ? "Save encounter" :
            gProjectDocument.standaloneCreature ? "Save creature" : "Save world map",
            gProjectDocument.standaloneDungeon ? "dungeon_map.txt" :
            gProjectDocument.standaloneEncounter ? "encounter.txt" :
            gProjectDocument.standaloneCreature ? "creature.txt" : "world_map.txt");
        if (gProjectFile.empty()) return;
        SaveProjectConfig();
    }
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
    bool linkedDungeonsSaved = SaveLinkedDungeonFiles();
    bool linkedEncountersSaved = SaveLinkedEncounterFiles();
    if (SaveProjectToPath(gProjectFile, true) && linkedDungeonsSaved && linkedEncountersSaved)
        gProjectDirty = false;
}

void RequestProjectSave() {
    if (gProjectFile.empty()) {
        SaveProjectNow();
        return;
    }
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
    if (candidate.extension() != ".txt") {
        LOG_WARN("Ignoring invalid project filename in %s", kConfigFile);
        return;
    }
    gProjectFile = candidate.string();
}

void RequestProjectSaveAs() {
    std::string suggested = gProjectFile.empty()
                                ? (gProjectDocument.standaloneDungeon ? "dungeon_map.txt" :
                                   gProjectDocument.standaloneEncounter ? "encounter.txt" :
                                   gProjectDocument.standaloneCreature ? "creature.txt" :
                                   "world_map.txt")
                                : std::filesystem::path(gProjectFile).filename().string();
    std::string path = ChooseProjectFileToSave("Save a copy as", suggested);
    if (path.empty()) return;
    gProjectFile = path;
    SaveProjectConfig();
    SaveProjectNow();
}

void ReturnToMainMenu() {
    if (gProjectDirty) {
        OpenConfirmation(ConfirmAction::ReturnMainMenu, "RETURN TO MAIN MENU",
                         {"DISCARD UNSAVED CHANGES?", "USE SAVE OR SAVE AS FIRST TO KEEP THEM"});
        return;
    }
    gModalType = ModalType::None;
    gMainMenuOpen = true;
}

void ExportTextSheet() {
    std::string path = ChooseProjectFileToSave("Export printable reference", "reference_sheet.txt");
    if (path.empty()) return;
    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    if (gProjectDocument.standaloneCreature && !gProjectDocument.creatures.empty()) {
        const CreatureStatBlock &c = gProjectDocument.creatures.front();
        out << c.name << "\n" << c.classification << "\nAC " << c.armorClass << "  HP "
            << c.hitPoints << "  SPEED " << c.speed << "\n";
        static const char *names[] = {"STR", "DEX", "CON", "INT", "WIS", "CHA"};
        for (int i = 0; i < 6; ++i) {
            int modifier = campaign_tools::AbilityModifier(c.abilityScores[i]);
            out << names[i] << ' ' << c.abilityScores[i] << " (" << (modifier >= 0 ? "+" : "")
                << modifier << ")  ";
        }
        out << "\nTRAITS " << c.traits << "\n";
        for (const auto &ability : c.specialAbilities)
            out << ability.name << ". " << ability.description << "\n";
        out << "ACTIONS " << c.actions << "\nREACTIONS " << c.reactions
            << "\nLEGENDARY ACTIONS " << c.legendaryActions << "\n";
    } else if (gProjectDocument.standaloneEncounter && !gEncounters.empty()) {
        const Encounter &e = gEncounters.front();
        out << e.name << "\n" << e.description << "\nTRIGGER " << e.trigger
            << "\nOBJECTIVE " << e.objective << "\nREWARDS " << e.rewards << "\n";
        for (const EncounterCreature &creature : e.creatures)
            out << creature.count << " x " << creature.name << "\n";
        for (size_t i = 0; i < e.effects.size(); ++i)
            out << "STEP " << i + 1 << ": " << e.effects[i].description << "\n";
        for (const EncounterRollTable &table : e.rollTables) {
            out << "\n" << table.name << " (d" << table.dieSides << ")\n";
            for (const auto &row : table.entries)
                out << row.minimumRoll << '-' << row.maximumRoll << "  " << row.result << "\n";
        }
    } else {
        out << "MAP DRAWER REFERENCE\nCities: " << gCities.size() << "\nPoints of interest: "
            << gPois.size() << "\nEncounters: " << gEncounters.size() << "\nDungeons: "
            << gDungeons.size() << "\n";
    }
    LOG_INFO("Reference sheet exported to %s", path.c_str());
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
        gTerrainDefinitions = DefaultTerrainDefinitions();
        gBrush = std::clamp(gBrush, 0, static_cast<int>(gTerrainDefinitions.size()) - 1);
        gTerrainPage = gBrush / kTerrainPageSize;
        gElevationEditMode = ElevationEditMode::Set;
        LoadMap();
        LoadElevation();
        LoadRegions();
        LoadCities();
        LoadPois();
        LoadRoutes();
        gEditor.ResetForLoadedDocument();
        gSelectionActive = false;
        gPlacementMode = PlacementMode::None;
        if (gEditorTab == EditorTab::Dungeon) {
            gCameraX = gWorldCameraX;
            gCameraY = gWorldCameraY;
            gZoom = gWorldZoom;
        }
        gEditorTab = EditorTab::World;
        gActiveDungeonIndex = -1;
        gSelectedDungeonPoiIndex = -1;
        gDungeonManagerIndex = -1;
        gDungeonPlacementMode = DungeonPlacementMode::None;
        gTileClipboard.clear();
        gPlayerView = false;
        gMeasureStage = 0;
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
    gBrush = std::clamp(gBrush, 0, static_cast<int>(gTerrainDefinitions.size()) - 1);
    gTerrainPage = gBrush / kTerrainPageSize;
    gElevationEditMode = ElevationEditMode::Set;
    gNextRegionId = 1;
    for (const auto &[regionId, region] : gRegions) {
        (void)region;
        gNextRegionId = std::max(gNextRegionId, regionId + 1);
    }
    gActiveRegionId = 0;
    gEditor.ResetForLoadedDocument();
    gSelectionActive = false;
    gPlacementMode = PlacementMode::None;
    if (gEditorTab == EditorTab::Dungeon) {
        gCameraX = gWorldCameraX;
        gCameraY = gWorldCameraY;
        gZoom = gWorldZoom;
    }
    gEditorTab = EditorTab::World;
    gActiveDungeonIndex = -1;
    gSelectedDungeonPoiIndex = -1;
    gDungeonManagerIndex = -1;
    gDungeonPlacementMode = DungeonPlacementMode::None;
    gTileClipboard.clear();
    gPlayerView = false;
    gMeasureStage = 0;
    LOG_INFO("Project loaded from %s (version %d, %zu terrain tiles, %zu elevation tiles)",
             path.c_str(), version, gMapData.size(), gElevationData.size());
    if (gProjectDocument.standaloneDungeon && !gDungeons.empty()) OpenDungeonTab(0);
    else if (gProjectDocument.standaloneEncounter && !gEncounters.empty()) EditEncounter(0);
    else if (gProjectDocument.standaloneCreature && !gProjectDocument.creatures.empty())
        OpenCreatureBuilder();
    else UpdateWindowTitle();
    return true;
}

void ClearActiveLayerNow() {
    size_t removed = 0;
    Dungeon *dungeon = ActiveDungeon();
    if (gEditorTab == EditorTab::Dungeon && dungeon) {
        if (gPaintMode == PaintMode::Terrain) {
            removed = dungeon->tiles.size();
            dungeon->tiles.clear();
        } else if (gPaintMode == PaintMode::Elevation) {
            removed = dungeon->elevation.size();
            dungeon->elevation.clear();
        } else if (gPaintMode == PaintMode::Fog) {
            removed = dungeon->fog.size();
            dungeon->fog.clear();
        }
    } else if (gPaintMode == PaintMode::Terrain) {
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
    gHistory.Clear();
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
    Dungeon *dungeon = ActiveDungeon();
    const auto &terrain = gEditorTab == EditorTab::Dungeon && dungeon ? dungeon->tiles : gMapData;
    if (terrain.empty()) {
        LOG_WARN("There are no terrain tiles to hide");
        return;
    }
    PaintMode previousMode = gPaintMode;
    gPaintMode = PaintMode::Fog;
    BeginStroke(terrain.size(), 1);
    for (const auto &entry : terrain) {
        int32_t col = static_cast<int32_t>(entry.first >> 32);
        int32_t row = static_cast<int32_t>(entry.first & 0xFFFFFFFFu);
        SetTileRecorded(col, row, 1);
    }
    EndStroke();
    gPaintMode = previousMode;
    LOG_INFO("Fogged all %zu terrain tiles", terrain.size());
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
        int height = entry.second < kTerrainCount ? terrainHeights[entry.second] : 0;
        SetTileRecorded(col, row, height);
    }
    EndStroke();
    gPaintMode = previousPaintMode;
    gElevationEditMode = previousEditMode;
    LOG_INFO("Generated terrain-based relief for %zu map tiles", gMapData.size());
}

bool RequestProjectLoad() {
    std::string selectedFile = ChooseProjectFileToOpen("Open a map");
    if (selectedFile.empty()) return false;
    if (gProjectDirty) {
        gPendingLoadFile = selectedFile;
        OpenConfirmation(ConfirmAction::LoadProject, "DISCARD CHANGES",
                         {"LOAD: " + std::filesystem::path(selectedFile).filename().string(),
                          "UNSAVED CHANGES WILL BE LOST"});
    } else {
        if (!LoadProjectFromPath(selectedFile, false)) return false;
        gProjectFile = selectedFile;
        SaveProjectConfig();
    }
    return true;
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
    ui_geometry::AppendRect(vertices, x, y, w, h, color, alpha);
}

void AppendUiText(std::vector<float> &vertices, std::string text, float x, float y, float scale,
                  const Vec3 &color, size_t maxChars = 64) {
    ui_geometry::AppendText(vertices, std::move(text), x, y, scale, color, maxChars);
}

void AddUiButton(std::vector<float> &vertices, float x, float y, float w, float h,
                 const std::string &label, UiAction action, int value = 0, bool selected = false) {
    ui_geometry::AppendButton(vertices, gUiHits, x, y, w, h, label, action, value, selected);
}

void RebuildGuiMesh(GLuint vbo, GLsizei &outVertexCount) {
    std::vector<float> vertices;
    gUiHits.clear();

    if (gMainMenuOpen) {
        const float width = static_cast<float>(gWindowWidth);
        const float height = static_cast<float>(gWindowHeight);
        const float cardWidth = std::min(580.0f, width - 80.0f);
        const float cardHeight = 640.0f;
        const float cardX = (width - cardWidth) * 0.5f;
        const float cardY = std::max(40.0f, (height - cardHeight) * 0.5f);

        AppendUiRect(vertices, 0.0f, 0.0f, width, height, {0.035f, 0.045f, 0.060f});
        AppendUiRect(vertices, 0.0f, 0.0f, width, height * 0.30f,
                     {0.080f, 0.115f, 0.145f});
        AppendUiRect(vertices, cardX, cardY, cardWidth, cardHeight,
                     {0.075f, 0.085f, 0.105f}, 0.99f);
        AppendUiRect(vertices, cardX, cardY, 5.0f, cardHeight,
                     {0.90f, 0.66f, 0.22f});

        AppendUiText(vertices, "MAP DRAWER", cardX + 42.0f, cardY + 38.0f, 4.2f,
                     {0.94f, 0.78f, 0.34f});
        AppendUiText(vertices, "CHOOSE WHAT YOU WANT TO WORK ON", cardX + 44.0f,
                     cardY + 94.0f, 1.7f, {0.68f, 0.74f, 0.82f}, 46);

        auto addTask = [&](float y, const char *number, const char *title,
                           const char *description, UiAction action, const Vec3 &accent) {
            AppendUiRect(vertices, cardX + 38.0f, y, cardWidth - 76.0f, 70.0f,
                         {0.105f, 0.125f, 0.155f});
            AppendUiRect(vertices, cardX + 38.0f, y, 7.0f, 70.0f, accent);
            AppendUiText(vertices, number, cardX + 62.0f, y + 16.0f, 2.6f, accent, 2);
            AppendUiText(vertices, title, cardX + 112.0f, y + 11.0f, 2.1f,
                         {0.94f, 0.95f, 0.97f}, 34);
            AppendUiText(vertices, description, cardX + 112.0f, y + 40.0f, 1.25f,
                         {0.65f, 0.70f, 0.78f}, 58);
            gUiHits.push_back({cardX + 38.0f, y, cardWidth - 76.0f, 70.0f, action, 0});
        };

        addTask(cardY + 112.0f, "1", "WORLD MAP",
                "TERRAIN, REGIONS, ROUTES AND LOCATIONS", UiAction::MainWorld,
                {0.32f, 0.72f, 0.48f});
        addTask(cardY + 190.0f, "2", "DUNGEON MAP",
                "GO STRAIGHT TO A STANDALONE DUNGEON", UiAction::MainDungeon,
                {0.88f, 0.38f, 0.20f});
        addTask(cardY + 268.0f, "3", "ENCOUNTER",
                "BUILD A STANDALONE EVENT OR COMBAT", UiAction::MainEncounter,
                {0.72f, 0.42f, 0.86f});
        addTask(cardY + 346.0f, "4", "CREATURE",
                "CREATE A STANDALONE STAT BLOCK", UiAction::MainCreature,
                {0.88f, 0.64f, 0.25f});
        addTask(cardY + 424.0f, "5", "LOAD FILE",
                "CHOOSE ANY SAVED MAP, ENCOUNTER OR CREATURE", UiAction::MainLoad,
                {0.30f, 0.62f, 0.90f});

        AddUiButton(vertices, cardX + 38.0f, cardY + 514.0f, cardWidth - 190.0f,
                    40.0f, "CONTINUE LAST", UiAction::MainContinue);
        AddUiButton(vertices, cardX + cardWidth - 144.0f, cardY + 514.0f, 106.0f,
                    40.0f, "QUIT", UiAction::MainQuit);
        AppendUiText(vertices, "PRESS 1 TO 5    ESC QUITS", cardX + 142.0f,
                     cardY + 580.0f, 1.35f, {0.50f, 0.55f, 0.63f}, 44);

        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                     vertices.data(), GL_DYNAMIC_DRAW);
        outVertexCount = static_cast<GLsizei>(vertices.size() / 6);
        return;
    }

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
    bool dungeonPoiSelected = gSelectedDungeonPoiIndex >= 0 &&
                              gSelectedDungeonPoiIndex < static_cast<int>(gPois.size()) &&
                              gPois[static_cast<size_t>(gSelectedDungeonPoiIndex)].kind == PoiKind::Dungeon;
    AddUiButton(vertices, kUiWidth - 174.0f, 8.0f, 52.0f, 24.0f, "MENU",
                UiAction::ReturnMainMenu);
    AddUiButton(vertices, kUiWidth - 117.0f, 8.0f, 47.0f, 24.0f, "DNG",
                UiAction::OpenDungeons, 0, dungeonPoiSelected);
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
        int pageCount = std::max(1, (static_cast<int>(gTerrainDefinitions.size()) +
                                     kTerrainPageSize - 1) /
                                        kTerrainPageSize);
        gTerrainPage = std::clamp(gTerrainPage, 0, pageCount - 1);
        int firstTerrain = gTerrainPage * kTerrainPageSize;
        for (int slot = 0; slot < kTerrainPageSize; ++slot) {
            int terrainIndex = firstTerrain + slot;
            if (terrainIndex >= static_cast<int>(gTerrainDefinitions.size())) break;
            float x = x0 + (slot % 3) * 83.0f;
            float y = 232.0f + (slot / 3) * 28.0f;
            const TerrainDefinition &terrain =
                gTerrainDefinitions[static_cast<size_t>(terrainIndex)];
            std::string label = terrain.name.substr(0, 8);
            AddUiButton(vertices, x, y, 78.0f, h, label, UiAction::SetTerrain, terrainIndex,
                        gBrush == terrainIndex);
            AppendUiRect(vertices, x + 67.0f, y + 7.0f, 7.0f, 10.0f, terrain.color);
        }
        AddUiButton(vertices, x0, 316.0f, 31.0f, h, "<", UiAction::TerrainPagePrevious);
        AddUiButton(vertices, x0 + 36.0f, 316.0f, 31.0f, h, ">", UiAction::TerrainPageNext);
        AppendUiText(vertices, std::to_string(gTerrainPage + 1) + "/" + std::to_string(pageCount),
                     x0 + 74.0f, 323.0f, 1.4f, {0.72f, 0.77f, 0.84f}, 8);
        AddUiButton(vertices, x0 + 126.0f, 316.0f, 54.0f, h, "EDIT", UiAction::EditTerrains);
        AddUiButton(vertices, x0 + 185.0f, 316.0f, 59.0f, h, "NEW", UiAction::EditTerrains, 1);
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
    std::string projectLabel = gProjectFile.empty()
                                   ? "NOT SAVED YET"
                                   : std::filesystem::path(gProjectFile).filename().string();
    if (projectLabel.size() > 24) projectLabel = projectLabel.substr(0, 21) + "...";
    AppendUiText(vertices, projectLabel, x0 + 78.0f, 532.0f, 1.2f, {0.62f, 0.67f, 0.74f}, 24);
    AddUiButton(vertices, x0, 544.0f, 57.0f, h, "SAVE", UiAction::Save);
    AddUiButton(vertices, x0 + 62.0f, 544.0f, 57.0f, h, "LOAD", UiAction::Load);
    AddUiButton(vertices, x0 + 124.0f, 544.0f, 57.0f, h, "UNDO", UiAction::Undo);
    AddUiButton(vertices, x0 + 186.0f, 544.0f, 58.0f, h, "REDO", UiAction::Redo);
    AddUiButton(vertices, x0, 572.0f, 78.0f, h, "CLEAR", UiAction::Clear);
    AddUiButton(vertices, x0 + 83.0f, 572.0f, 78.0f, h, "EXPORT", UiAction::Export);
    AddUiButton(vertices, x0 + 166.0f, 572.0f, 78.0f, h, "SAVE AS", UiAction::SaveAs);

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
                               std::to_string(gCities.size()) + " DNG " +
                               std::to_string(gDungeons.size()),
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

    if (gSelectionActive && gEditorTab == EditorTab::World) {
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

    if (gEditorTab == EditorTab::Dungeon) {
        gUiHits.clear();
        AppendUiRect(vertices, 0.0f, 0.0f, kUiWidth, static_cast<float>(gWindowHeight), panel, 1.0f);
        AppendUiRect(vertices, kUiWidth - 2.0f, 0.0f, 2.0f, static_cast<float>(gWindowHeight),
                     {0.28f, 0.34f, 0.42f});
        AppendUiText(vertices, "DUNGEON", 12.0f, 12.0f, 2.5f, {0.88f, 0.34f, 0.18f});
        AddUiButton(vertices, 116.0f, 8.0f, 62.0f, 24.0f,
                    gProjectDocument.standaloneDungeon ? "MENU" : "WORLD",
                    UiAction::DungeonReturnWorld);
        AddUiButton(vertices, 183.0f, 8.0f, 71.0f, 24.0f, "HELP", UiAction::Help);

        Dungeon *dungeon = ActiveDungeon();
        std::string dungeonName = dungeon ? dungeon->name : "NO DUNGEON";
        AppendUiText(vertices, dungeonName, 10.0f, 48.0f, 1.8f, {0.92f, 0.76f, 0.35f}, 32);
        headingText("PAINT LAYER", 80.0f);
        AddUiButton(vertices, x0, 96.0f, 78.0f, h, "TERRAIN", UiAction::SetMode,
                    static_cast<int>(PaintMode::Terrain), gPaintMode == PaintMode::Terrain);
        AddUiButton(vertices, x0 + 83.0f, 96.0f, 78.0f, h, "HEIGHT", UiAction::SetMode,
                    static_cast<int>(PaintMode::Elevation), gPaintMode == PaintMode::Elevation);
        AddUiButton(vertices, x0 + 166.0f, 96.0f, 78.0f, h, "FOG", UiAction::SetMode,
                    static_cast<int>(PaintMode::Fog), gPaintMode == PaintMode::Fog);

        headingText("TOOLS", 128.0f);
        const ToolMode dungeonTools[] = {ToolMode::Brush, ToolMode::FloodFill, ToolMode::Line,
                                         ToolMode::Curve, ToolMode::Polygon, ToolMode::Circle,
                                         ToolMode::Scatter};
        const char *dungeonToolLabels[] = {"BRUSH", "FILL", "LINE", "CURVE",
                                           "POLY", "CIRCLE", "SCATTER"};
        for (int i = 0; i < 7; ++i) {
            float x = x0 + (i % 4) * 62.0f;
            float y = 144.0f + (i / 4) * 28.0f;
            AddUiButton(vertices, x, y, 57.0f, h, dungeonToolLabels[i], UiAction::SetTool,
                        static_cast<int>(dungeonTools[i]), gToolMode == dungeonTools[i]);
        }

        headingText(gPaintMode == PaintMode::Elevation ? "ELEVATION LEVELS" : "DUNGEON TERRAIN",
                    206.0f);
        if (gPaintMode == PaintMode::Elevation) {
            for (int elevation = kMinElevation; elevation <= kMaxElevation; ++elevation) {
                int index = elevation - kMinElevation;
                float x = x0 + (index % 7) * 35.0f;
                float y = 222.0f + (index / 7) * 28.0f;
                std::string label = elevation > 0 ? "+" + std::to_string(elevation)
                                                  : std::to_string(elevation);
                AddUiButton(vertices, x, y, 31.0f, h, label, UiAction::SetElevationValue,
                            elevation, gElevationBrush == elevation);
                Vec3 band = ElevationBandColor(elevation);
                AppendUiRect(vertices, x + 3.0f, y + h - 5.0f, 25.0f, 3.0f, band);
            }
            const char *editLabels[] = {"SET", "RAISE", "LOWER", "FLAT", "AVG"};
            for (int i = 0; i < 5; ++i)
                AddUiButton(vertices, x0 + i * 50.0f, 278.0f, i == 4 ? 44.0f : 45.0f, h,
                            editLabels[i], UiAction::SetElevationTool, i,
                            static_cast<int>(gElevationEditMode) == i);
        } else if (dungeon) {
            int pageCount = std::max(1, (static_cast<int>(dungeon->terrainDefinitions.size()) +
                                         kTerrainPageSize - 1) / kTerrainPageSize);
            gTerrainPage = std::clamp(gTerrainPage, 0, pageCount - 1);
            int firstTerrain = gTerrainPage * kTerrainPageSize;
            for (int slot = 0; slot < kTerrainPageSize; ++slot) {
                int terrainIndex = firstTerrain + slot;
                if (terrainIndex >= static_cast<int>(dungeon->terrainDefinitions.size())) break;
                float x = x0 + (slot % 3) * 83.0f;
                float y = 222.0f + (slot / 3) * 28.0f;
                const TerrainDefinition &terrain =
                    dungeon->terrainDefinitions[static_cast<size_t>(terrainIndex)];
                AddUiButton(vertices, x, y, 78.0f, h, terrain.name.substr(0, 8),
                            terrainIndex == 0 ? UiAction::DungeonSetTile : UiAction::SetTerrain,
                            terrainIndex, gPaintMode == PaintMode::Terrain &&
                                              gDungeonBrush == terrainIndex);
                AppendUiRect(vertices, x + 67.0f, y + 7.0f, 7.0f, 10.0f, terrain.color);
            }
            AddUiButton(vertices, x0, 306.0f, 31.0f, h, "<", UiAction::TerrainPagePrevious);
            AddUiButton(vertices, x0 + 36.0f, 306.0f, 31.0f, h, ">", UiAction::TerrainPageNext);
            AddUiButton(vertices, x0 + 73.0f, 306.0f, 78.0f, h, "EDIT", UiAction::EditTerrains);
            AddUiButton(vertices, x0 + 156.0f, 306.0f, 88.0f, h, "NEW", UiAction::EditTerrains, 1);
        }

        headingText("BRUSH", 342.0f);
        AddUiButton(vertices, x0, 358.0f, 52.0f, h, "-", UiAction::BrushDown);
        AddUiButton(vertices, x0 + 57.0f, 358.0f, 72.0f, h,
                    "SIZE " + std::to_string(gBrushRadius * 2 + 1), UiAction::None);
        AddUiButton(vertices, x0 + 134.0f, 358.0f, 52.0f, h, "+", UiAction::BrushUp);
        AddUiButton(vertices, x0 + 191.0f, 358.0f, 53.0f, h,
                    gRoundBrush ? "ROUND" : "SQUARE", UiAction::ToggleShape, 0, gRoundBrush);

        headingText("FOG AND HEIGHT VIEW", 390.0f);
        AddUiButton(vertices, x0, 406.0f, 78.0f, h, "PLAYER", UiAction::TogglePlayerView, 0,
                    gPlayerView);
        AddUiButton(vertices, x0 + 83.0f, 406.0f, 78.0f, h, "ELEV VIEW",
                    UiAction::ToggleElevationView, 0, gElevationView);
        AddUiButton(vertices, x0 + 166.0f, 406.0f, 78.0f, h, "SHADE",
                    UiAction::ToggleHillshade, 0, gShowHillshade);
        AddUiButton(vertices, x0, 434.0f, 119.0f, h, "HIDE ALL", UiAction::FogHideAll);
        AddUiButton(vertices, x0 + 124.0f, 434.0f, 120.0f, h, "REVEAL ALL", UiAction::FogRevealAll);

        headingText("SPECIAL MARKERS", 472.0f);
        AddUiButton(vertices, x0, 488.0f, 57.0f, h, "ENTRY", UiAction::DungeonPlaceEntrance,
                    0, gDungeonPlacementMode == DungeonPlacementMode::Entrance);
        AddUiButton(vertices, x0 + 62.0f, 488.0f, 57.0f, h, "EXIT", UiAction::DungeonPlaceExit,
                    0, gDungeonPlacementMode == DungeonPlacementMode::Exit);
        AddUiButton(vertices, x0 + 124.0f, 488.0f, 57.0f, h, "MARKER", UiAction::DungeonAddMarker,
                    0, gDungeonPlacementMode == DungeonPlacementMode::Marker);
        AddUiButton(vertices, x0 + 186.0f, 488.0f, 58.0f, h, "ENCOUNT", UiAction::DungeonAddEncounter,
                    0, gDungeonPlacementMode == DungeonPlacementMode::Encounter);
        AppendUiText(vertices, dungeon && dungeon->hasEntrance ? "ENTRANCE SET" : "ENTRANCE NOT SET",
                     x0, 518.0f, 1.4f, dungeon && dungeon->hasEntrance
                                                   ? Vec3{0.38f, 0.90f, 0.46f}
                                                   : Vec3{0.72f, 0.55f, 0.42f}, 24);
        AppendUiText(vertices, dungeon && dungeon->hasExit ? "EXIT SET" : "EXIT NOT SET",
                     x0 + 124.0f, 518.0f, 1.4f, dungeon && dungeon->hasExit
                                                            ? Vec3{0.38f, 0.72f, 0.95f}
                                                            : Vec3{0.72f, 0.55f, 0.42f}, 18);

        headingText("DUNGEON", 546.0f);
        AddUiButton(vertices, x0, 562.0f, 78.0f, h, "DETAILS", UiAction::DungeonEditDetails);
        AddUiButton(vertices, x0 + 83.0f, 562.0f, 78.0f, h, "FIT", UiAction::DungeonFit);
        AddUiButton(vertices, x0 + 166.0f, 562.0f, 78.0f, h, "GRID", UiAction::ToggleGrid, 0,
                    gShowGrid);

        headingText("PROJECT", 596.0f);
        AddUiButton(vertices, x0, 612.0f, 57.0f, h, "SAVE", UiAction::Save);
        AddUiButton(vertices, x0 + 62.0f, 612.0f, 57.0f, h, "SAVE AS", UiAction::SaveAs);
        AddUiButton(vertices, x0 + 124.0f, 612.0f, 57.0f, h, "LOAD", UiAction::Load);
        AddUiButton(vertices, x0 + 186.0f, 612.0f, 58.0f, h, "CLEAR", UiAction::Clear);

        headingText("STATUS", 650.0f);
        const std::string brushName = dungeon && gDungeonBrush >= 0 &&
                                              gDungeonBrush < static_cast<int>(dungeon->terrainDefinitions.size())
                                          ? dungeon->terrainDefinitions[static_cast<size_t>(gDungeonBrush)].name
                                          : "ERASE";
        AppendUiText(vertices, std::string(PaintModeName(gPaintMode)) + " / " +
                                   (gPaintMode == PaintMode::Terrain ? brushName :
                                    gPaintMode == PaintMode::Elevation
                                        ? ElevationEditModeName(gElevationEditMode) : "FOG"),
                     x0, 668.0f, 1.5f, {0.85f, 0.87f, 0.90f}, 35);
        AppendUiText(vertices, "MAP TILES " + std::to_string(dungeon ? dungeon->tiles.size() : 0),
                     x0, 688.0f, 1.5f, {0.70f, 0.74f, 0.80f}, 35);
        AppendUiText(vertices, "HEIGHT " + std::to_string(dungeon ? dungeon->elevation.size() : 0) +
                                   " FOG " + std::to_string(dungeon ? dungeon->fog.size() : 0),
                     x0, 708.0f, 1.5f, {0.70f, 0.74f, 0.80f}, 35);
        AppendUiText(vertices, gProjectDirty ? "UNSAVED CHANGES" : "ALL CHANGES SAVED",
                     x0, 728.0f, 1.5f,
                     gProjectDirty ? Vec3{0.95f, 0.58f, 0.30f} : Vec3{0.45f, 0.78f, 0.52f}, 35);
        if (gDungeonPlacementMode != DungeonPlacementMode::None)
            AppendUiText(vertices, gDungeonPlacementMode == DungeonPlacementMode::Entrance
                                       ? "CLICK MAP FOR ENTRANCE"
                                       : "CLICK MAP FOR EXIT",
                         x0, 750.0f, 1.5f, {0.95f, 0.65f, 0.30f}, 35);

        auto [dungeonCol, dungeonRow] = DungeonWorldToTile(gLastCanvasWorldX, gLastCanvasWorldY);
        std::string dungeonCoordinates =
            "DUNGEON TILE " + std::to_string(dungeonCol) + "  " + std::to_string(dungeonRow);
        AppendUiRect(vertices, badgeX, badgeY, badgeWidth, 30.0f,
                     {0.07f, 0.09f, 0.12f}, 1.0f);
        AppendUiText(vertices, dungeonCoordinates, badgeX + 10.0f, badgeY + 10.0f, 1.7f,
                     {0.95f, 0.48f, 0.25f}, 34);
        gUiHits.push_back({badgeX, badgeY, badgeWidth, 30.0f, UiAction::None, 0});
    }

    if (gModalType != ModalType::None) {
        gUiHits.clear();
        AppendUiRect(vertices, 0.0f, 0.0f, static_cast<float>(gWindowWidth),
                     static_cast<float>(gWindowHeight), {0.01f, 0.01f, 0.02f}, 0.72f);
        const bool keybindHelp =
            gModalType == ModalType::Info &&
            (gInfoTitle == "KEYBOARD & MOUSE HELP" || gInfoTitle == "DUNGEON MAPPER HELP");
        const bool encounterBuilder = gModalType == ModalType::Encounter ||
                                      gModalType == ModalType::EncounterRunner;
        const bool creatureBuilder = gModalType == ModalType::CreatureBuilder;
        float mw = (creatureBuilder || gModalType == ModalType::Encounter)
                       ? std::min(1180.0f, static_cast<float>(gWindowWidth) - 40.0f)
                       : (keybindHelp || encounterBuilder)
                       ? std::min(760.0f, static_cast<float>(gWindowWidth) - 60.0f)
                       : 560.0f;
        float mh = keybindHelp
                       ? std::min(760.0f, static_cast<float>(gWindowHeight) - 60.0f)
                       : (gModalType == ModalType::DungeonManager
                              ? 330.0f
                       : (gModalType == ModalType::TerrainEditor
                              ? 420.0f
                       : ((gModalType == ModalType::Info || gModalType == ModalType::Confirm)
                              ? 320.0f
                               : (creatureBuilder
                                      ? std::min(820.0f, static_cast<float>(gWindowHeight) - 40.0f)
                               : (encounterBuilder
                                      ? std::min(700.0f, static_cast<float>(gWindowHeight) - 60.0f)
                                      : (gModalFields.size() > 1 ? 290.0f : 220.0f))))));
        float mx = (gWindowWidth - mw) * 0.5f, my = (gWindowHeight - mh) * 0.5f;
        AppendUiRect(vertices, mx, my, mw, mh, {0.10f, 0.12f, 0.16f});
        AppendUiRect(vertices, mx, my, mw, 4.0f, {0.75f, 0.57f, 0.20f});
        const char *title = gModalType == ModalType::Region ? "NEW REGION" :
                            gModalType == ModalType::City ? "NEW CITY" :
                            gModalType == ModalType::Poi ? "NEW POINT OF INTEREST" :
                            gModalType == ModalType::Encounter
                                ? (gEditingEncounterIndex >= 0 ? "ENCOUNTER BUILDER - EDIT"
                                                               : "ENCOUNTER BUILDER - NEW") :
                            gModalType == ModalType::EncounterRunner ? "RUN ENCOUNTER" :
                            gModalType == ModalType::CreatureBuilder ? "CREATURE STAT BLOCK" :
                            gModalType == ModalType::TerrainEditor ? "TERRAIN EDITOR" :
                            gModalType == ModalType::DungeonManager ? "DUNGEON MANAGER" :
                            gModalType == ModalType::DungeonDetails
                                ? (gEditingDungeonIndex >= 0 ? "EDIT DUNGEON" : "NEW DUNGEON") :
                            gModalType == ModalType::DungeonMarker ? "DUNGEON MARKER" :
                            gModalType == ModalType::Info ? gInfoTitle.c_str() :
                            gModalType == ModalType::Confirm ? gInfoTitle.c_str() :
                            gModalType == ModalType::ProjectName ? "PROJECT FILE" :
                            gModalType == ModalType::Search ? "FIND ON MAP" : "NAME ROUTE";
        AppendUiText(vertices, title, mx + 24.0f, my + 24.0f, 2.5f, {0.95f, 0.82f, 0.42f});
        if (gModalType == ModalType::City || gModalType == ModalType::Poi ||
            gModalType == ModalType::Encounter || gModalType == ModalType::DungeonDetails)
            AppendUiText(vertices, "TILE " + std::to_string(gModalCol) + " " + std::to_string(gModalRow),
                         mx + 350.0f, my + 29.0f, 1.5f, {0.72f, 0.77f, 0.84f}, 24);
        if (gModalType == ModalType::DungeonManager) {
            bool hasMap = gDungeonManagerIndex >= 0 &&
                          gDungeonManagerIndex < static_cast<int>(gDungeons.size());
            const PointOfInterest *selectedPoi =
                gSelectedDungeonPoiIndex >= 0 &&
                        gSelectedDungeonPoiIndex < static_cast<int>(gPois.size())
                    ? &gPois[static_cast<size_t>(gSelectedDungeonPoiIndex)]
                    : nullptr;
            if (hasMap) {
                const Dungeon &dungeon = gDungeons[static_cast<size_t>(gDungeonManagerIndex)];
                AppendUiText(vertices, dungeon.name, mx + 24.0f, my + 76.0f, 2.2f,
                             {0.92f, 0.76f, 0.35f}, 46);
                AppendUiText(vertices, "WORLD TILE " + std::to_string(dungeon.worldCol) + " " +
                                               std::to_string(dungeon.worldRow),
                             mx + 24.0f, my + 112.0f, 1.6f, {0.72f, 0.77f, 0.84f}, 48);
                AppendUiText(vertices, "MAP TILES " + std::to_string(dungeon.tiles.size()) +
                                               "   ENTRANCE " + (dungeon.hasEntrance ? "SET" : "NOT SET") +
                                               "   EXIT " + (dungeon.hasExit ? "SET" : "NOT SET"),
                             mx + 24.0f, my + 142.0f, 1.5f, {0.82f, 0.85f, 0.90f}, 68);
                std::string details = dungeon.description.empty() ? "NO DESCRIPTION" : dungeon.description;
                AppendUiText(vertices, details, mx + 24.0f, my + 174.0f, 1.5f,
                             {0.70f, 0.74f, 0.80f}, 68);
                if (!dungeon.sourceFile.empty())
                    AppendUiText(vertices,
                                 "LINKED FILE " +
                                     std::filesystem::path(dungeon.sourceFile).filename().string(),
                                 mx + 24.0f, my + 204.0f, 1.4f,
                                 {0.42f, 0.72f, 0.92f}, 62);
            } else {
                AppendUiText(vertices, selectedPoi ? selectedPoi->name : "DUNGEON POI",
                             mx + 24.0f, my + 76.0f, 2.2f, {0.92f, 0.76f, 0.35f}, 46);
                AppendUiText(vertices, "THIS DUNGEON POI DOES NOT HAVE A MAP YET",
                             mx + 24.0f, my + 122.0f, 1.6f, {0.78f, 0.80f, 0.84f}, 62);
                AppendUiText(vertices, "CREATE MAP TO OPEN ITS DEDICATED DUNGEON TAB",
                             mx + 24.0f, my + 156.0f, 1.5f, {0.62f, 0.68f, 0.76f}, 62);
            }
            AddUiButton(vertices, mx + 24.0f, my + mh - 48.0f, 118.0f, 28.0f,
                        hasMap ? "OPEN MAP" : "CREATE MAP",
                        hasMap ? UiAction::DungeonManagerOpen : UiAction::DungeonManagerNew);
            AddUiButton(vertices, mx + 154.0f, my + mh - 48.0f, 118.0f, 28.0f,
                        hasMap ? "RELINK FILE" : "LINK FILE", UiAction::DungeonManagerLink);
            AddUiButton(vertices, mx + mw - 112.0f, my + mh - 48.0f, 88.0f, 28.0f,
                        "CLOSE", UiAction::ModalCancel);
        } else if (gModalType == ModalType::EncounterRunner) {
            Encounter *encounter = gRunningEncounterIndex >= 0 &&
                                           gRunningEncounterIndex < static_cast<int>(gEncounters.size())
                                       ? &gEncounters[static_cast<size_t>(gRunningEncounterIndex)]
                                       : nullptr;
            if (encounter) {
                AppendUiText(vertices, encounter->name, mx + 24.0f, my + 66.0f, 2.1f,
                             {0.92f, 0.76f, 0.35f}, 48);
                AppendUiText(vertices, "ROUND " + std::to_string(encounter->currentRound),
                             mx + 24.0f, my + 102.0f, 1.7f,
                             {0.45f, 0.78f, 0.94f}, 20);
                AddUiButton(vertices, mx + 150.0f, my + 92.0f, 74.0f, 26.0f, "PREVIOUS",
                            UiAction::EncounterRunnerPrevious);
                AddUiButton(vertices, mx + 230.0f, my + 92.0f, 58.0f, 26.0f, "NEXT",
                            UiAction::EncounterRunnerNext);
                AddUiButton(vertices, mx + 294.0f, my + 92.0f, 92.0f, 26.0f, "NEXT ROUND",
                            UiAction::EncounterRunnerRound);
                AddUiButton(vertices, mx + 410.0f, my + 92.0f, 58.0f, 26.0f, "-1 HP",
                            UiAction::EncounterRunnerDamage);
                AddUiButton(vertices, mx + 474.0f, my + 92.0f, 58.0f, 26.0f, "+1 HP",
                            UiAction::EncounterRunnerHeal);
                AddUiButton(vertices, mx + 538.0f, my + 92.0f, 104.0f, 26.0f, "DEFEATED",
                            UiAction::EncounterRunnerToggleDefeated);

                float participantY = my + 138.0f;
                for (size_t index = 0; index < encounter->participants.size() && index < 12; ++index) {
                    const EncounterParticipant &participant = encounter->participants[index];
                    bool active = static_cast<int>(index) == encounter->activeParticipant;
                    AppendUiRect(vertices, mx + 24.0f, participantY, mw - 48.0f, 34.0f,
                                 active ? Vec3{0.20f, 0.28f, 0.38f} : Vec3{0.14f, 0.16f, 0.20f});
                    std::string line = (active ? "> " : "  ") + participant.name +
                        "   INIT " + std::to_string(participant.initiative) +
                        "   HP " + std::to_string(participant.currentHitPoints);
                    if (!participant.conditions.empty()) line += "   " + participant.conditions;
                    if (participant.defeated) line += "   DEFEATED";
                    AppendUiText(vertices, line, mx + 34.0f, participantY + 11.0f, 1.5f,
                                 participant.defeated ? Vec3{0.65f, 0.46f, 0.46f}
                                                      : Vec3{0.90f, 0.92f, 0.95f}, 78);
                    participantY += 39.0f;
                }
                if (encounter->participants.empty())
                    AppendUiText(vertices, "NO CREATURE PARTICIPANTS - USE EFFECTS AND TABLES",
                                 mx + 24.0f, participantY, 1.5f,
                                 {0.68f, 0.73f, 0.80f}, 64);
                AppendUiText(vertices,
                             "RUN STATE IS SAVED WITH THE ENCOUNTER. CLOSE RETURNS TO EDITING.",
                             mx + 24.0f, my + mh - 78.0f, 1.35f,
                             {0.62f, 0.68f, 0.76f}, 76);
            }
            AddUiButton(vertices, mx + mw - 112.0f, my + mh - 48.0f, 88.0f, 28.0f,
                        "CLOSE", UiAction::ModalCancel);
        } else if (gModalType == ModalType::Info) {
            const float lineSpacing = keybindHelp ? 20.0f : 34.0f;
            for (size_t i = 0; i < gInfoLines.size(); ++i) {
                const bool sectionHeading =
                    keybindHelp && (gInfoLines[i] == "NAVIGATION" || gInfoLines[i] == "PAINTING" ||
                                    gInfoLines[i] == "TOOLS" ||
                                    gInfoLines[i] == "WORLD AND PROJECT" ||
                                    gInfoLines[i] == "DUNGEON TAB");
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
            const char *fieldLabels[27] = {"NAME", "DETAILS", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", ""};
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
            if (gModalType == ModalType::DungeonMarker) {
                static const char *markerNames[] = {"ROOM", "ENCOUNTER", "TRAP", "TREASURE",
                                                    "SECRET", "STAIRS", "PORTAL", "NOTE"};
                for (int i = 0; i < 8; ++i) {
                    float kindX = mx + 24.0f + static_cast<float>(i % 4) * 128.0f;
                    float kindY = my + 54.0f + static_cast<float>(i / 4) * 26.0f;
                    AddUiButton(vertices, kindX, kindY, 122.0f, 22.0f, markerNames[i],
                                UiAction::DungeonMarkerKind, i,
                                static_cast<int>(gDungeonMarkerKind) == i);
                }
                fieldLabels[0] = "MARKER NAME";
                fieldLabels[1] = "DESCRIPTION OR GM NOTE";
            }
            if (gModalType == ModalType::Encounter) {
                fieldLabels[1] = "OVERVIEW OR NONCOMBAT EVENT";
                fieldLabels[2] = "CREATURES OPTIONAL - 3 GOBLINS; 1 OGRE";
                fieldLabels[3] = "EFFECT CHAIN - FIRST STEP > NEXT STEP";
                fieldLabels[4] = "TRIGGER";
                fieldLabels[5] = "OBJECTIVE";
                fieldLabels[6] = "ENVIRONMENT";
                fieldLabels[7] = "GM NOTES";
                fieldLabels[8] = "REWARDS";
                fieldLabels[9] = "SUCCESS OUTCOME";
                fieldLabels[10] = "FAILURE OUTCOME";
                fieldLabels[11] = "LINKED SOURCE FILE";
            }
            if (gModalType == ModalType::CreatureBuilder) {
                const char *creatureLabels[27] = {
                    "NAME", "SIZE, TYPE AND ALIGNMENT", "ARMOR CLASS", "HIT POINTS", "SPEED",
                    "STR", "DEX", "CON", "INT", "WIS", "CHA", "SAVING THROWS AND SKILLS",
                    "SENSES AND LANGUAGES", "CHALLENGE RATING", "TRAITS",
                    "SPECIAL ABILITIES - NAME=DESCRIPTION; ...", "ACTIONS", "REACTIONS",
                    "LEGENDARY ACTIONS", "DAMAGE VULNERABILITIES", "DAMAGE RESISTANCES",
                    "DAMAGE IMMUNITIES", "CONDITION IMMUNITIES", "PROFICIENCY BONUS",
                    "PASSIVE PERCEPTION", "SPELLCASTING", "PORTRAIT OR TOKEN FILE"};
                for (int index = 0; index < 27; ++index) fieldLabels[index] = creatureLabels[index];
            }
            if (gModalType == ModalType::TerrainEditor) {
                const std::vector<TerrainDefinition> &definitions = EditedTerrainDefinitions();
                fieldLabels[1] = "RED 0-255";
                fieldLabels[2] = "GREEN 0-255";
                fieldLabels[3] = "BLUE 0-255";
                AddUiButton(vertices, mx + 24.0f, my + 58.0f, 76.0f, 28.0f, "PREV",
                            UiAction::ModalTerrainPrevious);
                AddUiButton(vertices, mx + 106.0f, my + 58.0f, 76.0f, 28.0f, "NEXT",
                            UiAction::ModalTerrainNext);
                AddUiButton(vertices, mx + 188.0f, my + 58.0f, 76.0f, 28.0f, "NEW",
                            UiAction::ModalTerrainNew);
                std::string terrainPosition = gCreatingTerrain
                                                  ? "NEW TERRAIN"
                                                   : "TERRAIN " + std::to_string(gEditingTerrainIndex + 1) +
                                                         " OF " + std::to_string(definitions.size());
                AppendUiText(vertices, terrainPosition, mx + 286.0f, my + 67.0f, 1.5f,
                             {0.72f, 0.77f, 0.84f}, 30);
                int previewRed = 0, previewGreen = 0, previewBlue = 0;
                if (gModalFields.size() == 4 &&
                    ParseColorChannel(gModalFields[1], previewRed) &&
                    ParseColorChannel(gModalFields[2], previewGreen) &&
                    ParseColorChannel(gModalFields[3], previewBlue)) {
                    AppendUiRect(vertices, mx + mw - 56.0f, my + 56.0f, 34.0f, 32.0f,
                                 {0.04f, 0.04f, 0.05f});
                    AppendUiRect(vertices, mx + mw - 52.0f, my + 60.0f, 26.0f, 24.0f,
                                 {previewRed / 255.0f, previewGreen / 255.0f,
                                  previewBlue / 255.0f});
                }
            }
            float fieldsY = my + (gModalType == ModalType::DungeonMarker
                                      ? 116.0f
                                      : ((gModalType == ModalType::Poi ||
                                          gModalType == ModalType::TerrainEditor)
                                             ? 98.0f : 62.0f));
            const float fieldStep = creatureBuilder ? 47.0f : 62.0f;
            auto fieldY = [&](size_t index) {
                if (gModalType == ModalType::Encounter && index >= 4)
                    return fieldsY + static_cast<float>(index - 4) * 62.0f;
                if (!creatureBuilder) return fieldsY + static_cast<float>(index) * fieldStep;
                if (index < 5) return fieldsY + static_cast<float>(index) * fieldStep;
                if (index <= 10) return fieldsY + 5.0f * fieldStep;
                if (index >= 19) return fieldsY + static_cast<float>(index - 19) * 62.0f;
                return fieldsY + static_cast<float>(6 + index - 11) * fieldStep;
            };
            for (size_t i = 0; i < gModalFields.size(); ++i) {
                float fy = fieldY(i);
                if (creatureBuilder && i >= 5 && i <= 10) {
                    float segment = 690.0f / 6.0f;
                    float fx = mx + 24.0f + static_cast<float>(i - 5) * segment;
                    AppendUiText(vertices, fieldLabels[i], fx, fy, 1.4f, heading, 4);
                    Vec3 scoreColor = static_cast<int>(i) == gModalField
                                          ? Vec3{0.20f, 0.28f, 0.38f}
                                          : Vec3{0.14f, 0.16f, 0.20f};
                    AppendUiRect(vertices, fx, fy + 15.0f, segment - 6.0f, 32.0f, scoreColor);
                    std::string shown = gModalFields[i];
                    if (static_cast<int>(i) != gModalField && !shown.empty()) {
                        try {
                            int score = std::stoi(shown);
                            int modifier = campaign_tools::AbilityModifier(score);
                            shown += modifier >= 0 ? "  +" : "  ";
                            shown += std::to_string(modifier);
                        } catch (...) {}
                    }
                    if (static_cast<int>(i) == gModalField) shown += "_";
                    AppendUiText(vertices, shown, fx + 8.0f, fy + 25.0f, 1.7f,
                                 {0.95f, 0.95f, 0.96f}, 4);
                    gUiHits.push_back({fx, fy + 15.0f, segment - 6.0f, 32.0f,
                                       UiAction::ModalNext, static_cast<int>(i)});
                    continue;
                }
                const bool advancedCreatureField = creatureBuilder && i >= 19;
                const bool encounterDetailField = gModalType == ModalType::Encounter && i >= 4;
                float fieldX = (advancedCreatureField || encounterDetailField)
                                   ? mx + 738.0f : mx + 24.0f;
                float fieldWidth = (advancedCreatureField || encounterDetailField)
                                       ? mw - 762.0f
                                       : ((creatureBuilder || gModalType == ModalType::Encounter)
                                              ? 690.0f : mw - 48.0f);
                AppendUiText(vertices, fieldLabels[i], fieldX, fy, 1.5f, heading);
                Vec3 fieldColor = static_cast<int>(i) == gModalField ? Vec3{0.20f, 0.28f, 0.38f}
                                                                      : Vec3{0.14f, 0.16f, 0.20f};
                AppendUiRect(vertices, fieldX, fy + 15.0f, fieldWidth, 32.0f, fieldColor);
                std::string shown = gModalFields[i];
                if (static_cast<int>(i) == gModalField) shown += "_";
                if (shown.size() > 68) shown = "< " + shown.substr(shown.size() - 66);
                int visibleCharacters = (advancedCreatureField || encounterDetailField) ? 36 : 68;
                AppendUiText(vertices, shown, fieldX + 8.0f, fy + 25.0f, 1.7f,
                             {0.95f, 0.95f, 0.96f}, visibleCharacters);
                gUiHits.push_back({fieldX, fy + 15.0f, fieldWidth, 32.0f,
                                    i == 0 ? UiAction::ModalPrevious : UiAction::ModalNext,
                                    static_cast<int>(i)});
            }
            if (creatureBuilder && gModalFields.size() == 27) {
                AddUiButton(vertices, mx + 470.0f, fieldY(15) - 5.0f,
                            104.0f, 20.0f, "ADD ABILITY", UiAction::CreatureAddAbility);
                AddUiButton(vertices, mx + 580.0f, fieldY(15) - 5.0f,
                            110.0f, 20.0f, "REMOVE LAST", UiAction::CreatureRemoveAbility);
            }
            if (gModalType == ModalType::Encounter && gModalFields.size() == 12) {
                constexpr int rowsPerPage = 5;
                const float sectionY = my + 310.0f;
                AppendUiText(vertices, "ROLL TABLES", mx + 24.0f, sectionY, 1.6f,
                             {0.90f, 0.76f, 0.35f}, 24);
                AddUiButton(vertices, mx + 230.0f, sectionY - 8.0f, 154.0f, 26.0f,
                            "LOAD ENCOUNTER", UiAction::EncounterLoadFile);
                AddUiButton(vertices, mx + 390.0f, sectionY - 8.0f, 126.0f, 26.0f,
                            "ADD CREATURE", UiAction::EncounterAddCreature);
                AddUiButton(vertices, mx + 522.0f, sectionY - 8.0f, 64.0f, 26.0f,
                            "RUN", UiAction::EncounterRun);

                const float controlsY = sectionY + 22.0f;
                AddUiButton(vertices, mx + 24.0f, controlsY, 56.0f, 24.0f, "PREV",
                            UiAction::EncounterPreviousTable);
                AddUiButton(vertices, mx + 86.0f, controlsY, 56.0f, 24.0f, "NEXT",
                            UiAction::EncounterNextTable);
                AddUiButton(vertices, mx + 148.0f, controlsY, 92.0f, 24.0f, "ADD TABLE",
                            UiAction::EncounterAddTable);
                if (!gEncounterTableDrafts.empty())
                    AddUiButton(vertices, mx + 246.0f, controlsY, 108.0f, 24.0f,
                                "DELETE TABLE", UiAction::EncounterDeleteTable);
                if (!gEncounterTableDrafts.empty())
                    AddUiButton(vertices, mx + 360.0f, controlsY, 58.0f, 24.0f,
                                "ROLL", UiAction::EncounterRollTable);
                std::string tablePosition = gEncounterTableDrafts.empty()
                                                ? "NO TABLES - OPTIONAL"
                                                : "TABLE " + std::to_string(gEncounterActiveTable + 1) +
                                                      " OF " + std::to_string(gEncounterTableDrafts.size());
                AppendUiText(vertices, tablePosition, mx + 430.0f, controlsY + 8.0f, 1.35f,
                             {0.68f, 0.73f, 0.80f}, 35);

                if (!gEncounterTableDrafts.empty()) {
                    EncounterTableDraft &table =
                        gEncounterTableDrafts[static_cast<size_t>(gEncounterActiveTable)];
                    const float detailsY = controlsY + 31.0f;
                    AppendUiText(vertices, "TABLE NAME", mx + 24.0f, detailsY, 1.3f, heading, 18);
                    AppendUiText(vertices, "DIE SIDES", mx + 518.0f, detailsY, 1.3f, heading, 12);
                    auto addEncounterCell = [&](float x, float y, float width, int field,
                                                const std::string &value, int maxChars) {
                        Vec3 color = field == gModalField ? Vec3{0.20f, 0.28f, 0.38f}
                                                         : Vec3{0.14f, 0.16f, 0.20f};
                        AppendUiRect(vertices, x, y, width, 28.0f, color);
                        std::string shown = value;
                        if (field == gModalField) shown += "_";
                        if (shown.size() > static_cast<size_t>(maxChars))
                            shown = "< " + shown.substr(shown.size() - maxChars + 2);
                        AppendUiText(vertices, shown, x + 7.0f, y + 8.0f, 1.45f,
                                     {0.95f, 0.95f, 0.96f}, maxChars);
                        gUiHits.push_back({x, y, width, 28.0f, UiAction::ModalNext, field});
                    };
                    addEncounterCell(mx + 24.0f, detailsY + 14.0f, 480.0f, 100, table.name, 46);
                    addEncounterCell(mx + 518.0f, detailsY + 14.0f, 90.0f, 101,
                                     table.dieSides, 6);

                    const float headerY = detailsY + 50.0f;
                    AppendUiText(vertices, "FROM", mx + 30.0f, headerY, 1.3f, heading, 8);
                    AppendUiText(vertices, "TO", mx + 124.0f, headerY, 1.3f, heading, 8);
                    AppendUiText(vertices, "RESULT", mx + 218.0f, headerY, 1.3f, heading, 12);
                    int firstRow = gEncounterTableRowPage * rowsPerPage;
                    int lastRow = std::min(firstRow + rowsPerPage,
                                           static_cast<int>(table.rows.size()));
                    for (int row = firstRow; row < lastRow; ++row) {
                        const EncounterTableRowDraft &draft = table.rows[static_cast<size_t>(row)];
                        float rowY = headerY + 17.0f + static_cast<float>(row - firstRow) * 31.0f;
                        addEncounterCell(mx + 24.0f, rowY, 88.0f, 200 + row * 3,
                                         draft.minimumRoll, 6);
                        addEncounterCell(mx + 118.0f, rowY, 88.0f, 201 + row * 3,
                                         draft.maximumRoll, 6);
                        addEncounterCell(mx + 212.0f, rowY, 502.0f, 202 + row * 3,
                                         draft.result, 48);
                    }
                    if (table.rows.empty())
                        AppendUiText(vertices, "NO ROWS YET - ADD A ROW TO BEGIN", mx + 24.0f,
                                     headerY + 31.0f, 1.35f, {0.62f, 0.68f, 0.76f}, 50);

                    const float rowControlsY = headerY + 176.0f;
                    AddUiButton(vertices, mx + 24.0f, rowControlsY, 76.0f, 24.0f, "ADD ROW",
                                UiAction::EncounterAddTableRow);
                    AddUiButton(vertices, mx + 106.0f, rowControlsY, 92.0f, 24.0f, "DELETE ROW",
                                UiAction::EncounterDeleteTableRow);
                    AddUiButton(vertices, mx + 216.0f, rowControlsY, 56.0f, 24.0f, "PREV",
                                UiAction::EncounterPreviousRows);
                    AddUiButton(vertices, mx + 278.0f, rowControlsY, 56.0f, 24.0f, "NEXT",
                                UiAction::EncounterNextRows);
                    int pageCount = std::max(1, (static_cast<int>(table.rows.size()) +
                                                 rowsPerPage - 1) / rowsPerPage);
                    AppendUiText(vertices, "ROWS PAGE " + std::to_string(gEncounterTableRowPage + 1) +
                                               " OF " + std::to_string(pageCount),
                                 mx + 350.0f, rowControlsY + 8.0f, 1.3f,
                                 {0.68f, 0.73f, 0.80f}, 28);
                    if (!gEncounterRollResult.empty())
                        AppendUiText(vertices, gEncounterRollResult, mx + 500.0f,
                                     rowControlsY + 8.0f, 1.25f,
                                     {0.42f, 0.88f, 0.58f}, 28);
                } else {
                    AppendUiText(vertices,
                                 "ADD A TABLE FOR RANDOM RESULTS, OR LEAVE THIS AS A SIMPLE EVENT",
                                 mx + 24.0f, controlsY + 48.0f, 1.35f,
                                 {0.62f, 0.68f, 0.76f}, 76);
                }
            }
            float buttonY = my + mh - 48.0f;
            if (!gModalError.empty())
                AppendUiText(vertices, gModalError, mx + 24.0f, buttonY - 23.0f, 1.4f,
                             {0.95f, 0.38f, 0.28f}, 64);
            AddUiButton(vertices, mx + mw - 210.0f, buttonY, 86.0f, 28.0f, "CANCEL", UiAction::ModalCancel);
            AddUiButton(vertices, mx + mw - 112.0f, buttonY, 88.0f, 28.0f,
                        gModalType == ModalType::ProjectName ? "APPLY" :
                        gModalType == ModalType::Search ? "FIND" :
                        gModalType == ModalType::CreatureBuilder ? "SAVE" :
                        gModalType == ModalType::TerrainEditor ? (gCreatingTerrain ? "CREATE" : "SAVE") :
                        gModalType == ModalType::DungeonDetails
                            ? (gEditingDungeonIndex >= 0 ? "SAVE" : "CREATE") :
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
        case UiAction::MainWorld:
            gMainMenuOpen = false;
            if (!gEditorSessionStarted || gProjectDocument.standaloneDungeon ||
                gProjectDocument.standaloneEncounter || gProjectDocument.standaloneCreature)
                StartWorldDocument();
            else if (gEditorTab == EditorTab::Dungeon) ReturnToWorldTab();
            gEditorSessionStarted = true;
            gPlacementMode = PlacementMode::None;
            break;
        case UiAction::MainDungeon:
            gMainMenuOpen = false;
            if (gEditorSessionStarted && gProjectDocument.standaloneDungeon && !gDungeons.empty())
                OpenDungeonTab(0);
            else StartStandaloneDungeon();
            gEditorSessionStarted = true;
            break;
        case UiAction::MainEncounter:
            gMainMenuOpen = false;
            if (gEditorSessionStarted && gProjectDocument.standaloneEncounter &&
                !gEncounters.empty())
                EditEncounter(0);
            else
                StartStandaloneEncounter();
            gEditorSessionStarted = true;
            break;
        case UiAction::MainCreature:
            gMainMenuOpen = false;
            if (gEditorSessionStarted && gProjectDocument.standaloneCreature &&
                !gProjectDocument.creatures.empty())
                OpenCreatureBuilder();
            else
                StartStandaloneCreature();
            gEditorSessionStarted = true;
            break;
        case UiAction::MainLoad:
            gMainMenuOpen = false;
            if (gConfirmAction == ConfirmAction::RecoverAutosave) {
                gModalType = ModalType::None;
                gConfirmAction = ConfirmAction::None;
                gInfoTitle.clear();
                gInfoLines.clear();
            }
            if (!RequestProjectLoad()) gMainMenuOpen = true;
            else gEditorSessionStarted = true;
            break;
        case UiAction::MainContinue: {
            std::error_code error;
            if (!gProjectFile.empty() && std::filesystem::exists(gProjectFile, error) &&
                LoadProjectFromPath(gProjectFile, false)) {
                gMainMenuOpen = false;
                gEditorSessionStarted = true;
            } else {
                gMainMenuOpen = false;
                if (!RequestProjectLoad()) gMainMenuOpen = true;
                else gEditorSessionStarted = true;
            }
            break;
        }
        case UiAction::MainQuit: glfwSetWindowShouldClose(gWindow, GLFW_TRUE); break;
        case UiAction::SetMode:
            gPaintMode = static_cast<PaintMode>(hit.value);
            gDungeonPlacementMode = DungeonPlacementMode::None;
            break;
        case UiAction::SetTool: SelectTool(static_cast<ToolMode>(hit.value)); break;
        case UiAction::SetTerrain:
            if (gEditorTab == EditorTab::Dungeon) {
                gDungeonBrush = std::clamp(hit.value, 1,
                    static_cast<int>(ActiveTerrainDefinitions().size()) - 1);
                gTerrainPage = gDungeonBrush / kTerrainPageSize;
            } else {
                gBrush = std::clamp(hit.value, 0, static_cast<int>(gTerrainDefinitions.size()) - 1);
                gTerrainPage = gBrush / kTerrainPageSize;
            }
            gPaintMode = PaintMode::Terrain;
            break;
        case UiAction::EditTerrains:
            OpenTerrainEditor();
            if (hit.value != 0) StartNewTerrain();
            break;
        case UiAction::TerrainPagePrevious: {
            int pageCount = std::max(1, (static_cast<int>(ActiveTerrainDefinitions().size()) +
                                         kTerrainPageSize - 1) /
                                            kTerrainPageSize);
            gTerrainPage = (gTerrainPage + pageCount - 1) % pageCount;
            break;
        }
        case UiAction::TerrainPageNext: {
            int pageCount = std::max(1, (static_cast<int>(ActiveTerrainDefinitions().size()) +
                                         kTerrainPageSize - 1) /
                                            kTerrainPageSize);
            gTerrainPage = (gTerrainPage + 1) % pageCount;
            break;
        }
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
        case UiAction::OpenDungeons: OpenDungeonManager(); break;
        case UiAction::DeleteMarker: RemoveMarkerAtCursor(); break;
        case UiAction::DeleteRoute: RemoveRouteAtCursor(); break;
        case UiAction::WorldInfo: PrintWorldInfo(); break;
        case UiAction::Help: OpenKeybindHelp(); break;
        case UiAction::Find: OpenModal(ModalType::Search, {gLastSearchQuery}); break;
        case UiAction::Undo: Undo(); break;
        case UiAction::Redo: Redo(); break;
        case UiAction::Save: RequestProjectSave(); break;
        case UiAction::SaveAs:
            if (gModalType == ModalType::Encounter || gModalType == ModalType::CreatureBuilder) {
                CloseModal(true);
                if (gModalType == ModalType::None && gMainMenuOpen) RequestProjectSaveAs();
            } else {
                RequestProjectSaveAs();
            }
            break;
        case UiAction::Load: RequestProjectLoad(); break;
        case UiAction::ReturnMainMenu: ReturnToMainMenu(); break;
        case UiAction::Clear: RequestClearActiveLayer(); break;
        case UiAction::Export: ExportScreenshot(); break;
        case UiAction::ExportSheet: ExportTextSheet(); break;
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
            gPaintMode = PaintMode::Fog;
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
        case UiAction::ModalNext:
            gModalField = hit.value;
            if (gModalType == ModalType::Encounter && hit.value >= 200)
                gEncounterSelectedTableRow = (hit.value - 200) / 3;
            break;
        case UiAction::ModalAccept: CloseModal(true); break;
        case UiAction::ModalCancel: CloseModal(false); break;
        case UiAction::ModalPoiKind:
            gModalPoiKind = static_cast<PoiKind>(std::clamp(hit.value, 0, kPoiKindCount - 1)); break;
        case UiAction::ModalTerrainPrevious:
            LoadTerrainEditorFields((gCreatingTerrain ? static_cast<int>(EditedTerrainDefinitions().size())
                                                       : gEditingTerrainIndex) -
                                    1);
            break;
        case UiAction::ModalTerrainNext:
            LoadTerrainEditorFields((gCreatingTerrain ? -1 : gEditingTerrainIndex) + 1);
            break;
        case UiAction::ModalTerrainNew: StartNewTerrain(); break;
        case UiAction::DungeonReturnWorld: ReturnToWorldTab(); break;
        case UiAction::DungeonSetTile:
            gDungeonBrush = std::clamp(hit.value, 0,
                static_cast<int>(ActiveTerrainDefinitions().size()) - 1);
            gPaintMode = PaintMode::Terrain;
            gDungeonPlacementMode = DungeonPlacementMode::None;
            break;
        case UiAction::DungeonPlaceEntrance:
            gDungeonPlacementMode = gDungeonPlacementMode == DungeonPlacementMode::Entrance
                                        ? DungeonPlacementMode::None
                                        : DungeonPlacementMode::Entrance;
            break;
        case UiAction::DungeonPlaceExit:
            gDungeonPlacementMode = gDungeonPlacementMode == DungeonPlacementMode::Exit
                                        ? DungeonPlacementMode::None
                                        : DungeonPlacementMode::Exit;
            break;
        case UiAction::DungeonAddMarker:
            gDungeonPlacementMode = gDungeonPlacementMode == DungeonPlacementMode::Marker
                                        ? DungeonPlacementMode::None
                                        : DungeonPlacementMode::Marker;
            break;
        case UiAction::DungeonAddEncounter:
            gDungeonPlacementMode = gDungeonPlacementMode == DungeonPlacementMode::Encounter
                                        ? DungeonPlacementMode::None
                                        : DungeonPlacementMode::Encounter;
            break;
        case UiAction::DungeonMarkerKind:
            gDungeonMarkerKind = static_cast<DungeonMarkerKind>(std::clamp(hit.value, 0, 7));
            break;
        case UiAction::DungeonFit: FitDungeonToWindow(); break;
        case UiAction::DungeonEditDetails: OpenDungeonDetailsForm(); break;
        case UiAction::DungeonManagerOpen:
            if (gDungeonManagerIndex >= 0 &&
                gDungeonManagerIndex < static_cast<int>(gDungeons.size())) {
                gModalType = ModalType::None;
                OpenDungeonTab(gDungeonManagerIndex);
            }
            break;
        case UiAction::DungeonManagerNew: OpenNewDungeonForm(); break;
        case UiAction::DungeonManagerLink: LinkDungeonFileToSelectedPoi(); break;
        case UiAction::EncounterAddCreature: AddCreatureFileToEncounter(); break;
        case UiAction::EncounterLoadFile: LoadEncounterFileIntoBuilder(); break;
        case UiAction::EncounterRun: StartEncounterRunner(); break;
        case UiAction::EncounterRollTable: RollActiveEncounterTable(); break;
        case UiAction::EncounterRunnerPrevious:
        case UiAction::EncounterRunnerNext:
        case UiAction::EncounterRunnerRound:
        case UiAction::EncounterRunnerDamage:
        case UiAction::EncounterRunnerHeal:
        case UiAction::EncounterRunnerToggleDefeated:
            if (gRunningEncounterIndex >= 0 &&
                gRunningEncounterIndex < static_cast<int>(gEncounters.size())) {
                Encounter &encounter = gEncounters[static_cast<size_t>(gRunningEncounterIndex)];
                if (hit.action == UiAction::EncounterRunnerRound) {
                    campaign_tools::AdvanceRound(encounter);
                } else if (hit.action == UiAction::EncounterRunnerPrevious)
                    campaign_tools::AdvanceTurn(encounter, -1);
                else if (hit.action == UiAction::EncounterRunnerNext)
                    campaign_tools::AdvanceTurn(encounter, 1);
                else if (hit.action == UiAction::EncounterRunnerDamage)
                    campaign_tools::AdjustActiveHitPoints(encounter, -1);
                else if (hit.action == UiAction::EncounterRunnerHeal)
                    campaign_tools::AdjustActiveHitPoints(encounter, 1);
                else
                    campaign_tools::ToggleActiveDefeated(encounter);
                MarkProjectDirty();
            }
            break;
        case UiAction::EncounterAddTable: {
            EncounterTableDraft table;
            table.rows.push_back({});
            gEncounterTableDrafts.push_back(std::move(table));
            gEncounterActiveTable = static_cast<int>(gEncounterTableDrafts.size()) - 1;
            gEncounterTableRowPage = 0;
            gEncounterSelectedTableRow = 0;
            gModalField = 100;
            gModalError.clear();
            break;
        }
        case UiAction::EncounterDeleteTable:
            if (!gEncounterTableDrafts.empty()) {
                gEncounterTableDrafts.erase(gEncounterTableDrafts.begin() + gEncounterActiveTable);
                gEncounterActiveTable = std::max(0, std::min(gEncounterActiveTable,
                    static_cast<int>(gEncounterTableDrafts.size()) - 1));
                gEncounterTableRowPage = 0;
                gEncounterSelectedTableRow = -1;
                gModalField = gEncounterTableDrafts.empty() ? 0 : 100;
                gModalError.clear();
            }
            break;
        case UiAction::EncounterPreviousTable:
            if (!gEncounterTableDrafts.empty()) {
                int count = static_cast<int>(gEncounterTableDrafts.size());
                gEncounterActiveTable = (gEncounterActiveTable + count - 1) % count;
                gEncounterTableRowPage = 0;
                gEncounterSelectedTableRow = -1;
                gModalField = 100;
            }
            break;
        case UiAction::EncounterNextTable:
            if (!gEncounterTableDrafts.empty()) {
                gEncounterActiveTable = (gEncounterActiveTable + 1) %
                                        static_cast<int>(gEncounterTableDrafts.size());
                gEncounterTableRowPage = 0;
                gEncounterSelectedTableRow = -1;
                gModalField = 100;
            }
            break;
        case UiAction::EncounterAddTableRow:
            if (!gEncounterTableDrafts.empty()) {
                auto &rows = gEncounterTableDrafts[static_cast<size_t>(gEncounterActiveTable)].rows;
                rows.push_back({});
                gEncounterSelectedTableRow = static_cast<int>(rows.size()) - 1;
                gEncounterTableRowPage = gEncounterSelectedTableRow / 5;
                gModalField = 200 + gEncounterSelectedTableRow * 3;
                gModalError.clear();
            }
            break;
        case UiAction::EncounterDeleteTableRow:
            if (!gEncounterTableDrafts.empty()) {
                auto &rows = gEncounterTableDrafts[static_cast<size_t>(gEncounterActiveTable)].rows;
                if (gEncounterSelectedTableRow >= 0 &&
                    gEncounterSelectedTableRow < static_cast<int>(rows.size())) {
                    rows.erase(rows.begin() + gEncounterSelectedTableRow);
                    if (rows.empty()) gEncounterSelectedTableRow = -1;
                    else gEncounterSelectedTableRow = std::min(
                        gEncounterSelectedTableRow, static_cast<int>(rows.size()) - 1);
                    int pageCount = std::max(1, (static_cast<int>(rows.size()) + 4) / 5);
                    gEncounterTableRowPage = std::min(gEncounterTableRowPage, pageCount - 1);
                    gModalField = gEncounterSelectedTableRow < 0
                                      ? 100 : 200 + gEncounterSelectedTableRow * 3;
                    gModalError.clear();
                }
            }
            break;
        case UiAction::EncounterPreviousRows:
            if (gEncounterTableRowPage > 0) {
                --gEncounterTableRowPage;
                gEncounterSelectedTableRow = -1;
                gModalField = 100;
            }
            break;
        case UiAction::EncounterNextRows:
            if (!gEncounterTableDrafts.empty()) {
                const auto &rows =
                    gEncounterTableDrafts[static_cast<size_t>(gEncounterActiveTable)].rows;
                int pageCount = std::max(1, (static_cast<int>(rows.size()) + 4) / 5);
                if (gEncounterTableRowPage + 1 < pageCount) {
                    ++gEncounterTableRowPage;
                    gEncounterSelectedTableRow = -1;
                    gModalField = 100;
                }
            }
            break;
        case UiAction::CreatureAddAbility: AppendCreatureAbilityTemplate(); break;
        case UiAction::CreatureRemoveAbility: RemoveLastCreatureAbility(); break;
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
    return gMainMenuOpen || gModalType != ModalType::None || mouseX < kUiWidth;
}

bool CursorOverGui() {
    double mouseX = 0.0, mouseY = 0.0;
    glfwGetCursorPos(gWindow, &mouseX, &mouseY);
    if (gMainMenuOpen || gModalType != ModalType::None || mouseX < kUiWidth) return true;
    for (const auto &hit : gUiHits)
        if (mouseX >= hit.x && mouseX <= hit.x + hit.w && mouseY >= hit.y && mouseY <= hit.y + hit.h)
            return true;
    return false;
}

void CharacterCallback(GLFWwindow * /*window*/, unsigned int codepoint) {
    if (gModalType == ModalType::None || gModalFields.empty()) return;
    if (gModalType == ModalType::TerrainEditor && gModalField > 0 &&
        (codepoint < '0' || codepoint > '9'))
        return;
    if (gModalType == ModalType::CreatureBuilder && gModalField >= 5 && gModalField <= 10 &&
        (codepoint < '0' || codepoint > '9'))
        return;
    const bool encounterNumber = gModalType == ModalType::Encounter &&
        (gModalField == 101 ||
         (gModalField >= 200 && (gModalField - 200) % 3 < 2));
    if (encounterNumber && (codepoint < '0' || codepoint > '9')) return;
    size_t limit = 80;
    if (gModalType == ModalType::CreatureBuilder && gModalField >= 5 && gModalField <= 10)
        limit = 2;
    else if (gModalType == ModalType::CreatureBuilder && gModalField > 0)
        limit = 500;
    else if (gModalType == ModalType::Encounter) {
        if (encounterNumber) limit = 4;
        else if (gModalField >= 200 || (gModalField >= 1 && gModalField <= 3)) limit = 500;
    }
    else if (gModalType == ModalType::DungeonDetails && gModalField == 1)
        limit = 240;
    else if (gModalType == ModalType::TerrainEditor && gModalField > 0) limit = 3;
    std::string *target = gModalType == ModalType::Encounter
                              ? EncounterModalText(gModalField)
                              : (gModalField >= 0 && gModalField < static_cast<int>(gModalFields.size())
                                     ? &gModalFields[static_cast<size_t>(gModalField)] : nullptr);
    if (target && codepoint >= 32 && codepoint <= 126 && target->size() < limit) {
        target->push_back(static_cast<char>(codepoint));
        gModalError.clear();
    }
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
    if (gEditorTab == EditorTab::Dungeon) {
        if (gDungeonPlacementMode != DungeonPlacementMode::None && action == GLFW_PRESS) {
            if (button == GLFW_MOUSE_BUTTON_LEFT) PlaceDungeonSpecialAtCursor();
            else if (button == GLFW_MOUSE_BUTTON_RIGHT)
                gDungeonPlacementMode = DungeonPlacementMode::None;
            return;
        }
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
            } else if (gEditorTab == EditorTab::World && ShowMarkerInfoAtCursor()) {
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
                if (gHistory.StrokeChanged()) ++gSceneRevision;
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
                if (gHistory.StrokeChanged()) ++gSceneRevision;
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
                if (gHistory.StrokeChanged()) ++gSceneRevision;
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
                if (gHistory.StrokeChanged()) ++gSceneRevision;
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
        if (gHistory.StrokeChanged()) ++gSceneRevision;
        return;
    }
    if (gPaintingLeft) PaintAtCursor(ActivePaintValue());
    else if (gPaintingRight) PaintAtCursor(0);
    if ((gPaintingLeft || gPaintingRight) && gHistory.StrokeChanged()) ++gSceneRevision;
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
    gDungeonPlacementMode = DungeonPlacementMode::None;
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
    if (gMainMenuOpen) {
        if (action != GLFW_PRESS) return;
        if (key == GLFW_KEY_1 || key == GLFW_KEY_KP_1)
            HandleUiAction({0.0f, 0.0f, 0.0f, 0.0f, UiAction::MainWorld, 0});
        else if (key == GLFW_KEY_2 || key == GLFW_KEY_KP_2)
            HandleUiAction({0.0f, 0.0f, 0.0f, 0.0f, UiAction::MainDungeon, 0});
        else if (key == GLFW_KEY_3 || key == GLFW_KEY_KP_3)
            HandleUiAction({0.0f, 0.0f, 0.0f, 0.0f, UiAction::MainEncounter, 0});
        else if (key == GLFW_KEY_4 || key == GLFW_KEY_KP_4)
            HandleUiAction({0.0f, 0.0f, 0.0f, 0.0f, UiAction::MainCreature, 0});
        else if (key == GLFW_KEY_5 || key == GLFW_KEY_KP_5 || key == GLFW_KEY_L)
            HandleUiAction({0.0f, 0.0f, 0.0f, 0.0f, UiAction::MainLoad, 0});
        else if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_Q)
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        return;
    }
    if (gModalType != ModalType::None) {
        if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
        if (gModalType == ModalType::DungeonManager) {
            if (key == GLFW_KEY_ESCAPE) CloseModal(false);
            else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) {
                if (gDungeonManagerIndex >= 0 &&
                    gDungeonManagerIndex < static_cast<int>(gDungeons.size())) {
                    gModalType = ModalType::None;
                    OpenDungeonTab(gDungeonManagerIndex);
                } else {
                    OpenNewDungeonForm();
                }
            }
            return;
        }
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
        else if (gModalType == ModalType::Encounter && key == GLFW_KEY_BACKSPACE) {
            std::string *target = EncounterModalText(gModalField);
            if (target && !target->empty()) {
                target->pop_back();
                gModalError.clear();
            }
        } else if (gModalType == ModalType::Encounter &&
                   (key == GLFW_KEY_TAB || key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER)) {
            std::vector<int> fields = EncounterEditableFields();
            auto current = std::find(fields.begin(), fields.end(), gModalField);
            if (current == fields.end() || ++current == fields.end()) gModalField = fields.front();
            else gModalField = *current;
        } else if (key == GLFW_KEY_BACKSPACE && !gModalFields.empty() &&
                   gModalField >= 0 && gModalField < static_cast<int>(gModalFields.size()) &&
                   !gModalFields[static_cast<size_t>(gModalField)].empty()) {
            gModalFields[static_cast<size_t>(gModalField)].pop_back();
            gModalError.clear();
        } else if (key == GLFW_KEY_TAB && !gModalFields.empty())
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

    if (gEditorTab == EditorTab::Dungeon) {
        if (key == GLFW_KEY_S && (mods & GLFW_MOD_CONTROL)) RequestProjectSave();
        else if (key == GLFW_KEY_L && (mods & GLFW_MOD_CONTROL)) RequestProjectLoad();
        else if (key == GLFW_KEY_Z && (mods & GLFW_MOD_CONTROL)) Undo();
        else if (key == GLFW_KEY_Y && (mods & GLFW_MOD_CONTROL)) Redo();
        else if (key == GLFW_KEY_SLASH && (mods & GLFW_MOD_SHIFT)) OpenKeybindHelp();
        else if (key == GLFW_KEY_ESCAPE) ReturnToWorldTab();
        else if (key == GLFW_KEY_HOME) FitDungeonToWindow();
        else if (key == GLFW_KEY_G) gShowGrid = !gShowGrid;
        else if (key == GLFW_KEY_D) OpenDungeonDetailsForm();
        else if (key == GLFW_KEY_E && gPaintMode != PaintMode::Elevation) {
            gDungeonPlacementMode = DungeonPlacementMode::Entrance;
        } else if (key == GLFW_KEY_X) {
            gDungeonPlacementMode = DungeonPlacementMode::Exit;
        } else if ((key == GLFW_KEY_Q || key == GLFW_KEY_E) &&
                   gPaintMode == PaintMode::Elevation) {
            gElevationBrush = std::clamp(gElevationBrush + (key == GLFW_KEY_Q ? -1 : 1),
                                         kMinElevation, kMaxElevation);
            gElevationEditMode = ElevationEditMode::Set;
        } else if (key == GLFW_KEY_T) {
            gPaintMode = gPaintMode == PaintMode::Terrain ? PaintMode::Elevation
                       : gPaintMode == PaintMode::Elevation ? PaintMode::Fog
                                                            : PaintMode::Terrain;
        } else if (key == GLFW_KEY_F11) {
            gPlayerView = !gPlayerView;
            ++gSceneRevision;
        } else if (key == GLFW_KEY_H) {
            gRoundBrush = !gRoundBrush;
        } else if (key == GLFW_KEY_LEFT_BRACKET) {
            gBrushRadius = std::max(0, gBrushRadius - 1);
        } else if (key == GLFW_KEY_RIGHT_BRACKET) {
            gBrushRadius = std::min(kMaxBrushRadius, gBrushRadius + 1);
        } else if (key == GLFW_KEY_C) {
            RequestClearActiveLayer();
        } else if (key == GLFW_KEY_F1) SelectTool(ToolMode::Brush);
        else if (key == GLFW_KEY_F2 || key == GLFW_KEY_F) SelectTool(ToolMode::FloodFill);
        else if (key == GLFW_KEY_F3) SelectTool(ToolMode::Line);
        else if (key == GLFW_KEY_F4) SelectTool(ToolMode::Curve);
        else if (key == GLFW_KEY_F5) SelectTool(ToolMode::Polygon);
        else if (key == GLFW_KEY_F6) SelectTool(ToolMode::Circle);
        else if (key == GLFW_KEY_F7) SelectTool(ToolMode::Scatter);
        else if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9 &&
                 key - GLFW_KEY_0 < static_cast<int>(ActiveTerrainDefinitions().size())) {
            gDungeonBrush = key - GLFW_KEY_0;
            gPaintMode = PaintMode::Terrain;
            gDungeonPlacementMode = DungeonPlacementMode::None;
        }
        UpdateWindowTitle();
        return;
    }

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
        gMainMenuOpen = true;
        gPlacementMode = PlacementMode::None;
    } else if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9 &&
               key - GLFW_KEY_0 < static_cast<int>(gTerrainDefinitions.size())) {
        gBrush = key - GLFW_KEY_0;
        gTerrainPage = gBrush / kTerrainPageSize;
        gPaintMode = PaintMode::Terrain;
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
    if (gMainMenuOpen || gModalType != ModalType::None) return;
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
    render_geometry::AppendTile(vertices, worldCx, worldCy,
                                {gCameraX, gCameraY, gZoom},
                                gEditorTab == EditorTab::World && gHexGrid, color, alpha);
}

void AppendTileOutline(std::vector<float> &vertices, int32_t col, int32_t row,
                       float r, float g, float b) {
    auto [worldCx, worldCy] = TileCenterWorld(col, row);
    render_geometry::AppendTileOutline(vertices, worldCx, worldCy,
                                       {gCameraX, gCameraY, gZoom},
                                       gEditorTab == EditorTab::World && gHexGrid, {r, g, b});
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

void DungeonVisibleBounds(int32_t &minCol, int32_t &minRow, int32_t &maxCol, int32_t &maxRow) {
    grid_geometry::BoundsForWorldRect(gCameraX, gCameraY, gCameraX + gWindowWidth / gZoom,
                                      gCameraY + gWindowHeight / gZoom, false,
                                      minCol, minRow, maxCol, maxRow);
}

void AppendDungeonTile(std::vector<float> &vertices, int32_t col, int32_t row, Vec3 color) {
    float x = static_cast<float>((col * kTileSize - gCameraX) * gZoom);
    float y = static_cast<float>((row * kTileSize - gCameraY) * gZoom);
    float size = static_cast<float>(kTileSize * gZoom);
    AppendUiRect(vertices, x, y, size, size, color);
}

void RebuildDungeonTileMesh(GLuint vbo, GLsizei &outVertexCount) {
    std::vector<float> vertices;
    Dungeon *dungeon = ActiveDungeon();
    if (dungeon) {
        int32_t minCol = 0, minRow = 0, maxCol = 0, maxRow = 0;
        DungeonVisibleBounds(minCol, minRow, maxCol, maxRow);
        auto dungeonElevation = [&](int32_t col, int32_t row) {
            auto it = dungeon->elevation.find(TileKey(col, row));
            return it == dungeon->elevation.end() ? 0 : static_cast<int>(it->second);
        };
        for (const auto &[key, value] : dungeon->tiles) {
            auto [col, row] = TileCoords(key);
            if (col < minCol || col > maxCol || row < minRow || row > maxRow) continue;
            size_t terrainIndex = std::min(static_cast<size_t>(value),
                                           dungeon->terrainDefinitions.size() - 1);
            int elevation = dungeonElevation(col, row);
            Vec3 baseColor = gElevationView ? ElevationBandColor(elevation)
                                            : dungeon->terrainDefinitions[terrainIndex].color;
            float brightness = gElevationView ? 1.0f : 1.0f + elevation * 0.075f;
            float shade = 1.0f;
            if (gShowHillshade) {
                int northwest = dungeonElevation(col - 1, row - 1);
                int southeast = dungeonElevation(col + 1, row + 1);
                shade = std::clamp(1.0f + static_cast<float>(northwest - southeast) * 0.075f,
                                   0.62f, 1.35f);
            }
            Vec3 color{std::clamp(baseColor.r * brightness * shade, 0.0f, 1.0f),
                       std::clamp(baseColor.g * brightness * shade, 0.0f, 1.0f),
                       std::clamp(baseColor.b * brightness * shade, 0.0f, 1.0f)};
            AppendDungeonTile(vertices, col, row, color);
        }
        auto appendSpecial = [&](int32_t col, int32_t row, bool entrance) {
            float x = static_cast<float>((col * kTileSize - gCameraX) * gZoom);
            float y = static_cast<float>((row * kTileSize - gCameraY) * gZoom);
            float size = static_cast<float>(kTileSize * gZoom);
            float inset = size * 0.18f;
            Vec3 color = entrance ? Vec3{0.15f, 0.95f, 0.28f} : Vec3{0.12f, 0.65f, 1.0f};
            AppendUiRect(vertices, x + inset, y + inset, size - inset * 2.0f,
                         size - inset * 2.0f, color);
            AppendUiRect(vertices, x + inset * 1.65f, y + inset * 1.65f,
                         size - inset * 3.3f, size - inset * 3.3f, {0.04f, 0.05f, 0.06f});
        };
        if (dungeon->hasEntrance)
            appendSpecial(dungeon->entranceCol, dungeon->entranceRow, true);
        if (dungeon->hasExit) appendSpecial(dungeon->exitCol, dungeon->exitRow, false);
        static const Vec3 markerColors[] = {
            {0.92f, 0.76f, 0.35f}, {0.95f, 0.28f, 0.18f}, {0.88f, 0.38f, 0.16f},
            {0.98f, 0.78f, 0.18f}, {0.68f, 0.32f, 0.88f}, {0.25f, 0.78f, 0.92f},
            {0.32f, 0.90f, 0.66f}, {0.85f, 0.86f, 0.90f}};
        for (const DungeonMarker &marker : dungeon->markers) {
            if (gPlayerView && marker.gameMasterOnly) continue;
            float x = static_cast<float>((marker.col * kTileSize - gCameraX) * gZoom);
            float y = static_cast<float>((marker.row * kTileSize - gCameraY) * gZoom);
            float size = static_cast<float>(kTileSize * gZoom);
            float inset = size * 0.28f;
            AppendUiRect(vertices, x + inset, y + inset, size - inset * 2.0f,
                         size - inset * 2.0f,
                         markerColors[static_cast<int>(marker.kind)]);
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                 vertices.data(), GL_DYNAMIC_DRAW);
    outVertexCount = static_cast<GLsizei>(vertices.size() / 6);
}

void RebuildDungeonFogOverlay(GLuint vbo, GLsizei &outVertexCount) {
    std::vector<float> vertices;
    Dungeon *dungeon = ActiveDungeon();
    if (dungeon) {
        int32_t minCol = 0, minRow = 0, maxCol = 0, maxRow = 0;
        DungeonVisibleBounds(minCol, minRow, maxCol, maxRow);
        ForEachVisibleStoredTile(dungeon->fog, minCol, minRow, maxCol, maxRow,
            [&](int32_t col, int32_t row, uint8_t /*hidden*/) {
                float x = static_cast<float>((col * kTileSize - gCameraX) * gZoom);
                float y = static_cast<float>((row * kTileSize - gCameraY) * gZoom);
                float size = static_cast<float>(kTileSize * gZoom);
                AppendUiRect(vertices, x, y, size, size, {0.015f, 0.018f, 0.025f},
                             gPlayerView ? 0.985f : 0.58f);
            });
    }
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                 vertices.data(), GL_DYNAMIC_DRAW);
    outVertexCount = static_cast<GLsizei>(vertices.size() / 6);
}

void RebuildDungeonGrid(GLuint vbo, GLsizei &outVertexCount) {
    std::vector<float> vertices;
    int32_t minCol = 0, minRow = 0, maxCol = 0, maxRow = 0;
    DungeonVisibleBounds(minCol, minRow, maxCol, maxRow);
    auto addLine = [&](double worldX0, double worldY0, double worldX1, double worldY1) {
        float x0 = static_cast<float>((worldX0 - gCameraX) * gZoom);
        float y0 = static_cast<float>((worldY0 - gCameraY) * gZoom);
        float x1 = static_cast<float>((worldX1 - gCameraX) * gZoom);
        float y1 = static_cast<float>((worldY1 - gCameraY) * gZoom);
        const float line[] = {x0, y0, 0.24f, 0.27f, 0.31f, 0.72f,
                              x1, y1, 0.24f, 0.27f, 0.31f, 0.72f};
        vertices.insert(vertices.end(), std::begin(line), std::end(line));
    };
    double left = static_cast<double>(minCol) * kTileSize;
    double right = static_cast<double>(maxCol + 1) * kTileSize;
    double top = static_cast<double>(minRow) * kTileSize;
    double bottom = static_cast<double>(maxRow + 1) * kTileSize;
    for (int32_t col = minCol; col <= maxCol + 1; ++col)
        addLine(static_cast<double>(col) * kTileSize, top,
                static_cast<double>(col) * kTileSize, bottom);
    for (int32_t row = minRow; row <= maxRow + 1; ++row)
        addLine(left, static_cast<double>(row) * kTileSize, right,
                static_cast<double>(row) * kTileSize);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                 vertices.data(), GL_DYNAMIC_DRAW);
    outVertexCount = static_cast<GLsizei>(vertices.size() / 6);
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
        size_t terrainIndex = std::min(static_cast<size_t>(terrain), gTerrainDefinitions.size() - 1);
        Vec3 baseColor = gElevationView ? ElevationBandColor(elevation)
                                        : gTerrainDefinitions[terrainIndex].color;
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
    render_geometry::AppendRegularPolygon(vertices, cx, cy, radius, sides, rotation, {r, g, b});
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
    render_geometry::AppendThickWorldLine(vertices, x0, y0, x1, y1, thicknessWorld, color,
                                          {gCameraX, gCameraY, gZoom});
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
    render_geometry::AppendWorldText(vertices, text, worldCenterX, worldTopY, color,
                                     kLabelPixelSize, kLabelGlyphAdvance,
                                     {gCameraX, gCameraY, gZoom});
}

void RebuildDungeonElevationLabels(GLuint vbo, GLsizei &outVertexCount) {
    std::vector<float> vertices;
    Dungeon *dungeon = ActiveDungeon();
    if (dungeon && gElevationView && kTileSize * gZoom >= 20.0) {
        int32_t minCol = 0, minRow = 0, maxCol = 0, maxRow = 0;
        DungeonVisibleBounds(minCol, minRow, maxCol, maxRow);
        ForEachVisibleStoredTile(dungeon->tiles, minCol, minRow, maxCol, maxRow,
            [&](int32_t col, int32_t row, uint8_t /*terrain*/) {
                auto elevationIt = dungeon->elevation.find(TileKey(col, row));
                int elevation = elevationIt == dungeon->elevation.end() ? 0 : elevationIt->second;
                auto [worldX, worldY] = TileCenterWorld(col, row);
                std::string label = elevation > 0 ? "+" + std::to_string(elevation)
                                                  : std::to_string(elevation);
                Vec3 color = elevation >= 2 ? Vec3{0.08f, 0.09f, 0.11f}
                                            : Vec3{0.96f, 0.97f, 1.0f};
                AppendLabelText(vertices, label, worldX, worldY - kLabelPixelSize * 2.5, color);
            });
    }
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                 vertices.data(), GL_DYNAMIC_DRAW);
    outVertexCount = static_cast<GLsizei>(vertices.size() / 6);
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

    if (gEditorTab == EditorTab::World && gHexGrid) {
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

    if (gEditorTab == EditorTab::World && gHexGrid) {
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

    if (gEditorTab == EditorTab::Dungeon &&
        gDungeonPlacementMode != DungeonPlacementMode::None) {
        auto [col, row] = WorldToTile(curWX, curWY);
        Vec3 markerColor = gDungeonPlacementMode == DungeonPlacementMode::Entrance
                               ? Vec3{0.15f, 0.95f, 0.28f}
                               : Vec3{0.12f, 0.65f, 1.0f};
        AppendTileOutline(vertices, col, row, markerColor.r, markerColor.g, markerColor.b);
    } else if (gPlacementMode != PlacementMode::None) {
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
    } else if (gToolMode == ToolMode::Brush || gToolMode == ToolMode::FloodFill ||
               gToolMode == ToolMode::Scatter) {
        auto [centerCol, centerRow] = WorldToTile(curWX, curWY);
        for (int32_t dr = -gBrushRadius; dr <= gBrushRadius; ++dr)
            for (int32_t dc = -gBrushRadius; dc <= gBrushRadius; ++dc)
                if (BrushCovers(centerCol, dc, dr))
                    AppendTileOutline(vertices, centerCol + dc, centerRow + dr, r, g, b);
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
    LOG_INFO("  Keys 0-9          : select one of the first ten terrain brushes");
    LOG_INFO("  Terrain EDIT/NEW  : rename, recolor, or create project terrain types");
    LOG_INFO("  Dungeon POI       : click its overworld marker to create or open its map");
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
    LOG_INFO("  Escape            : cancel the current action or open the main menu");

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
            if (gEditorTab == EditorTab::Dungeon) {
                RebuildDungeonTileMesh(tileMesh.Buffer(), tileMesh.VertexCount());
                RebuildDungeonGrid(gridMesh.Buffer(), gridMesh.VertexCount());
                RebuildDungeonFogOverlay(fogMesh.Buffer(), fogMesh.VertexCount());
                RebuildDungeonElevationLabels(labelMesh.Buffer(), labelMesh.VertexCount());
            } else {
                RebuildVisibleTileMesh(tileMesh.Buffer(), tileMesh.VertexCount());
                RebuildVisibleRegionOverlay(regionMesh.Buffer(), regionMesh.VertexCount());
                RebuildElevationContours(contourMesh.Buffer(), contourMesh.VertexCount());
                RebuildVisibleFogOverlay(fogMesh.Buffer(), fogMesh.VertexCount());
                RebuildVisibleGridLines(gridMesh.Buffer(), gridMesh.VertexCount());
                RebuildCityMarkers(cityMesh.Buffer(), cityMesh.VertexCount());
                RebuildPoiMarkers(poiMesh.Buffer(), poiMesh.VertexCount());
                RebuildRouteMesh(routeMesh.Buffer(), routeMesh.VertexCount());
                RebuildLabelMesh(labelMesh.Buffer(), labelMesh.VertexCount());
            }
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
        if (gEditorTab == EditorTab::Dungeon) {
            if (gShowGrid) gridMesh.Draw(GL_LINES);
            if (gElevationView) labelMesh.Draw(GL_TRIANGLES);
            fogMesh.Draw(GL_TRIANGLES);
            selectionMesh.Draw(GL_LINES);
        } else {
            if (gShowRegions && !gElevationView) regionMesh.Draw(GL_TRIANGLES);
            if (gShowGrid) gridMesh.Draw(GL_LINES);
            contourMesh.Draw(GL_LINES);
            routeMesh.Draw(GL_TRIANGLES);
            cityMesh.Draw(GL_TRIANGLES);
            poiMesh.Draw(GL_TRIANGLES);
            if (gShowLabels) labelMesh.Draw(GL_TRIANGLES);
            fogMesh.Draw(GL_TRIANGLES);
            selectionMesh.Draw(GL_LINES);
        }
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
