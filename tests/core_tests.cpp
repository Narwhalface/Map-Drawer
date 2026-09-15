#include "app_types.h"
#include "grid_geometry.h"
#include "project_document.h"
#include "tiny_font.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int gFailures = 0;

void Check(bool condition, const char *description) {
    if (condition) return;
    std::cerr << "FAILED: " << description << '\n';
    ++gFailures;
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
}

void TestDomainLookups() {
    Check(std::string(PoiKindName(PoiKind::Temple)) == "Temple", "POI names remain stable");
    Check(std::string(ToolName(ToolMode::TradeRoute)) == "Trade Route", "tool names remain stable");
    Check(GetGlyph('A').rows[2] == 0b111, "tiny font glyph lookup works");
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
    Dungeon dungeon;
    dungeon.worldCol = 4;
    dungeon.worldRow = 5;
    dungeon.name = "Old Gate Crypt";
    dungeon.description = "Three rooms beneath the ruined arch";
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
    Check(loaded.terrain == source.terrain, "terrain layer round-trips");
    Check(loaded.terrainDefinitions.size() == 9 &&
              loaded.terrainDefinitions[2].name == "Deep Woods" &&
              loaded.terrainDefinitions[8].name == "Tundra",
          "custom terrain definitions round-trip");
    Check(loaded.elevation == source.elevation, "elevation layer round-trips");
    Check(loaded.fog == source.fog, "fog layer round-trips");
    Check(loaded.cities.size() == 1 && loaded.cities[0].name == "Stonehome",
          "city metadata round-trips");
    Check(loaded.pointsOfInterest.size() == 2 &&
              loaded.pointsOfInterest[0].kind == PoiKind::Ruin &&
              loaded.pointsOfInterest[1].kind == PoiKind::Dungeon,
          "POI metadata, including dungeon markers, round-trips");
    Check(loaded.encounters.size() == 1 &&
              loaded.encounters[0].name == "Goblin Ambush" &&
              loaded.encounters[0].description == "Six goblins attack from the ridge",
          "encounter metadata round-trips");
    Check(loaded.dungeons.size() == 1 && loaded.dungeons[0].name == "Old Gate Crypt" &&
              loaded.dungeons[0].tiles.size() == 2 && loaded.dungeons[0].hasEntrance &&
              loaded.dungeons[0].hasExit && loaded.dungeons[0].terrainDefinitions.size() == 7 &&
              loaded.dungeons[0].terrainDefinitions[6].name == "Moss" &&
              loaded.dungeons[0].elevation.size() == 1 && loaded.dungeons[0].fog.size() == 1,
          "dungeon terrain, elevation, fog, and special markers round-trip");
    Check(loaded.routes.size() == 1 && loaded.routes[0].points.size() == 2,
          "route geometry round-trips");

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

} // namespace

int main() {
    TestGridGeometry();
    TestDomainLookups();
    TestProjectRoundTrip();
    TestVersionThreeProjectCompatibility();
    TestDungeonPoiMigration();
    if (gFailures != 0) {
        std::cerr << gFailures << " core test(s) failed\n";
        return 1;
    }
    std::cout << "All core tests passed\n";
    return 0;
}
