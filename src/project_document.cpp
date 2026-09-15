#include "project_document.h"

#include "grid_geometry.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <utility>

namespace {

template <typename Layer>
void WriteTileLayer(std::ostream &output, const char *name, const Layer &layer) {
    output << name << ' ' << layer.size() << '\n';
    for (const auto &[key, value] : layer) {
        auto [column, row] = grid_geometry::Unpack(key);
        output << column << ' ' << row << ' ' << static_cast<int>(value) << '\n';
    }
}

bool ReadSectionCount(std::istream &input, const char *expectedName, std::size_t &count) {
    std::string actualName;
    input >> actualName >> count;
    return static_cast<bool>(input) && actualName == expectedName;
}

template <typename Layer>
bool ReadUnsignedTileLayer(std::istream &input, const char *name, Layer &layer, int maximumValue) {
    std::size_t count = 0;
    if (!ReadSectionCount(input, name, count)) return false;
    layer.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        int32_t column = 0;
        int32_t row = 0;
        int value = 0;
        input >> column >> row >> value;
        if (!input || value <= 0 || value > maximumValue) return false;
        layer[grid_geometry::Pack(column, row)] = static_cast<uint8_t>(value);
    }
    return true;
}

} // namespace

bool SaveProjectDocument(const std::string &path, const ProjectDocument &document,
                         std::string &errorMessage) {
    std::ofstream output(path);
    if (!output) {
        errorMessage = "could not open file for writing";
        return false;
    }
    output << std::setprecision(17);
    output << "MAP_DRAWER_PROJECT " << kProjectVersion << '\n';
    output << "GRID " << (document.hexGrid ? 1 : 0) << '\n';
    output << "TERRAIN_TYPES " << document.terrainDefinitions.size() << '\n';
    for (const TerrainDefinition &terrain : document.terrainDefinitions) {
        output << terrain.color.r << ' ' << terrain.color.g << ' ' << terrain.color.b << ' '
               << std::quoted(terrain.name) << '\n';
    }
    output << "ELEVATION_SETTINGS " << document.metresPerElevationLevel << ' '
           << document.seaLevel << ' ' << document.contourInterval << ' '
           << (document.elevationView ? 1 : 0) << ' ' << (document.showContours ? 1 : 0) << ' '
           << (document.showHillshade ? 1 : 0) << '\n';
    WriteTileLayer(output, "TERRAIN", document.terrain);
    WriteTileLayer(output, "ELEVATION", document.elevation);
    WriteTileLayer(output, "FOG", document.fog);

    output << "REGIONS " << document.regions.size() << '\n';
    for (const auto &[id, region] : document.regions) {
        (void)id;
        output << region.id << ' ' << region.color.r << ' ' << region.color.g << ' '
               << region.color.b << ' ' << std::quoted(region.name) << ' '
               << std::quoted(region.ruler) << '\n';
    }
    WriteTileLayer(output, "REGION_TILES", document.regionsByTile);

    output << "CITIES " << document.cities.size() << '\n';
    for (const City &city : document.cities) {
        output << city.col << ' ' << city.row << ' ' << std::quoted(city.name) << ' '
               << std::quoted(city.ruler) << '\n';
    }

    output << "POIS " << document.pointsOfInterest.size() << '\n';
    for (const PointOfInterest &poi : document.pointsOfInterest) {
        output << poi.col << ' ' << poi.row << ' ' << static_cast<int>(poi.kind) << ' '
               << std::quoted(poi.name) << ' ' << std::quoted(poi.description) << '\n';
    }

    output << "ENCOUNTERS " << document.encounters.size() << '\n';
    for (const Encounter &encounter : document.encounters) {
        output << encounter.col << ' ' << encounter.row << ' ' << std::quoted(encounter.name) << ' '
               << std::quoted(encounter.description) << '\n';
    }

    output << "ROUTES " << document.routes.size() << '\n';
    for (const Route &route : document.routes) {
        output << static_cast<int>(route.kind) << ' ' << route.points.size() << ' '
               << std::quoted(route.name) << '\n';
        for (const auto &[x, y] : route.points) output << x << ' ' << y << '\n';
    }
    output << "END\n";

    if (!output) {
        errorMessage = "write failed before the document was complete";
        return false;
    }
    return true;
}

bool LoadProjectDocument(const std::string &path, ProjectDocument &document,
                         int &loadedVersion, std::string &errorMessage) {
    std::ifstream input(path);
    if (!input) {
        errorMessage = "could not open file for reading";
        return false;
    }

    ProjectDocument loaded;
    std::string tag;
    input >> tag >> loadedVersion;
    if (!input || tag != "MAP_DRAWER_PROJECT" || loadedVersion < 1 ||
        loadedVersion > kProjectVersion) {
        errorMessage = "unsupported or invalid project header";
        return false;
    }

    int gridValue = 0;
    input >> tag >> gridValue;
    if (!input || tag != "GRID" || (gridValue != 0 && gridValue != 1)) {
        errorMessage = "invalid GRID section";
        return false;
    }
    loaded.hexGrid = gridValue != 0;

    if (loadedVersion >= 5) {
        std::size_t terrainTypeCount = 0;
        if (!ReadSectionCount(input, "TERRAIN_TYPES", terrainTypeCount) || terrainTypeCount == 0 ||
            terrainTypeCount > kMaxTerrainTypes) {
            errorMessage = "invalid TERRAIN_TYPES section";
            return false;
        }
        loaded.terrainDefinitions.clear();
        loaded.terrainDefinitions.reserve(terrainTypeCount);
        for (std::size_t index = 0; index < terrainTypeCount; ++index) {
            TerrainDefinition terrain;
            input >> terrain.color.r >> terrain.color.g >> terrain.color.b >> std::quoted(terrain.name);
            const auto validColor = [](float value) {
                return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
            };
            if (!input || terrain.name.empty() || !validColor(terrain.color.r) ||
                !validColor(terrain.color.g) || !validColor(terrain.color.b)) {
                errorMessage = "invalid terrain definition";
                return false;
            }
            loaded.terrainDefinitions.push_back(std::move(terrain));
        }
    }

    if (loadedVersion >= 3) {
        int elevationView = 0;
        int contours = 0;
        int hillshade = 0;
        input >> tag >> loaded.metresPerElevationLevel >> loaded.seaLevel >> loaded.contourInterval
              >> elevationView >> contours >> hillshade;
        if (!input || tag != "ELEVATION_SETTINGS" || loaded.metresPerElevationLevel <= 0 ||
            loaded.metresPerElevationLevel > 100000 || loaded.seaLevel < kMinElevation ||
            loaded.seaLevel > kMaxElevation || loaded.contourInterval <= 0 ||
            loaded.contourInterval > kMaxElevation - kMinElevation ||
            (elevationView != 0 && elevationView != 1) || (contours != 0 && contours != 1) ||
            (hillshade != 0 && hillshade != 1)) {
            errorMessage = "invalid ELEVATION_SETTINGS section";
            return false;
        }
        loaded.elevationView = elevationView != 0;
        loaded.showContours = contours != 0;
        loaded.showHillshade = hillshade != 0;
    }

    if (!ReadUnsignedTileLayer(input, "TERRAIN", loaded.terrain,
                               static_cast<int>(loaded.terrainDefinitions.size()) - 1)) {
        errorMessage = "invalid TERRAIN section";
        return false;
    }

    std::size_t elevationCount = 0;
    if (!ReadSectionCount(input, "ELEVATION", elevationCount)) {
        errorMessage = "invalid ELEVATION section";
        return false;
    }
    loaded.elevation.reserve(elevationCount);
    for (std::size_t index = 0; index < elevationCount; ++index) {
        int32_t column = 0;
        int32_t row = 0;
        int value = 0;
        input >> column >> row >> value;
        if (!input || value < kMinElevation || value > kMaxElevation || value == 0) {
            errorMessage = "invalid elevation tile";
            return false;
        }
        loaded.elevation[grid_geometry::Pack(column, row)] = static_cast<int8_t>(value);
    }

    if (loadedVersion >= 2 && !ReadUnsignedTileLayer(input, "FOG", loaded.fog, 1)) {
        errorMessage = "invalid FOG section";
        return false;
    }

    std::size_t regionCount = 0;
    if (!ReadSectionCount(input, "REGIONS", regionCount)) {
        errorMessage = "invalid REGIONS section";
        return false;
    }
    for (std::size_t index = 0; index < regionCount; ++index) {
        Region region;
        input >> region.id >> region.color.r >> region.color.g >> region.color.b
              >> std::quoted(region.name) >> std::quoted(region.ruler);
        if (!input || region.id <= 0 || region.id > kMaxRegions) {
            errorMessage = "invalid region metadata";
            return false;
        }
        loaded.regions[region.id] = std::move(region);
    }
    if (!ReadUnsignedTileLayer(input, "REGION_TILES", loaded.regionsByTile, kMaxRegions)) {
        errorMessage = "invalid REGION_TILES section";
        return false;
    }

    std::size_t cityCount = 0;
    if (!ReadSectionCount(input, "CITIES", cityCount)) {
        errorMessage = "invalid CITIES section";
        return false;
    }
    loaded.cities.reserve(cityCount);
    for (std::size_t index = 0; index < cityCount; ++index) {
        City city;
        input >> city.col >> city.row >> std::quoted(city.name) >> std::quoted(city.ruler);
        if (!input) {
            errorMessage = "invalid city data";
            return false;
        }
        loaded.cities.push_back(std::move(city));
    }

    std::size_t poiCount = 0;
    if (!ReadSectionCount(input, "POIS", poiCount)) {
        errorMessage = "invalid POIS section";
        return false;
    }
    loaded.pointsOfInterest.reserve(poiCount);
    for (std::size_t index = 0; index < poiCount; ++index) {
        PointOfInterest poi;
        int kind = 0;
        input >> poi.col >> poi.row >> kind >> std::quoted(poi.name) >> std::quoted(poi.description);
        if (!input || kind < 0 || kind >= kPoiKindCount) {
            errorMessage = "invalid point-of-interest data";
            return false;
        }
        poi.kind = static_cast<PoiKind>(kind);
        loaded.pointsOfInterest.push_back(std::move(poi));
    }

    if (loadedVersion >= 4) {
        std::size_t encounterCount = 0;
        if (!ReadSectionCount(input, "ENCOUNTERS", encounterCount)) {
            errorMessage = "invalid ENCOUNTERS section";
            return false;
        }
        loaded.encounters.reserve(encounterCount);
        for (std::size_t index = 0; index < encounterCount; ++index) {
            Encounter encounter;
            input >> encounter.col >> encounter.row >> std::quoted(encounter.name)
                  >> std::quoted(encounter.description);
            if (!input) {
                errorMessage = "invalid encounter data";
                return false;
            }
            loaded.encounters.push_back(std::move(encounter));
        }
    }

    std::size_t routeCount = 0;
    if (!ReadSectionCount(input, "ROUTES", routeCount)) {
        errorMessage = "invalid ROUTES section";
        return false;
    }
    loaded.routes.reserve(routeCount);
    for (std::size_t index = 0; index < routeCount; ++index) {
        Route route;
        int kind = 0;
        std::size_t pointCount = 0;
        input >> kind >> pointCount >> std::quoted(route.name);
        if (!input || (kind != 0 && kind != 1)) {
            errorMessage = "invalid route data";
            return false;
        }
        route.kind = kind == 0 ? RouteKind::River : RouteKind::TradeRoute;
        route.points.reserve(pointCount);
        for (std::size_t pointIndex = 0; pointIndex < pointCount; ++pointIndex) {
            double x = 0.0;
            double y = 0.0;
            input >> x >> y;
            if (!input) {
                errorMessage = "invalid route point";
                return false;
            }
            route.points.emplace_back(x, y);
        }
        loaded.routes.push_back(std::move(route));
    }

    input >> tag;
    if (!input || tag != "END") {
        errorMessage = "missing END marker";
        return false;
    }

    document = std::move(loaded);
    return true;
}
