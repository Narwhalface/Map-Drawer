#pragma once

#include "app_types.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct ProjectDocument {
    std::vector<TerrainDefinition> terrainDefinitions = DefaultTerrainDefinitions();
    std::unordered_map<uint64_t, uint8_t> terrain;
    std::unordered_map<uint64_t, int8_t> elevation;
    std::unordered_map<uint64_t, uint8_t> regionsByTile;
    std::unordered_map<uint64_t, uint8_t> fog;
    std::unordered_map<int, Region> regions;
    std::vector<City> cities;
    std::vector<PointOfInterest> pointsOfInterest;
    std::vector<Encounter> encounters;
    std::vector<Dungeon> dungeons;
    std::vector<Route> routes;

    bool hexGrid = true;
    int metresPerElevationLevel = kDefaultMetresPerElevationLevel;
    int seaLevel = 0;
    int contourInterval = 1;
    bool elevationView = false;
    bool showContours = true;
    bool showHillshade = true;
};

bool SaveProjectDocument(const std::string &path, const ProjectDocument &document,
                         std::string &errorMessage);
bool LoadProjectDocument(const std::string &path, ProjectDocument &document,
                         int &loadedVersion, std::string &errorMessage);
