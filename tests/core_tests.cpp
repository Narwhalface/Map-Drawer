#include "app_types.h"
#include "campaign_tools.h"
#include "editor_commands.h"
#include "editor_history.h"
#include "editor_state.h"
#include "encounter_builder.h"
#include "grid_geometry.h"
#include "gl_lite.h"
#include "mesh_buffer.h"
#include "project_document.h"
#include "render_geometry.h"
#include "shader_program.h"
#include "tiny_font.h"
#include "ui_geometry.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

void Check(bool condition, const char *description) {
    if (condition) return;
    std::cerr << "FAILED: " << description << '\n';
    ++gFailures;
}

bool ReplaceFirstInFile(const std::filesystem::path &path, const std::string &from,
                        const std::string &to) {
    std::ifstream input(path, std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
    const std::size_t position = contents.find(from);
    if (position == std::string::npos) return false;
    contents.replace(position, from.size(), to);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return static_cast<bool>(output);
}

void TestGridGeometry() {
    constexpr int32_t column = -1234;
    constexpr int32_t row = 5678;
    Check(grid_geometry::Unpack(grid_geometry::Pack(column, row)) ==
              grid_geometry::TileCoordinate{column, row},
          "packed tile coordinates round-trip");

    for (bool hexGrid : {false, true}) {
        auto center = grid_geometry::TileCenter(column, row, hexGrid);
        Check(grid_geometry::WorldToTile(center.first, center.second, hexGrid) ==
                  grid_geometry::TileCoordinate{column, row},
              "tile centers map back to their source tile");
        int neighborCount = 0;
        (void)grid_geometry::Neighbors(column, row, hexGrid, neighborCount);
        Check(neighborCount == (hexGrid ? 6 : 8), "grid has the expected neighbor count");
    }
    Check(grid_geometry::TileDistance(0, 0, 2, 0, true) == 2.0,
          "hex tile distance is calculated in axial space");
    Check(grid_geometry::TileDistance(0, 0, 3, 4, false) == 5.0,
          "square tile distance uses Euclidean distance");
    int32_t minColumn = 0, minRow = 0, maxColumn = 0, maxRow = 0;
    grid_geometry::BoundsForWorldRect(0.0, 0.0, 64.0, 64.0, false,
                                      minColumn, minRow, maxColumn, maxRow);
    Check(minColumn <= 0 && minRow <= 0 && maxColumn >= 2 && maxRow >= 2,
          "square visible bounds conservatively contain the viewport");
    grid_geometry::BoundsForWorldRect(-64.0, -64.0, 64.0, 64.0, true,
                                      minColumn, minRow, maxColumn, maxRow);
    Check(minColumn < 0 && minRow < 0 && maxColumn > 0 && maxRow > 0,
          "hex visible bounds cover negative and positive coordinates");
}

void TestDomainLookups() {
    Check(std::string(PoiKindName(PoiKind::Temple)) == "Temple", "POI names remain stable");
    Check(std::string(ToolName(ToolMode::TradeRoute)) == "Trade Route", "tool names remain stable");
    Check(GetGlyph('A').rows[2] == 0b111, "tiny font glyph lookup works");
}

void TestTerrainFeature() {
    const auto terrains = DefaultTerrainDefinitions();
    Check(terrains.size() == kTerrainCount, "world terrain palette has every built-in terrain");
    Check(terrains.front().name == "Empty" && terrains[3].name == "Water" &&
              terrains.back().name == "Road",
          "world terrain names retain their file-format indices");
    for (const TerrainDefinition &terrain : terrains) {
        Check(terrain.color.r >= 0.0f && terrain.color.r <= 1.0f &&
                  terrain.color.g >= 0.0f && terrain.color.g <= 1.0f &&
                  terrain.color.b >= 0.0f && terrain.color.b <= 1.0f,
              "world terrain colors stay normalized");
    }

    ProjectDocument document;
    const uint64_t key = grid_geometry::Pack(4, -2);
    EditTarget target{document, nullptr, false};
    Check(editor_commands::SetLayerValue(target, PaintMode::Terrain, key, 7) &&
              editor_commands::GetLayerValue(target, PaintMode::Terrain, key) == 7,
          "terrain tiles can be painted");
    Check(editor_commands::SetLayerValue(target, PaintMode::Terrain, key, 0) &&
              document.terrain.empty(),
          "terrain tiles can be erased");
}

void TestElevationFeature() {
    ProjectDocument document;
    const uint64_t low = grid_geometry::Pack(0, 0);
    const uint64_t high = grid_geometry::Pack(1, 0);
    EditTarget target{document, nullptr, false};
    Check(editor_commands::SetLayerValue(target, PaintMode::Elevation, low, kMinElevation) &&
              editor_commands::SetLayerValue(target, PaintMode::Elevation, high, kMaxElevation),
          "minimum and maximum elevations can be painted");
    Check(editor_commands::GetLayerValue(target, PaintMode::Elevation, low) == kMinElevation &&
              editor_commands::GetLayerValue(target, PaintMode::Elevation, high) == kMaxElevation,
          "signed elevation values remain intact");
    const Vec3 below = ElevationBandColor(kMinElevation - 100);
    const Vec3 minimum = ElevationBandColor(kMinElevation);
    const Vec3 above = ElevationBandColor(kMaxElevation + 100);
    const Vec3 maximum = ElevationBandColor(kMaxElevation);
    Check(below.r == minimum.r && below.g == minimum.g && below.b == minimum.b,
          "elevation colors clamp below the supported range");
    Check(above.r == maximum.r && above.g == maximum.g && above.b == maximum.b,
          "elevation colors clamp above the supported range");
    Check(std::string(ElevationEditModeName(ElevationEditMode::Set)) == "Set" &&
              std::string(ElevationEditModeName(ElevationEditMode::Raise)) == "Raise" &&
              std::string(ElevationEditModeName(ElevationEditMode::Lower)) == "Lower" &&
              std::string(ElevationEditModeName(ElevationEditMode::Flatten)) == "Flatten" &&
              std::string(ElevationEditModeName(ElevationEditMode::Smooth)) == "Smooth",
          "every elevation editing mode is exposed");
}

void TestFogFeature() {
    ProjectDocument document;
    const uint64_t key = grid_geometry::Pack(5, 8);
    EditTarget target{document, nullptr, false};
    Check(editor_commands::SetLayerValue(target, PaintMode::Fog, key, 1) &&
              editor_commands::GetLayerValue(target, PaintMode::Fog, key) == 1,
          "fog can hide a tile");
    Check(editor_commands::SetLayerValue(target, PaintMode::Fog, key, 0) && document.fog.empty(),
          "fog can reveal a tile");
    Check(std::string(PaintModeName(PaintMode::Fog)) == "Fog", "fog mode has a stable label");
}

void TestRegionFeature() {
    ProjectDocument document;
    document.regions[12] = {12, kRegionPalette[2], "Sun Coast", "Mira"};
    const uint64_t key = grid_geometry::Pack(-3, 7);
    EditTarget target{document, nullptr, false};
    Check(editor_commands::SetLayerValue(target, PaintMode::Region, key, 12) &&
              editor_commands::GetLayerValue(target, PaintMode::Region, key) == 12,
          "region membership can be painted");
    Check(document.regions[12].name == "Sun Coast" && document.regions[12].ruler == "Mira",
          "region metadata is retained");
    Check(kRegionPaletteSize >= 10, "region creation has a full color palette");
}

void TestMarkerFeatures() {
    const PoiKind kinds[] = {PoiKind::Dungeon, PoiKind::Ruin, PoiKind::Landmark,
                             PoiKind::Temple, PoiKind::Camp};
    for (PoiKind kind : kinds) {
        const PoiVisual visual = GetPoiVisual(kind);
        Check(std::string(PoiKindName(kind)) != "?", "every POI kind has a label");
        Check(visual.sides >= 3, "every POI kind has renderable marker geometry");
    }
    City city{-2, 4, "Stonehome", "Ada"};
    PointOfInterest poi{1, 9, PoiKind::Temple, "Dawn Shrine", "Hilltop temple"};
    Encounter encounter{3, -8, "Wolf Pack", "Four hungry wolves"};
    Check(city.name == "Stonehome" && city.ruler == "Ada", "city metadata is editable");
    Check(poi.kind == PoiKind::Temple && poi.description == "Hilltop temple",
          "POI type and description are editable");
    Check(encounter.name == "Wolf Pack" && encounter.description == "Four hungry wolves",
          "encounter metadata is editable");
}

void TestRouteFeature() {
    Check(std::string(RouteKindName(RouteKind::River)) == "River" &&
              std::string(RouteKindName(RouteKind::TradeRoute)) == "Trade Route",
          "both route kinds have stable labels");
    Check(RouteThickness(RouteKind::River) > RouteThickness(RouteKind::TradeRoute),
          "rivers render thicker than trade routes");
    const Vec3 river = RouteColor(RouteKind::River);
    const Vec3 trade = RouteColor(RouteKind::TradeRoute);
    Check(river.b > river.r && trade.r > trade.b, "route kinds have distinct colors");
    Route route{RouteKind::TradeRoute, "King's Road", {{0.0, 0.0}, {32.0, 32.0}}};
    Check(route.points.size() == 2, "route paths retain their control points");
}

void TestDungeonFeature() {
    const auto terrains = DefaultDungeonTerrainDefinitions();
    Check(terrains.size() == kDungeonTileKindCount && terrains[0].name == "Erase" &&
              terrains[static_cast<int>(DungeonTileKind::Door)].name == "Door",
          "dungeon palette matches dungeon tile kinds");
    for (int index = 0; index < kDungeonTileKindCount; ++index) {
        const auto kind = static_cast<DungeonTileKind>(index);
        Check(std::string(DungeonTileKindName(kind)) != "?", "every dungeon tile has a label");
    }

    ProjectDocument document;
    Dungeon dungeon;
    EditTarget target{document, &dungeon, true};
    const uint64_t key = grid_geometry::Pack(1, 2);
    Check(editor_commands::SetLayerValue(target, PaintMode::Terrain, key,
                                         static_cast<int>(DungeonTileKind::Wall)) &&
              dungeon.tiles[key] == static_cast<uint8_t>(DungeonTileKind::Wall),
          "dungeon terrain can be painted");
    Check(editor_commands::SetLayerValue(target, PaintMode::Fog, key, 1) &&
              editor_commands::SetLayerValue(target, PaintMode::Elevation, key, -1),
          "dungeon fog and elevation can be painted");
    Check(!editor_commands::SetLayerValue(target, PaintMode::Region, key, 1),
          "dungeons reject overworld regions");

    const DungeonMarkerKind markerKinds[] = {
        DungeonMarkerKind::Room, DungeonMarkerKind::Encounter, DungeonMarkerKind::Trap,
        DungeonMarkerKind::Treasure, DungeonMarkerKind::Secret, DungeonMarkerKind::Stairs,
        DungeonMarkerKind::Portal, DungeonMarkerKind::Note};
    for (std::size_t index = 0; index < std::size(markerKinds); ++index) {
        DungeonMarker marker;
        marker.col = static_cast<int32_t>(index);
        marker.row = -static_cast<int32_t>(index);
        marker.kind = markerKinds[index];
        marker.name = "Marker " + std::to_string(index);
        marker.description = "Dungeon content";
        marker.gameMasterOnly = index % 2 == 0;
        Check(campaign_tools::AddDungeonMarker(dungeon, std::move(marker)),
              "every dungeon marker kind can be added");
    }
    Check(dungeon.markers.size() == std::size(markerKinds) &&
              dungeon.markers[1].kind == DungeonMarkerKind::Encounter &&
              !dungeon.markers[1].gameMasterOnly,
          "dungeon marker positions, kinds, and visibility are retained");
    Check(!campaign_tools::AddDungeonMarker(dungeon, DungeonMarker{}),
          "unnamed dungeon markers are rejected");
}

void TestEncounterBuilder() {
    Encounter encounter;
    std::string error;
    Check(ParseEncounterBuilderFields(
              "3 Goblins; 1 Ogre",
              "A warning bell rings > The gate closes > Reinforcements arrive",
              "Complication D6: 1-2=Nothing happens; 3-4=A patrol arrives; 5-6=The bridge falls || "
              "Treasure d4: 1=Empty; 2-4=Hidden cache",
              encounter, error),
          "encounter builder parses creatures, effect chains, and multiple roll tables");
    Check(encounter.creatures.size() == 2 && encounter.creatures[0].count == 3 &&
              encounter.creatures[1].name == "Ogre",
          "encounter builder keeps optional creature groups structured");
    Check(encounter.effects.size() == 3 &&
              encounter.effects[1].description == "The gate closes",
          "encounter builder preserves ordered effect steps");
    Check(encounter.rollTables.size() == 2 && encounter.rollTables[0].dieSides == 6 &&
              encounter.rollTables[0].entries[1].minimumRoll == 3 &&
              encounter.rollTables[1].entries.back().maximumRoll == 4,
          "encounter tables preserve dice and result ranges");

    Encounter reparsed;
    error.clear();
    Check(ParseEncounterBuilderFields(FormatEncounterCreatures(encounter),
                                      FormatEncounterEffects(encounter),
                                      FormatEncounterTables(encounter), reparsed, error) &&
              reparsed.creatures.size() == 2 && reparsed.effects.size() == 3 &&
              reparsed.rollTables.size() == 2,
          "formatted encounter builder fields parse back into structured data");

    error.clear();
    Check(!ParseEncounterBuilderFields("", "", "Bad d6: 1-4=First; 4-6=Overlap",
                                       reparsed, error) && !error.empty(),
          "overlapping encounter-table ranges are rejected");
    error.clear();
    Check(ParseEncounterBuilderFields("", "A strange light appears", "", reparsed, error) &&
              reparsed.creatures.empty() && reparsed.effects.size() == 1 &&
              reparsed.rollTables.empty(),
          "encounters can be noncombat single events without a roll table");
}

void TestEncounterRunner() {
    Encounter encounter;
    encounter.name = "Bridge Defense";
    encounter.currentRound = 0;
    encounter.activeParticipant = 0;
    encounter.participants = {{"Scout", 18, 12, "", false},
                              {"Ogre", 8, 30, "slowed", false},
                              {"Mage", 14, 9, "concentrating", false}};

    Check(campaign_tools::AdvanceTurn(encounter, 1) && encounter.activeParticipant == 1,
          "encounter runner advances to the next participant");
    Check(campaign_tools::AdvanceTurn(encounter, -1) && encounter.activeParticipant == 0,
          "encounter runner moves to the previous participant");
    Check(campaign_tools::AdvanceTurn(encounter, -1) && encounter.activeParticipant == 2,
          "encounter runner wraps backward through initiative");
    Check(campaign_tools::AdvanceTurn(encounter, 1) && encounter.activeParticipant == 0,
          "encounter runner wraps forward through initiative");
    Check(campaign_tools::AdjustActiveHitPoints(encounter, -3) &&
              encounter.participants[0].currentHitPoints == 9,
          "encounter runner applies damage to the active participant");
    Check(campaign_tools::AdjustActiveHitPoints(encounter, 2) &&
              encounter.participants[0].currentHitPoints == 11,
          "encounter runner heals the active participant");
    Check(campaign_tools::ToggleActiveDefeated(encounter) &&
              encounter.participants[0].defeated,
          "encounter runner toggles defeated state");
    campaign_tools::AdvanceRound(encounter);
    Check(encounter.currentRound == 1 && encounter.activeParticipant == 0,
          "advancing a round normalizes round state and resets initiative position");

    Encounter empty;
    Check(!campaign_tools::AdvanceTurn(empty, 1) &&
              !campaign_tools::AdjustActiveHitPoints(empty, 1) &&
              !campaign_tools::ToggleActiveDefeated(empty),
          "runner controls safely ignore encounters without participants");

    EncounterRollTable table{"Weather", 6,
                             {{1, 2, "Rain"}, {3, 5, "Cloud"}, {6, 6, "Clear"}}};
    const EncounterTableEntry *low = campaign_tools::ResolveRoll(table, 1);
    const EncounterTableEntry *high = campaign_tools::ResolveRoll(table, 6);
    Check(low && low->result == "Rain" && high && high->result == "Clear",
          "direct table rolls resolve boundary values");
    Check(campaign_tools::ResolveRoll(table, 0) == nullptr &&
              campaign_tools::ResolveRoll(table, 7) == nullptr,
          "table rolls outside the die range do not resolve");
    EncounterRollTable gap{"Gaps", 6, {{1, 2, "Low"}, {5, 6, "High"}}};
    Check(campaign_tools::ResolveRoll(gap, 3) == nullptr,
          "uncovered table rolls report no matching result");
}

void TestCreatureBuilderExtensions() {
    CreatureStatBlock creature;
    Check(creature.specialAbilities.empty(),
          "new creatures begin with zero special abilities");
    creature.specialAbilities.push_back({"Pack Tactics", "Advantage near an ally"});
    creature.specialAbilities.push_back({"Keen Hearing", "Advantage on hearing checks"});
    Check(creature.specialAbilities.size() == 2 &&
              creature.specialAbilities[1].name == "Keen Hearing",
          "creatures support any number of named abilities");

    Check(campaign_tools::AbilityModifier(1) == -5 &&
              campaign_tools::AbilityModifier(9) == -1 &&
              campaign_tools::AbilityModifier(10) == 0 &&
              campaign_tools::AbilityModifier(11) == 0 &&
              campaign_tools::AbilityModifier(20) == 5,
          "ability modifiers use tabletop floor division across score boundaries");

    creature.damageVulnerabilities = "radiant";
    creature.damageResistances = "cold; fire";
    creature.damageImmunities = "poison";
    creature.conditionImmunities = "charmed; poisoned";
    creature.proficiencyBonus = "+4";
    creature.passivePerception = "16";
    creature.spellcasting = "Innate spellcasting";
    creature.portraitFile = "tokens/frost_witch.png";
    Check(!creature.damageResistances.empty() && !creature.spellcasting.empty() &&
              creature.portraitFile.find(".png") != std::string::npos,
          "expanded defenses, spellcasting, and token references are retained");
}

void TestMenuAndWorkflowActions() {
    std::vector<float> vertices;
    std::vector<UiHit> hits;
    const UiAction actions[] = {UiAction::MainContinue, UiAction::SaveAs,
                                UiAction::ReturnMainMenu, UiAction::ExportSheet,
                                UiAction::EncounterLoadFile, UiAction::EncounterRun,
                                UiAction::EncounterRollTable, UiAction::DungeonAddMarker,
                                UiAction::DungeonAddEncounter};
    for (std::size_t index = 0; index < std::size(actions); ++index)
        ui_geometry::AppendButton(vertices, hits, 0.0f, static_cast<float>(index) * 24.0f,
                                  120.0f, 22.0f, "ACTION", actions[index],
                                  static_cast<int>(index), false);
    Check(hits.size() == std::size(actions),
          "new menu and workflow controls create hit targets");
    for (std::size_t index = 0; index < std::size(actions); ++index)
        Check(hits[index].action == actions[index] &&
                  hits[index].value == static_cast<int>(index),
              "new workflow hit targets preserve their action and value");
}

void TestToolCatalogue() {
    const ToolMode tools[] = {ToolMode::Brush, ToolMode::FloodFill, ToolMode::Line,
                              ToolMode::Curve, ToolMode::Polygon, ToolMode::Circle,
                              ToolMode::Scatter, ToolMode::River, ToolMode::TradeRoute,
                              ToolMode::Selection, ToolMode::Measure};
    for (ToolMode tool : tools)
        Check(std::string(ToolName(tool)) != "?", "every editor tool has a stable label");
}

void TestEditorHistory() {
    EditorHistory history;
    std::unordered_map<uint64_t, int> values;
    const uint64_t first = grid_geometry::Pack(2, 3);
    const uint64_t second = grid_geometry::Pack(3, 3);
    auto getValue = [&](PaintMode, uint64_t key) {
        auto found = values.find(key);
        return found == values.end() ? 0 : found->second;
    };
    auto setValue = [&](PaintMode, uint64_t key, int value) {
        if (value == 0) values.erase(key);
        else values[key] = value;
    };

    history.BeginStroke(PaintMode::Terrain, 2);
    history.RecordOriginal(first, getValue(PaintMode::Terrain, first));
    values[first] = 4;
    history.RecordOriginal(second, getValue(PaintMode::Terrain, second));
    values[second] = 4;
    Check(history.EndStroke(getValue), "changed stroke enters history");
    Check(history.UndoCount() == 1 && history.RedoCount() == 0,
          "completed stroke populates undo stack");

    auto undo = history.Undo(setValue);
    Check(undo && undo->tileCount == 2 && values.empty(),
          "undo restores all original tile values");
    auto redo = history.Redo(setValue);
    Check(redo && values[first] == 4 && values[second] == 4,
          "redo restores all edited tile values");

    history.Undo(setValue);
    history.BeginStroke(PaintMode::Terrain);
    history.RecordOriginal(first, 0);
    values[first] = 2;
    history.EndStroke(getValue);
    Check(history.RedoCount() == 0, "a new edit after undo invalidates redo history");

    history.BeginStroke(PaintMode::Fog);
    Check(!history.EndStroke(getValue) && history.UndoCount() == 1,
          "unchanged stroke is not added to history");
    history.Clear();
    Check(history.UndoCount() == 0 && history.RedoCount() == 0,
          "history clears at document boundaries");
}

void TestEditorStateLifecycle() {
    EditorState state;
    state.lastSearchQuery = "keep";
    state.lastFoundLabel = "result";
    state.searchMatches.push_back({"City", 1.0, 2.0});
    state.MarkDirty();
    Check(state.dirty && state.lastSearchQuery.empty() && state.searchMatches.empty(),
          "dirtying editor state invalidates cached search results");

    const uint64_t oldRevision = state.sceneRevision;
    state.ResetForLoadedDocument();
    Check(!state.dirty && state.sceneRevision == oldRevision + 1,
          "loading a document resets dirty state and revises the scene");
}

void TestEditorLayerCommands() {
    ProjectDocument document;
    Dungeon dungeon;
    const uint64_t key = grid_geometry::Pack(-4, 9);
    EditTarget world{document, nullptr, false};
    Check(editor_commands::SetLayerValue(world, PaintMode::Terrain, key, 3),
          "world terrain command reports a change");
    Check(editor_commands::GetLayerValue(world, PaintMode::Terrain, key) == 3,
          "world terrain command stores its value");
    Check(!editor_commands::SetLayerValue(world, PaintMode::Terrain, key, 3),
          "setting an unchanged layer value is a no-op");
    Check(editor_commands::SetLayerValue(world, PaintMode::Terrain, key, 0) &&
              document.terrain.empty(),
          "zero erases a world layer value");

    EditTarget dungeonTarget{document, &dungeon, true};
    Check(!editor_commands::SetLayerValue(dungeonTarget, PaintMode::Region, key, 2),
          "dungeons reject the unsupported region layer");
    Check(editor_commands::SetLayerValue(dungeonTarget, PaintMode::Elevation, key, -2) &&
              document.elevation.empty() && dungeon.elevation[key] == -2,
          "dungeon commands remain isolated from world layers");

    EditTarget missingDungeon{document, nullptr, true};
    Check(!editor_commands::SetLayerValue(missingDungeon, PaintMode::Fog, key, 1) &&
              document.fog.empty(),
          "missing dungeon target cannot accidentally edit the world");
}

void TestPresentationGeometry() {
    std::vector<float> vertices;
    ui_geometry::AppendRect(vertices, 2.0f, 3.0f, 10.0f, 5.0f, {1.0f, 0.5f, 0.25f});
    Check(vertices.size() == 36, "UI rectangle emits two triangles");
    Check(vertices[0] == 2.0f && vertices[1] == 3.0f &&
              vertices[12] == 12.0f && vertices[13] == 8.0f,
          "UI rectangle positions are stable");

    vertices.clear();
    ui_geometry::AppendText(vertices, "MAP", 0.0f, 0.0f, 2.0f, {1.0f, 1.0f, 1.0f});
    Check(!vertices.empty() && vertices.size() % 36 == 0,
          "UI text emits complete glyph-pixel quads");
    std::vector<UiHit> hits;
    vertices.clear();
    ui_geometry::AppendButton(vertices, hits, 4.0f, 5.0f, 80.0f, 22.0f, "SAVE",
                              UiAction::Save, 7, true);
    Check(hits.size() == 1 && hits[0].action == UiAction::Save && hits[0].value == 7,
          "UI buttons create matching interaction targets");

    vertices.clear();
    render_geometry::CameraView view{0.0, 0.0, 1.0};
    render_geometry::AppendTile(vertices, 16.0, 16.0, view, false,
                                {0.2f, 0.3f, 0.4f}, 1.0f);
    Check(vertices.size() == 72, "square tile emits four triangle-fan triangles");
    vertices.clear();
    render_geometry::AppendTile(vertices, 16.0, 16.0, view, true,
                                {0.2f, 0.3f, 0.4f}, 1.0f);
    Check(vertices.size() == 108, "hex tile emits six triangle-fan triangles");

    vertices.clear();
    render_geometry::AppendThickWorldLine(vertices, 0.0, 0.0, 32.0, 0.0,
                                          4.0f, {1.0f, 1.0f, 1.0f}, view);
    Check(vertices.size() == 36, "thick route segment emits a quad");

    vertices.clear();
    render_geometry::AppendTileOutline(vertices, 16.0, 16.0, view, true,
                                       {1.0f, 1.0f, 1.0f});
    Check(vertices.size() == 72, "hex grid outline emits six line segments");
    vertices.clear();
    render_geometry::AppendRegularPolygon(vertices, 20.0f, 20.0f, 5.0f, 8, 0.0f,
                                          {0.5f, 0.5f, 0.5f});
    Check(vertices.size() == 144, "marker polygon emits one triangle per side");
    vertices.clear();
    render_geometry::AppendWorldText(vertices, "A", 16.0, 16.0,
                                     {1.0f, 1.0f, 1.0f}, 2.0f, 8.0f, view);
    Check(!vertices.empty(), "map labels emit renderable world-space text");
    vertices.clear();
    render_geometry::AppendThickWorldLine(vertices, 1.0, 1.0, 1.0, 1.0,
                                          4.0f, {1.0f, 1.0f, 1.0f}, view);
    Check(vertices.empty(), "zero-length routes do not emit invalid geometry");
}

void TestProjectRoundTrip() {
    ProjectDocument source;
    source.hexGrid = true;
    source.metresPerElevationLevel = 500;
    source.seaLevel = -1;
    source.contourInterval = 2;
    source.elevationView = true;
    source.terrainDefinitions[2] = {"Deep Woods", {0.05f, 0.22f, 0.08f}};
    source.terrainDefinitions.push_back({"Tundra", {0.72f, 0.80f, 0.84f}});
    source.terrain[grid_geometry::Pack(-2, 7)] = 4;
    source.terrain[grid_geometry::Pack(5, 6)] = 8;
    source.elevation[grid_geometry::Pack(-2, 7)] = 3;
    source.fog[grid_geometry::Pack(8, 9)] = 1;
    source.regionsByTile[grid_geometry::Pack(-2, 7)] = 1;
    source.regions[1] = {1, {0.2f, 0.4f, 0.6f}, "North Reach", "Ada"};
    source.cities.push_back({-2, 7, "Stonehome", "Ada"});
    source.pointsOfInterest.push_back({8, 9, PoiKind::Ruin, "Old Gate", "Collapsed arch"});
    source.pointsOfInterest.push_back(
        {4, 5, PoiKind::Dungeon, "Old Gate Crypt", "Three rooms beneath the ruined arch"});
    source.encounters.push_back({4, -3, "Goblin Ambush", "Six goblins attack from the ridge"});
    source.encounters.back().creatures = {{6, "Goblin", "goblin.txt"}, {1, "Goblin Boss", ""}};
    source.encounters.back().effects = {{"The scouts loose arrows"},
                                        {"The boss charges on round two"}};
    source.encounters.back().rollTables = {
        {"Reinforcements", 6, {{1, 3, "No reinforcements"}, {4, 6, "Two goblins arrive"}}}};
    source.encounters.back().trigger = "The party crosses the ridge";
    source.encounters.back().objective = "Reach the watchtower";
    source.encounters.back().environment = "Rocky highland road";
    source.encounters.back().gameMasterNotes = "The boss retreats below half health";
    source.encounters.back().rewards = "A silver map case";
    source.encounters.back().successOutcome = "The watchtower is secured";
    source.encounters.back().failureOutcome = "The warning beacon is lit";
    source.encounters.back().sourceFile = "encounters/goblin_ambush.txt";
    source.encounters.back().currentRound = 2;
    source.encounters.back().activeParticipant = 0;
    source.encounters.back().participants = {{"Goblin 1", 14, 5, "poisoned", false}};
    Dungeon dungeon;
    dungeon.worldCol = 4;
    dungeon.worldRow = 5;
    dungeon.name = "Old Gate Crypt";
    dungeon.description = "Three rooms beneath the ruined arch";
    dungeon.sourceFile = "linked_crypt.txt";
    dungeon.terrainDefinitions.push_back({"Moss", {0.18f, 0.42f, 0.16f}});
    dungeon.tiles[grid_geometry::Pack(0, 0)] = static_cast<uint8_t>(DungeonTileKind::Floor);
    dungeon.tiles[grid_geometry::Pack(1, 0)] = 6;
    dungeon.elevation[grid_geometry::Pack(1, 0)] = 3;
    dungeon.fog[grid_geometry::Pack(1, 0)] = 1;
    dungeon.hasEntrance = true;
    dungeon.entranceCol = 0;
    dungeon.entranceRow = 0;
    dungeon.hasExit = true;
    dungeon.exitCol = 5;
    dungeon.exitRow = 2;
    dungeon.markers.push_back({2, 1, DungeonMarkerKind::Treasure, "Hidden Cache",
                               "Coins beneath a loose flagstone", "", true});
    dungeon.markers.push_back({3, 1, DungeonMarkerKind::Encounter, "Crypt Guardians",
                               "Skeletons rise", "encounters/crypt_guardians.txt", false});
    source.dungeons.push_back(std::move(dungeon));
    source.routes.push_back({RouteKind::River, "Bluewater", {{1.25, 2.5}, {3.75, 4.0}}});

    std::filesystem::path path = std::filesystem::temp_directory_path() / "map_drawer_round_trip.txt";
    std::string error;
    Check(SaveProjectDocument(path.string(), source, error), "project document saves");

    ProjectDocument loaded;
    int version = 0;
    error.clear();
    Check(LoadProjectDocument(path.string(), loaded, version, error), "project document loads");
    Check(version == kProjectVersion, "project version round-trips");
    Check(loaded.hexGrid == source.hexGrid &&
              loaded.metresPerElevationLevel == source.metresPerElevationLevel &&
              loaded.seaLevel == source.seaLevel &&
              loaded.contourInterval == source.contourInterval &&
              loaded.elevationView == source.elevationView &&
              loaded.showContours == source.showContours &&
              loaded.showHillshade == source.showHillshade,
          "grid and elevation-view settings round-trip");
    Check(loaded.terrain == source.terrain, "terrain layer round-trips");
    Check(loaded.terrainDefinitions.size() == 9 &&
              loaded.terrainDefinitions[2].name == "Deep Woods" &&
              loaded.terrainDefinitions[8].name == "Tundra",
          "custom terrain definitions round-trip");
    Check(loaded.elevation == source.elevation, "elevation layer round-trips");
    Check(loaded.fog == source.fog, "fog layer round-trips");
    Check(loaded.regionsByTile == source.regionsByTile && loaded.regions.size() == 1 &&
              loaded.regions.at(1).name == "North Reach" && loaded.regions.at(1).ruler == "Ada",
          "region layer and metadata round-trip");
    Check(loaded.cities.size() == 1 && loaded.cities[0].name == "Stonehome",
          "city metadata round-trips");
    Check(loaded.pointsOfInterest.size() == 2 &&
              loaded.pointsOfInterest[0].kind == PoiKind::Ruin &&
              loaded.pointsOfInterest[1].kind == PoiKind::Dungeon,
          "POI metadata, including dungeon markers, round-trips");
    Check(loaded.encounters.size() == 1 &&
              loaded.encounters[0].name == "Goblin Ambush" &&
              loaded.encounters[0].description == "Six goblins attack from the ridge" &&
              loaded.encounters[0].creatures.size() == 2 &&
              loaded.encounters[0].creatures[0].sourceFile == "goblin.txt" &&
              loaded.encounters[0].effects.size() == 2 &&
              loaded.encounters[0].rollTables.size() == 1 &&
              loaded.encounters[0].rollTables[0].entries.size() == 2 &&
              loaded.encounters[0].trigger == "The party crosses the ridge" &&
              loaded.encounters[0].objective == "Reach the watchtower" &&
              loaded.encounters[0].environment == "Rocky highland road" &&
              loaded.encounters[0].gameMasterNotes == "The boss retreats below half health" &&
              loaded.encounters[0].rewards == "A silver map case" &&
              loaded.encounters[0].successOutcome == "The watchtower is secured" &&
              loaded.encounters[0].failureOutcome == "The warning beacon is lit" &&
              loaded.encounters[0].sourceFile == "encounters/goblin_ambush.txt" &&
              loaded.encounters[0].currentRound == 2 &&
              loaded.encounters[0].activeParticipant == 0 &&
              loaded.encounters[0].participants.size() == 1 &&
              loaded.encounters[0].participants[0].initiative == 14 &&
              loaded.encounters[0].participants[0].currentHitPoints == 5 &&
              loaded.encounters[0].participants[0].conditions == "poisoned" &&
              !loaded.encounters[0].participants[0].defeated,
          "structured encounter content and run state round-trip");
    Check(loaded.dungeons.size() == 1 && loaded.dungeons[0].name == "Old Gate Crypt" &&
              loaded.dungeons[0].tiles.size() == 2 && loaded.dungeons[0].hasEntrance &&
              loaded.dungeons[0].entranceCol == 0 && loaded.dungeons[0].entranceRow == 0 &&
              loaded.dungeons[0].hasExit && loaded.dungeons[0].exitCol == 5 &&
              loaded.dungeons[0].exitRow == 2 && loaded.dungeons[0].terrainDefinitions.size() == 7 &&
              loaded.dungeons[0].terrainDefinitions[6].name == "Moss" &&
              loaded.dungeons[0].elevation.size() == 1 && loaded.dungeons[0].fog.size() == 1 &&
              loaded.dungeons[0].sourceFile == "linked_crypt.txt" &&
              loaded.dungeons[0].markers.size() == 2 &&
              loaded.dungeons[0].markers[0].kind == DungeonMarkerKind::Treasure &&
              loaded.dungeons[0].markers[0].gameMasterOnly &&
              loaded.dungeons[0].markers[1].kind == DungeonMarkerKind::Encounter &&
              !loaded.dungeons[0].markers[1].gameMasterOnly &&
              loaded.dungeons[0].markers[1].sourceFile == "encounters/crypt_guardians.txt",
          "dungeon terrain, elevation, fog, links, and content markers round-trip");
    Check(loaded.routes.size() == 1 && loaded.routes[0].kind == RouteKind::River &&
              loaded.routes[0].name == "Bluewater" && loaded.routes[0].points.size() == 2 &&
              loaded.routes[0].points[0].first == 1.25 && loaded.routes[0].points[1].second == 4.0,
          "route geometry round-trips");

    ProjectDocument standaloneSource;
    standaloneSource.standaloneDungeon = true;
    Dungeon standaloneDungeon;
    standaloneDungeon.name = "Standalone Crypt";
    standaloneDungeon.tiles[grid_geometry::Pack(2, 3)] =
        static_cast<uint8_t>(DungeonTileKind::Floor);
    standaloneDungeon.markers.push_back({2, 3, DungeonMarkerKind::Secret,
                                         "False Wall", "Leads to the vault", "", true});
    standaloneSource.dungeons.push_back(std::move(standaloneDungeon));
    const std::filesystem::path standalonePath =
        std::filesystem::temp_directory_path() / "map_drawer_standalone_dungeon.txt";
    error.clear();
    Check(SaveProjectDocument(standalonePath.string(), standaloneSource, error),
          "standalone dungeon document saves");
    ProjectDocument standaloneLoaded;
    Check(LoadProjectDocument(standalonePath.string(), standaloneLoaded, version, error) &&
              standaloneLoaded.standaloneDungeon && standaloneLoaded.dungeons.size() == 1 &&
              standaloneLoaded.pointsOfInterest.empty() &&
              standaloneLoaded.dungeons[0].name == "Standalone Crypt" &&
              standaloneLoaded.dungeons[0].markers.size() == 1 &&
              standaloneLoaded.dungeons[0].markers[0].kind == DungeonMarkerKind::Secret,
          "standalone dungeon identity and map round-trip without an overworld POI");

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(standalonePath, ignored);

    ProjectDocument encounterSource;
    encounterSource.standaloneEncounter = true;
    Encounter standaloneEncounter;
    standaloneEncounter.name = "Falling Stars";
    standaloneEncounter.effects = {{"A star strikes the old tower"}};
    standaloneEncounter.trigger = "Midnight on the solstice";
    standaloneEncounter.objective = "Contain the falling star";
    standaloneEncounter.currentRound = 3;
    standaloneEncounter.participants = {{"Star Spawn", 17, 22, "glowing", false}};
    standaloneEncounter.rollTables = {
        {"Impact", 4, {{1, 2, "Harmless sparks"}, {3, 4, "Arcane shockwave"}}}};
    encounterSource.encounters.push_back(std::move(standaloneEncounter));
    const std::filesystem::path encounterPath =
        std::filesystem::temp_directory_path() / "map_drawer_standalone_encounter.txt";
    error.clear();
    Check(SaveProjectDocument(encounterPath.string(), encounterSource, error),
          "standalone encounter document saves");
    ProjectDocument encounterLoaded;
    Check(LoadProjectDocument(encounterPath.string(), encounterLoaded, version, error) &&
              encounterLoaded.standaloneEncounter && encounterLoaded.encounters.size() == 1 &&
              encounterLoaded.encounters[0].name == "Falling Stars" &&
              encounterLoaded.encounters[0].rollTables.size() == 1 &&
              encounterLoaded.encounters[0].trigger == "Midnight on the solstice" &&
              encounterLoaded.encounters[0].objective == "Contain the falling star" &&
              encounterLoaded.encounters[0].currentRound == 3 &&
              encounterLoaded.encounters[0].participants.size() == 1 &&
              encounterLoaded.encounters[0].participants[0].name == "Star Spawn",
          "standalone encounter identity, builder data, and run state round-trip");
    std::filesystem::remove(encounterPath, ignored);

    ProjectDocument creatureSource;
    creatureSource.standaloneCreature = true;
    CreatureStatBlock creature;
    creature.name = "Ash Drake";
    creature.classification = "Large dragon, neutral";
    creature.armorClass = "17 (natural armor)";
    creature.hitPoints = "95 (10d10+40)";
    creature.speed = "40 ft., fly 80 ft.";
    creature.abilityScores = {19, 14, 18, 8, 13, 12};
    creature.challenge = "6";
    creature.traits = "Heated Body: adjacent creatures take fire damage";
    creature.specialAbilities = {
        {"Ash Step", "Teleport up to 30 feet through smoke"},
        {"Cinder Sight", "See normally through magical smoke"}};
    creature.actions = "Bite: +7 to hit; Ash Breath: recharge 5-6";
    creature.reactions = "Tail Deflection: add 2 AC against one attack";
    creature.legendaryActions = "Wingbeat: move half speed without opportunity attacks";
    creature.damageResistances = "fire";
    creature.damageVulnerabilities = "cold";
    creature.damageImmunities = "poison";
    creature.conditionImmunities = "frightened";
    creature.proficiencyBonus = "+3";
    creature.passivePerception = "14";
    creature.spellcasting = "Innate: produce flame at will";
    creature.portraitFile = "tokens/ash_drake.png";
    creatureSource.creatures.push_back(std::move(creature));
    const std::filesystem::path creaturePath =
        std::filesystem::temp_directory_path() / "map_drawer_creature.txt";
    error.clear();
    Check(SaveProjectDocument(creaturePath.string(), creatureSource, error),
          "standalone creature stat block saves");
    ProjectDocument creatureLoaded;
    Check(LoadProjectDocument(creaturePath.string(), creatureLoaded, version, error) &&
              creatureLoaded.standaloneCreature && creatureLoaded.creatures.size() == 1 &&
              creatureLoaded.creatures[0].name == "Ash Drake" &&
              creatureLoaded.creatures[0].armorClass == "17 (natural armor)" &&
              creatureLoaded.creatures[0].abilityScores[0] == 19 &&
              creatureLoaded.creatures[0].abilityScores[5] == 12 &&
              creatureLoaded.creatures[0].actions.find("Ash Breath") != std::string::npos &&
              creatureLoaded.creatures[0].specialAbilities.size() == 2 &&
              creatureLoaded.creatures[0].specialAbilities[1].name == "Cinder Sight" &&
              creatureLoaded.creatures[0].reactions.find("Tail Deflection") != std::string::npos &&
              creatureLoaded.creatures[0].legendaryActions.find("Wingbeat") != std::string::npos &&
              creatureLoaded.creatures[0].damageVulnerabilities == "cold" &&
              creatureLoaded.creatures[0].damageResistances == "fire" &&
              creatureLoaded.creatures[0].damageImmunities == "poison" &&
              creatureLoaded.creatures[0].conditionImmunities == "frightened" &&
              creatureLoaded.creatures[0].proficiencyBonus == "+3" &&
              creatureLoaded.creatures[0].passivePerception == "14" &&
              creatureLoaded.creatures[0].spellcasting.find("produce flame") != std::string::npos &&
              creatureLoaded.creatures[0].portraitFile == "tokens/ash_drake.png",
          "standalone creature identity and complete stat block round-trip");
    std::filesystem::remove(creaturePath, ignored);
}

void TestVersionFourteenProjectCompatibility() {
    ProjectDocument source;
    Encounter encounter;
    encounter.name = "Old Encounter";
    encounter.trigger = "Version fifteen only";
    source.encounters.push_back(encounter);
    CreatureStatBlock creature;
    creature.name = "Old Creature";
    creature.damageResistances = "fire";
    source.creatures.push_back(creature);
    Dungeon dungeon;
    dungeon.name = "Old Dungeon";
    source.dungeons.push_back(dungeon);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "map_drawer_version_14.txt";
    std::string error;
    Check(SaveProjectDocument(path.string(), source, error),
          "version-fourteen compatibility fixture saves in the current format");
    std::ifstream input(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind("ENCOUNTER_DETAILS ", 0) == 0 ||
            line.rfind("ENCOUNTER_RUN ", 0) == 0 ||
            line.rfind("CREATURE_DETAILS ", 0) == 0 ||
            line.rfind("DUNGEON_MARKERS ", 0) == 0)
            continue;
        lines.push_back(line);
    }
    input.close();
    lines[0] = "MAP_DRAWER_PROJECT 14";
    std::ofstream output(path, std::ios::trunc);
    for (const std::string &savedLine : lines) output << savedLine << '\n';
    output.close();

    ProjectDocument loaded;
    int version = 0;
    error.clear();
    Check(LoadProjectDocument(path.string(), loaded, version, error),
          "version-fourteen projects remain loadable without new sections");
    Check(version == 14 && loaded.encounters.size() == 1 &&
              loaded.encounters[0].name == "Old Encounter" &&
              loaded.encounters[0].trigger.empty() && loaded.creatures.size() == 1 &&
              loaded.creatures[0].damageResistances.empty() && loaded.dungeons.size() == 1 &&
              loaded.dungeons[0].markers.empty(),
          "version-fourteen files receive safe defaults for version-fifteen features");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void TestVersionThreeProjectCompatibility() {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / "map_drawer_version_three.txt";
    {
        std::ofstream output(path);
        output << "MAP_DRAWER_PROJECT 3\n"
                  "GRID 1\n"
                  "ELEVATION_SETTINGS 250 0 1 0 1 1\n"
                  "TERRAIN 1\n0 0 2\n"
                  "ELEVATION 0\n"
                  "FOG 0\n"
                  "REGIONS 0\n"
                  "REGION_TILES 0\n"
                  "CITIES 0\n"
                  "POIS 0\n"
                  "ROUTES 0\n"
                  "END\n";
    }
    ProjectDocument loaded;
    int version = 0;
    std::string error;
    Check(LoadProjectDocument(path.string(), loaded, version, error),
          "version-three projects remain loadable");
    Check(version == 3 && loaded.terrainDefinitions.size() == kTerrainCount,
          "older projects receive the default terrain palette");

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void TestDungeonPoiMigration() {
    ProjectDocument source;
    Dungeon dungeon;
    dungeon.worldCol = -7;
    dungeon.worldRow = 12;
    dungeon.name = "Legacy Cavern";
    dungeon.description = "Created before dungeon POIs became the map entry point";
    source.dungeons.push_back(dungeon);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "map_drawer_dungeon_poi_migration.txt";
    std::string error;
    Check(SaveProjectDocument(path.string(), source, error), "legacy dungeon fixture saves");
    {
        // Strip the v8 per-dungeon layer sections to recreate the version-six layout.
        std::ifstream input(path);
        std::vector<std::string> lines;
        std::string line;
        bool skippingDungeonLayers = false;
        while (std::getline(input, line)) {
            if (line.rfind("DOCUMENT ", 0) == 0) continue;
            if (line.rfind("CREATURES ", 0) == 0) continue;
            if (line.rfind("DUNGEON_TERRAINS ", 0) == 0) {
                skippingDungeonLayers = true;
                continue;
            }
            if (skippingDungeonLayers && line.rfind("ROUTES ", 0) != 0) continue;
            if (line.rfind("ROUTES ", 0) == 0) skippingDungeonLayers = false;
            lines.push_back(line);
        }
        input.close();
        lines[0] = "MAP_DRAWER_PROJECT 6";
        for (size_t index = 0; index + 1 < lines.size(); ++index) {
            if (lines[index].rfind("DUNGEONS ", 0) == 0 &&
                lines[index + 1].size() >= 3 &&
                lines[index + 1].compare(lines[index + 1].size() - 3, 3, " \"\"") == 0) {
                lines[index + 1].resize(lines[index + 1].size() - 3);
                break;
            }
        }
        std::ofstream output(path, std::ios::trunc);
        for (const std::string &savedLine : lines) output << savedLine << '\n';
    }

    ProjectDocument loaded;
    int version = 0;
    error.clear();
    Check(LoadProjectDocument(path.string(), loaded, version, error),
          "version-six dungeon project remains loadable");
    Check(version == 6 && loaded.pointsOfInterest.size() == 1 &&
              loaded.pointsOfInterest[0].kind == PoiKind::Dungeon &&
              loaded.pointsOfInterest[0].col == -7 && loaded.pointsOfInterest[0].row == 12 &&
              loaded.pointsOfInterest[0].name == "Legacy Cavern",
          "older dungeon maps receive a matching overworld Dungeon POI");

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void TestProjectValidation() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "map_drawer_invalid_project.txt";
    ProjectDocument loaded;
    int version = 0;
    std::string error;

    {
        std::ofstream output(path);
        output << "MAP_DRAWER_PROJECT " << (kProjectVersion + 1) << "\n";
    }
    Check(!LoadProjectDocument(path.string(), loaded, version, error) && !error.empty(),
          "future project versions are rejected with an error");

    {
        std::ofstream output(path, std::ios::trunc);
        output << "MAP_DRAWER_PROJECT 8\n"
                  "GRID 1\n"
                  "TERRAIN_TYPES 1\n"
                  "2 0 0 \"Invalid Red\"\n";
    }
    error.clear();
    Check(!LoadProjectDocument(path.string(), loaded, version, error) &&
              error == "invalid terrain definition",
          "out-of-range terrain colors are rejected");

    {
        std::ofstream output(path, std::ios::trunc);
        output << "not a map drawer project\n";
    }
    error.clear();
    Check(!LoadProjectDocument(path.string(), loaded, version, error),
          "malformed project headers are rejected");

    ProjectDocument versionFifteen;
    Encounter encounter;
    encounter.name = "Validation Encounter";
    encounter.currentRound = 1;
    encounter.participants = {{"Scout", 10, 5, "", false}};
    versionFifteen.encounters.push_back(encounter);
    CreatureStatBlock creature;
    creature.name = "Validation Creature";
    versionFifteen.creatures.push_back(creature);
    Dungeon dungeon;
    dungeon.name = "Validation Dungeon";
    dungeon.markers.push_back({0, 0, DungeonMarkerKind::Note, "Note", "Text", "", true});
    versionFifteen.dungeons.push_back(dungeon);

    error.clear();
    Check(SaveProjectDocument(path.string(), versionFifteen, error),
          "version-fifteen validation fixture saves");
    Check(ReplaceFirstInFile(path, "ENCOUNTER_RUN 1 0 1", "ENCOUNTER_RUN 1 3 1"),
          "encounter run fixture can be corrupted");
    error.clear();
    Check(!LoadProjectDocument(path.string(), loaded, version, error) &&
              error == "encounter active participant is out of range",
          "out-of-range active encounter participants are rejected");

    error.clear();
    Check(SaveProjectDocument(path.string(), versionFifteen, error),
          "version-fifteen fixture resets after run-state validation");
    Check(ReplaceFirstInFile(path, "10 5 0 \"Scout\"", "10 5 2 \"Scout\""),
          "participant fixture can be corrupted");
    error.clear();
    Check(!LoadProjectDocument(path.string(), loaded, version, error) &&
              error == "invalid encounter participant",
          "invalid defeated-state values are rejected");

    error.clear();
    Check(SaveProjectDocument(path.string(), versionFifteen, error),
          "version-fifteen fixture resets after participant validation");
    Check(ReplaceFirstInFile(path, "0 0 7 1 \"Note\"",
                            "0 0 99 1 \"Note\""),
          "dungeon marker fixture can be corrupted");
    error.clear();
    Check(!LoadProjectDocument(path.string(), loaded, version, error) &&
              error == "invalid dungeon marker",
          "unknown dungeon marker kinds are rejected");

    error.clear();
    Check(SaveProjectDocument(path.string(), versionFifteen, error),
          "version-fifteen fixture resets after marker validation");
    Check(ReplaceFirstInFile(path, "CREATURE_DETAILS ", "BROKEN_CREATURE_DETAILS "),
          "creature details fixture can be corrupted");
    error.clear();
    Check(!LoadProjectDocument(path.string(), loaded, version, error) &&
              error == "invalid CREATURE_DETAILS section",
          "missing version-fifteen creature details are rejected");

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

struct FeatureTest {
    const char *name;
    const char *description;
    void (*run)();
};

struct FeatureResult {
    enum class Status { Pending, Running, Passed, Failed };

    std::string name;
    std::string description;
    Status status = Status::Pending;
};

const FeatureTest kFeatureTests[] = {
    {"grid", "square/hex coordinates, neighbors, bounds, and distance", TestGridGeometry},
    {"terrain", "terrain palettes and tile painting", TestTerrainFeature},
    {"elevation", "signed heights, bands, and elevation modes", TestElevationFeature},
    {"fog", "hide and reveal tile state", TestFogFeature},
    {"regions", "region metadata, palette, and tile assignment", TestRegionFeature},
    {"markers", "cities, POIs, encounters, and POI visuals", TestMarkerFeatures},
    {"routes", "river and trade-route metadata and appearance", TestRouteFeature},
    {"dungeons", "dungeon palettes, layers, and world isolation", TestDungeonFeature},
    {"encounters", "noncombat events, creatures, effect chains, and roll tables", TestEncounterBuilder},
    {"runner", "turns, rounds, hit points, defeated state, and table rolls", TestEncounterRunner},
    {"creatures", "abilities, modifiers, defenses, spells, and token references", TestCreatureBuilderExtensions},
    {"workflows", "main-menu, linking, export, run, and marker UI actions", TestMenuAndWorkflowActions},
    {"tools", "complete editor tool catalogue", TestToolCatalogue},
    {"commands", "generic world/dungeon layer commands", TestEditorLayerCommands},
    {"history", "stroke recording, undo, redo, and reset", TestEditorHistory},
    {"state", "dirty state, scene revision, and search invalidation", TestEditorStateLifecycle},
    {"presentation", "UI hits, glyphs, tiles, markers, routes, and labels", TestPresentationGeometry},
    {"persistence", "version-fifteen full-feature save/load round trip", TestProjectRoundTrip},
    {"compatibility", "v14/v3 loading, safe defaults, and dungeon-POI migration", nullptr},
    {"validation", "invalid and unsupported project rejection", TestProjectValidation},
    {"lookups", "domain labels and tiny-font lookup", TestDomainLookups},
};

void RunCompatibilityTests() {
    TestVersionFourteenProjectCompatibility();
    TestVersionThreeProjectCompatibility();
    TestDungeonPoiMigration();
}

const FeatureTest *FindFeature(const std::string &name) {
    for (const FeatureTest &feature : kFeatureTests)
        if (name == feature.name) return &feature;
    return nullptr;
}

FeatureResult RunFeature(const FeatureTest &feature) {
    const int failuresBefore = gFailures;
    std::cout << "[ RUN  ] " << feature.name << " - " << feature.description << '\n';
    if (feature.run) feature.run();
    else RunCompatibilityTests();
    const bool passed = gFailures == failuresBefore;
    if (passed)
        std::cout << "[ PASS ] " << feature.name << '\n';
    else
        std::cout << "[ FAIL ] " << feature.name << '\n';
    return {feature.name, feature.description,
            passed ? FeatureResult::Status::Passed : FeatureResult::Status::Failed};
}

const char *kTestVertexShader = R"glsl(
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

const char *kTestFragmentShader = R"glsl(
#version 330 core
in vec4 vColor;
out vec4 FragColor;
void main() { FragColor = vColor; }
)glsl";

void AppendVisualPreview(std::vector<float> &vertices, float panelX, float panelWidth,
                         double animationTime) {
    ui_geometry::AppendRect(vertices, panelX, 92.0f, panelWidth, 554.0f,
                            {0.060f, 0.072f, 0.095f});
    ui_geometry::AppendText(vertices, "LIVE VISUAL CHECK", panelX + 22.0f, 112.0f, 2.0f,
                            {0.82f, 0.87f, 0.98f});
    ui_geometry::AppendText(vertices, "TILES AND FOG", panelX + 22.0f, 152.0f, 1.4f,
                            {0.55f, 0.64f, 0.78f});

    const render_geometry::CameraView view{0.0, 0.0, 1.0};
    const double pulse = 0.82 + std::sin(animationTime * 3.0) * 0.12;
    render_geometry::AppendTile(vertices, panelX + 72.0f, 205.0f, view, false,
                                {0.18f, 0.58f, 0.28f}, 1.0f);
    render_geometry::AppendTile(vertices, panelX + 142.0f, 205.0f, view, true,
                                {0.15f, 0.42f, 0.78f}, 1.0f);
    render_geometry::AppendTile(vertices, panelX + 212.0f, 205.0f, view, false,
                                {0.64f, 0.52f, 0.22f}, 1.0f);
    render_geometry::AppendTile(vertices, panelX + 212.0f, 205.0f, view, false,
                                {0.03f, 0.04f, 0.06f}, static_cast<float>(pulse));

    ui_geometry::AppendText(vertices, "MARKERS", panelX + 22.0f, 252.0f, 1.4f,
                            {0.55f, 0.64f, 0.78f});
    const PoiKind markerKinds[] = {PoiKind::Dungeon, PoiKind::Ruin, PoiKind::Temple,
                                   PoiKind::Camp};
    for (int index = 0; index < 4; ++index) {
        const PoiVisual visual = GetPoiVisual(markerKinds[index]);
        render_geometry::AppendRegularPolygon(vertices, panelX + 60.0f + index * 62.0f,
                                              302.0f, 18.0f, visual.sides,
                                              visual.rotation, visual.color);
    }

    ui_geometry::AppendText(vertices, "ROUTES AND LABELS", panelX + 22.0f, 344.0f, 1.4f,
                            {0.55f, 0.64f, 0.78f});
    render_geometry::AppendThickWorldLine(vertices, panelX + 30.0f, 397.0f,
                                          panelX + panelWidth - 30.0f, 377.0f,
                                          RouteThickness(RouteKind::River),
                                          RouteColor(RouteKind::River), view);
    for (int dash = 0; dash < 5; ++dash) {
        const double x0 = panelX + 35.0 + dash * 48.0;
        render_geometry::AppendThickWorldLine(vertices, x0, 425.0, x0 + 26.0, 425.0,
                                              RouteThickness(RouteKind::TradeRoute),
                                              RouteColor(RouteKind::TradeRoute), view);
    }
    render_geometry::AppendWorldText(vertices, "STONEHOME", panelX + panelWidth * 0.5,
                                     448.0f, {0.95f, 0.85f, 0.15f}, 2.5f, 10.0f, view);

    ui_geometry::AppendText(vertices, "ELEVATION", panelX + 22.0f, 490.0f, 1.4f,
                            {0.55f, 0.64f, 0.78f});
    for (int elevation = kMinElevation; elevation <= kMaxElevation; ++elevation) {
        ui_geometry::AppendRect(vertices, panelX + 22.0f + (elevation - kMinElevation) * 20.0f,
                                518.0f, 18.0f, 24.0f, ElevationBandColor(elevation));
    }

    std::vector<UiHit> previewHits;
    ui_geometry::AppendButton(vertices, previewHits, panelX + 22.0f, 570.0f,
                              panelWidth - 44.0f, 34.0f, "UI BUTTON AND HIT TARGET",
                              UiAction::None, 0, true);
    ui_geometry::AppendText(vertices, "PREVIEW USES PRODUCTION GEOMETRY",
                            panelX + 22.0f, 620.0f, 1.25f, {0.52f, 0.60f, 0.70f}, 40);
}

bool ShowResultsWindow(std::vector<FeatureResult> &results) {
    if (!glfwInit()) {
        std::cerr << "Could not initialize GLFW for the test results window\n";
        return false;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 4);
    GLFWwindow *window = glfwCreateWindow(1240, 720, "Map Drawer Live Feature Tests", nullptr, nullptr);
    if (!window) {
        std::cerr << "Could not create the test results window\n";
        glfwTerminate();
        return false;
    }
    glfwMakeContextCurrent(window);
    glfwSetWindowSizeLimits(window, 1000, 680, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwSwapInterval(1);
    glfwSetKeyCallback(window, [](GLFWwindow *target, int key, int, int action, int) {
        if (action == GLFW_PRESS && (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_ENTER))
            glfwSetWindowShouldClose(target, GLFW_TRUE);
    });
    if (!LoadGLFunctions(reinterpret_cast<void *(*)(const char *)>(glfwGetProcAddress))) {
        std::cerr << "Could not load OpenGL functions for the test results window\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        return false;
    }

    glEnable(GL_MULTISAMPLE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    const GLuint program = CreateShaderProgram(kTestVertexShader, kTestFragmentShader);
    const GLint resolutionLocation = glGetUniformLocation(program, "uResolution");
    MeshBuffer mesh;
    mesh.Initialize();

    std::size_t nextFeature = 0;
    int runningFeature = -1;
    double phaseStarted = glfwGetTime();

    while (!glfwWindowShouldClose(window)) {
        const double now = glfwGetTime();
        if (runningFeature >= 0 && now - phaseStarted >= 0.16) {
            results[static_cast<std::size_t>(runningFeature)] =
                RunFeature(kFeatureTests[static_cast<std::size_t>(runningFeature)]);
            ++nextFeature;
            runningFeature = -1;
            phaseStarted = now;
        } else if (runningFeature < 0 && nextFeature < results.size() &&
                   now - phaseStarted >= 0.10) {
            results[nextFeature].status = FeatureResult::Status::Running;
            runningFeature = static_cast<int>(nextFeature);
            phaseStarted = now;
        }

        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);

        std::vector<float> vertices;
        ui_geometry::AppendRect(vertices, 0.0f, 0.0f, static_cast<float>(width),
                                static_cast<float>(height), {0.045f, 0.052f, 0.070f});
        ui_geometry::AppendText(vertices, "MAP DRAWER FEATURE TESTS", 28.0f, 24.0f, 3.0f,
                                {0.92f, 0.94f, 1.0f});
        const std::size_t passedCount = static_cast<std::size_t>(std::count_if(
            results.begin(), results.end(), [](const FeatureResult &result) {
                return result.status == FeatureResult::Status::Passed;
            }));
        const std::size_t completedCount = static_cast<std::size_t>(std::count_if(
            results.begin(), results.end(), [](const FeatureResult &result) {
                return result.status == FeatureResult::Status::Passed ||
                       result.status == FeatureResult::Status::Failed;
            }));
        const bool allPassed = completedCount == results.size() && passedCount == results.size();
        const std::string summary = completedCount == results.size()
            ? std::to_string(passedCount) + " / " + std::to_string(results.size()) +
                  " FEATURE SUITES PASSED"
            : "TESTING " + std::to_string(completedCount) + " / " +
                  std::to_string(results.size());
        const Vec3 summaryColor = allPassed ? Vec3{0.35f, 0.92f, 0.52f}
                                  : completedCount == results.size()
                                      ? Vec3{0.95f, 0.32f, 0.28f}
                                      : Vec3{0.95f, 0.72f, 0.24f};
        ui_geometry::AppendText(vertices, summary, 28.0f, 62.0f, 2.0f, summaryColor);

        const float listWidth = std::min(760.0f, static_cast<float>(width) * 0.62f);
        const float columnGap = 8.0f;
        const float columnWidth = (listWidth - columnGap) * 0.5f;
        const std::size_t rowsPerColumn = (results.size() + 1) / 2;
        for (std::size_t index = 0; index < results.size(); ++index) {
            const FeatureResult &result = results[index];
            const std::size_t column = index / rowsPerColumn;
            const std::size_t row = index % rowsPerColumn;
            const float x = 24.0f + static_cast<float>(column) * (columnWidth + columnGap);
            const float y = 100.0f + static_cast<float>(row) * 42.0f;
            const Vec3 rowColor = index % 2 == 0 ? Vec3{0.085f, 0.10f, 0.13f}
                                                  : Vec3{0.065f, 0.078f, 0.105f};
            ui_geometry::AppendRect(vertices, x, y - 6.0f,
                                    columnWidth, 36.0f, rowColor);
            const char *statusText = "WAIT";
            Vec3 statusColor{0.22f, 0.25f, 0.31f};
            if (result.status == FeatureResult::Status::Running) {
                statusText = "RUN";
                statusColor = {0.68f, 0.46f, 0.10f};
            } else if (result.status == FeatureResult::Status::Passed) {
                statusText = "PASS";
                statusColor = {0.12f, 0.48f, 0.25f};
            } else if (result.status == FeatureResult::Status::Failed) {
                statusText = "FAIL";
                statusColor = {0.58f, 0.12f, 0.10f};
            }
            ui_geometry::AppendRect(vertices, x + 10.0f, y, 54.0f, 18.0f,
                                    statusColor);
            ui_geometry::AppendText(vertices, statusText,
                                    x + 17.0f, y + 4.0f, 1.4f, {1.0f, 1.0f, 1.0f});
            ui_geometry::AppendText(vertices, result.name, x + 76.0f, y, 1.7f,
                                    {0.88f, 0.91f, 0.98f}, 20);
            ui_geometry::AppendText(vertices, result.description, x + 76.0f, y + 19.0f, 1.0f,
                                    {0.67f, 0.72f, 0.80f}, 38);
        }
        AppendVisualPreview(vertices, listWidth + 48.0f,
                            static_cast<float>(width) - listWidth - 72.0f, now);
        const std::string footer = completedCount == results.size()
            ? "TESTS COMPLETE - WINDOW WILL STAY OPEN UNTIL YOU CLOSE IT"
            : "TESTS ARE RUNNING - PLEASE WAIT";
        ui_geometry::AppendText(vertices, footer, 28.0f,
                                static_cast<float>(height) - 34.0f, 1.7f,
                                completedCount == results.size()
                                    ? Vec3{0.58f, 0.78f, 0.66f}
                                    : Vec3{0.58f, 0.63f, 0.72f}, 68);

        glBindBuffer(GL_ARRAY_BUFFER, mesh.Buffer());
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                     vertices.data(), GL_DYNAMIC_DRAW);
        mesh.VertexCount() = static_cast<GLsizei>(vertices.size() / 6);
        glClearColor(0.045f, 0.052f, 0.070f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(program);
        glUniform2f(resolutionLocation, static_cast<float>(width), static_cast<float>(height));
        mesh.Draw(GL_TRIANGLES);
        glfwSwapBuffers(window);
        glfwWaitEventsTimeout(1.0 / 60.0);
    }

    mesh.Release();
    glDeleteProgram(program);
    glfwDestroyWindow(window);
    glfwTerminate();
    return true;
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--list") {
        for (const FeatureTest &feature : kFeatureTests)
            std::cout << feature.name << "\t" << feature.description << '\n';
        return 0;
    }

    std::vector<FeatureResult> results;
    bool showWindow = false;
    if (argc == 3 && std::string(argv[1]) == "--feature") {
        const FeatureTest *feature = FindFeature(argv[2]);
        if (!feature) {
            std::cerr << "Unknown feature: " << argv[2] << "\nUse --list to see valid names.\n";
            return 2;
        }
        results.push_back(RunFeature(*feature));
    } else if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--headless")) {
        showWindow = argc == 1;
        for (const FeatureTest &feature : kFeatureTests) {
            if (showWindow)
                results.push_back({feature.name, feature.description,
                                   FeatureResult::Status::Pending});
            else
                results.push_back(RunFeature(feature));
        }
    } else {
        std::cerr << "Usage: MapDrawerCoreTests [--headless | --list | --feature <name>]\n";
        return 2;
    }

    if (showWindow) {
        if (!ShowResultsWindow(results)) return 3;
        // Closing the dashboard early must not turn a test run into a partial run.
        for (std::size_t index = 0; index < results.size(); ++index) {
            if (results[index].status == FeatureResult::Status::Pending ||
                results[index].status == FeatureResult::Status::Running)
                results[index] = RunFeature(kFeatureTests[index]);
        }
    }

    if (gFailures != 0) {
        std::cerr << gFailures << " feature assertion(s) failed\n";
        return 1;
    }
    std::cout << "All selected feature tests passed\n";
    return 0;
}
