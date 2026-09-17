#pragma once

#include "app_config.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct Vec3 {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

struct TerrainDefinition {
    std::string name;
    Vec3 color{};
};

std::vector<TerrainDefinition> DefaultDungeonTerrainDefinitions();

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

struct EncounterCreature {
    int count = 1;
    std::string name;
    std::string sourceFile;
};

struct EncounterParticipant {
    std::string name;
    int initiative = 0;
    int currentHitPoints = 0;
    std::string conditions;
    bool defeated = false;
};

struct CreatureStatBlock {
    std::string name;
    std::string classification;
    std::string armorClass;
    std::string hitPoints;
    std::string speed;
    std::array<int, 6> abilityScores{10, 10, 10, 10, 10, 10};
    std::string savesAndSkills;
    std::string sensesAndLanguages;
    std::string challenge;
    std::string traits;
    struct Ability {
        std::string name;
        std::string description;
    };
    std::vector<Ability> specialAbilities;
    std::string actions;
    std::string reactions;
    std::string legendaryActions;
    std::string damageVulnerabilities;
    std::string damageResistances;
    std::string damageImmunities;
    std::string conditionImmunities;
    std::string proficiencyBonus;
    std::string passivePerception;
    std::string spellcasting;
    std::string portraitFile;
};

struct EncounterEffect {
    std::string description;
};

struct EncounterTableEntry {
    int minimumRoll = 1;
    int maximumRoll = 1;
    std::string result;
};

struct EncounterRollTable {
    std::string name;
    int dieSides = 6;
    std::vector<EncounterTableEntry> entries;
};

struct Encounter {
    int32_t col = 0;
    int32_t row = 0;
    std::string name;
    std::string description;
    std::vector<EncounterCreature> creatures;
    std::vector<EncounterEffect> effects;
    std::vector<EncounterRollTable> rollTables;
    std::string sourceFile;
    std::string trigger;
    std::string objective;
    std::string environment;
    std::string gameMasterNotes;
    std::string rewards;
    std::string successOutcome;
    std::string failureOutcome;
    int currentRound = 0;
    int activeParticipant = 0;
    std::vector<EncounterParticipant> participants;
};

enum class DungeonTileKind : uint8_t { Empty, Floor, Wall, Door, Water, Trap };
inline constexpr int kDungeonTileKindCount = 6;

enum class DungeonMarkerKind : uint8_t {
    Room, Encounter, Trap, Treasure, Secret, Stairs, Portal, Note
};

struct DungeonMarker {
    int32_t col = 0;
    int32_t row = 0;
    DungeonMarkerKind kind = DungeonMarkerKind::Note;
    std::string name;
    std::string description;
    std::string sourceFile;
    bool gameMasterOnly = true;
};

struct Dungeon {
    int32_t worldCol = 0;
    int32_t worldRow = 0;
    std::string name;
    std::string description;
    std::string sourceFile;
    std::vector<TerrainDefinition> terrainDefinitions = DefaultDungeonTerrainDefinitions();
    std::unordered_map<uint64_t, uint8_t> tiles;
    std::unordered_map<uint64_t, int8_t> elevation;
    std::unordered_map<uint64_t, uint8_t> fog;
    bool hasEntrance = false;
    int32_t entranceCol = 0;
    int32_t entranceRow = 0;
    bool hasExit = false;
    int32_t exitCol = 0;
    int32_t exitRow = 0;
    std::vector<DungeonMarker> markers;
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
enum class ModalType {
    None, Region, City, Poi, Encounter, TerrainEditor, DungeonManager, DungeonDetails,
    DungeonMarker, CreatureBuilder, EncounterRunner, Route, Info, Confirm, ProjectName, Search
};
enum class PlacementMode { None, City, Poi, Encounter };
enum class EditorTab { World, Dungeon };
enum class DungeonPlacementMode { None, Entrance, Exit, Marker, Encounter };
enum class ConfirmAction {
    None, ClearLayer, LoadProject, OverwriteProject, RecoverAutosave, GenerateRelief, ReturnMainMenu
};

enum class UiAction {
    None, MainWorld, MainDungeon, MainEncounter, MainCreature, MainLoad, MainContinue, MainQuit,
    SetMode, SetTool, SetTerrain, EditTerrains, TerrainPagePrevious, TerrainPageNext,
    SetElevationValue, SetElevationTool,
    ElevationDown, ElevationUp, BrushDown, BrushUp, ToggleShape,
    NewRegion, CycleRegion, NewCity, NewPoi, NewEncounter, OpenDungeons, DeleteMarker, DeleteRoute,
    WorldInfo, Help, Find,
    Undo, Redo, Save, SaveAs, Load, ReturnMainMenu, Clear, Export, ExportSheet,
    ToggleGrid, ToggleGeometry, ToggleRegions,
    ToggleLabels, TogglePlayerView, ToggleElevationView, ToggleContours, ToggleHillshade, GenerateRelief,
    FogHideAll, FogRevealAll, ResetCamera, FitMap, ProjectName,
    SelectionCopy, SelectionCut, SelectionPaste, SelectionDelete,
    SelectionPaint, SelectionRegion, SelectionElevationDown, SelectionElevationUp, SelectionClear,
    ModalPrevious, ModalNext, ModalAccept, ModalCancel, ModalPoiKind,
    ModalTerrainPrevious, ModalTerrainNext, ModalTerrainNew,
    DungeonReturnWorld, DungeonSetTile, DungeonPlaceEntrance, DungeonPlaceExit,
    DungeonFit, DungeonEditDetails, DungeonManagerOpen, DungeonManagerNew, DungeonManagerLink,
    DungeonAddMarker, DungeonAddEncounter, DungeonMarkerKind, EncounterAddCreature, EncounterLoadFile,
    EncounterRun, EncounterRollTable, EncounterRunnerPrevious, EncounterRunnerNext,
    EncounterRunnerRound, EncounterRunnerDamage, EncounterRunnerHeal,
    EncounterRunnerToggleDefeated, CreatureAddAbility, CreatureRemoveAbility,
    EncounterAddTable, EncounterDeleteTable, EncounterPreviousTable, EncounterNextTable,
    EncounterAddTableRow, EncounterDeleteTableRow, EncounterPreviousRows, EncounterNextRows
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

std::vector<TerrainDefinition> DefaultTerrainDefinitions();

const char *PoiKindName(PoiKind kind);
const char *DungeonTileKindName(DungeonTileKind kind);
Vec3 DungeonTileKindColor(DungeonTileKind kind);
PoiVisual GetPoiVisual(PoiKind kind);
Vec3 RouteColor(RouteKind kind);
float RouteThickness(RouteKind kind);
const char *RouteKindName(RouteKind kind);
const char *PaintModeName(PaintMode mode);
const char *ElevationEditModeName(ElevationEditMode mode);
Vec3 ElevationBandColor(int elevation);
const char *ToolName(ToolMode mode);
