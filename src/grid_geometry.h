#pragma once

#include <array>
#include <cstdint>
#include <utility>

namespace grid_geometry {

using TileCoordinate = std::pair<int32_t, int32_t>;
using NeighborList = std::array<TileCoordinate, 8>;

uint64_t Pack(int32_t column, int32_t row);
TileCoordinate Unpack(uint64_t key);
TileCoordinate WorldToTile(double worldX, double worldY, bool hexGrid);
std::pair<double, double> TileCenter(int32_t column, int32_t row, bool hexGrid);
NeighborList Neighbors(int32_t column, int32_t row, bool hexGrid, int &count);
double TileDistance(int32_t startColumn, int32_t startRow, int32_t endColumn, int32_t endRow,
                    bool hexGrid);
void BoundsForWorldRect(double minX, double minY, double maxX, double maxY, bool hexGrid,
                        int32_t &minColumn, int32_t &minRow, int32_t &maxColumn, int32_t &maxRow);

} // namespace grid_geometry

