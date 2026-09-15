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
    source.encounters.push_back({4, -3, "Goblin Ambush", "Six goblins attack from the ridge"});
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
    Check(loaded.pointsOfInterest.size() == 1 &&
              loaded.pointsOfInterest[0].kind == PoiKind::Ruin,
          "POI metadata round-trips");
    Check(loaded.encounters.size() == 1 &&
              loaded.encounters[0].name == "Goblin Ambush" &&
              loaded.encounters[0].description == "Six goblins attack from the ridge",
          "encounter metadata round-trips");
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

} // namespace

int main() {
    TestGridGeometry();
    TestDomainLookups();
    TestProjectRoundTrip();
    TestVersionThreeProjectCompatibility();
    if (gFailures != 0) {
        std::cerr << gFailures << " core test(s) failed\n";
        return 1;
    }
    std::cout << "All core tests passed\n";
    return 0;
}
