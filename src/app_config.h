#pragma once

#include <cstddef>

// Application-wide tuning and file-format constants. Keeping these in one place
// makes limits and persistence defaults discoverable without searching main.cpp.
inline constexpr const char *kLogFile = "map_drawer.log";
inline constexpr float kTileSize = 32.0f;
inline constexpr float kUiWidth = 264.0f;
inline constexpr double kHexRowHeight = kTileSize * 0.8660254037844386;
inline constexpr double kHexColSpacing = kTileSize * 0.75;
inline constexpr int kTerrainCount = 8;
inline constexpr int kMaxTerrainTypes = 256;
inline constexpr double kPanSpeed = 600.0;
inline constexpr double kMinZoom = 0.05;
inline constexpr double kMaxZoom = 8.0;
inline constexpr int kMaxBrushRadius = 6;
inline constexpr int kFloodFillLimit = 200000;
inline constexpr std::size_t kMaxUndoStrokes = 200;
inline constexpr std::size_t kBulkReserveLimit = 2000000;
inline constexpr double kAutosaveIntervalSeconds = 5.0 * 60.0;
inline constexpr double kKilometresPerTile = 10.0;
inline constexpr double kWalkingKilometresPerDay = 40.0;
inline constexpr int kProjectVersion = 5;
inline constexpr int kMinElevation = -4;
inline constexpr int kMaxElevation = 8;
inline constexpr int kDefaultMetresPerElevationLevel = 250;
inline constexpr int kNoPaintValue = -1000;
inline constexpr int kMaxRegions = 255;
inline constexpr float kRegionOverlayAlpha = 0.4f;
inline constexpr float kScatterDensity = 0.35f;
inline constexpr double kPi = 3.14159265358979323846;

inline constexpr const char *kMapFile = "map.txt";
inline constexpr const char *kScreenshotFile = "map_export.bmp";
inline constexpr const char *kRegionsFile = "regions.txt";
inline constexpr const char *kCitiesFile = "cities.txt";
inline constexpr const char *kPoisFile = "pois.txt";
inline constexpr const char *kRoutesFile = "routes.txt";
inline constexpr const char *kElevationFile = "elevation.txt";
inline constexpr const char *kDefaultProjectFile = "map_drawer_project.txt";
inline constexpr const char *kAutosaveFile = "map_drawer_autosave.txt";
inline constexpr const char *kConfigFile = "map_drawer.cfg";
inline constexpr const char *kBackupDirectory = "backups";
