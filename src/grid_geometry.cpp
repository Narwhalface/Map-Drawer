#include "grid_geometry.h"

#include "app_config.h"

#include <cmath>
#include <cstdlib>
#include <limits>

namespace grid_geometry {

uint64_t Pack(int32_t column, int32_t row) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(column)) << 32) |
           static_cast<uint32_t>(row);
}

TileCoordinate Unpack(uint64_t key) {
    return {static_cast<int32_t>(key >> 32), static_cast<int32_t>(key & 0xFFFFFFFFu)};
}

std::pair<double, double> TileCenter(int32_t column, int32_t row, bool hexGrid) {
    if (!hexGrid) return {(column + 0.5) * kTileSize, (row + 0.5) * kTileSize};
    double rowOffset = (column % 2 != 0) ? 0.5 : 0.0;
    return {column * kHexColSpacing + kTileSize * 0.5,
            (row + rowOffset + 0.5) * kHexRowHeight};
}

TileCoordinate WorldToTile(double worldX, double worldY, bool hexGrid) {
    if (!hexGrid) {
        return {static_cast<int32_t>(std::floor(worldX / kTileSize)),
                static_cast<int32_t>(std::floor(worldY / kTileSize))};
    }

    int32_t approximateColumn =
        static_cast<int32_t>(std::llround((worldX - kTileSize * 0.5) / kHexColSpacing));
    int32_t bestColumn = approximateColumn;
    int32_t bestRow = 0;
    double bestDistance = std::numeric_limits<double>::max();
    for (int32_t column = approximateColumn - 2; column <= approximateColumn + 2; ++column) {
        double rowOffset = (column % 2 != 0) ? 0.5 : 0.0;
        int32_t approximateRow =
            static_cast<int32_t>(std::llround(worldY / kHexRowHeight - rowOffset - 0.5));
        for (int32_t row = approximateRow - 2; row <= approximateRow + 2; ++row) {
            auto [centerX, centerY] = TileCenter(column, row, true);
            double dx = centerX - worldX;
            double dy = centerY - worldY;
            double distance = dx * dx + dy * dy;
            if (distance < bestDistance) {
                bestDistance = distance;
                bestColumn = column;
                bestRow = row;
            }
        }
    }
    return {bestColumn, bestRow};
}

NeighborList Neighbors(int32_t column, int32_t row, bool hexGrid, int &count) {
    if (!hexGrid) {
        count = 8;
        return {{{column - 1, row - 1}, {column, row - 1}, {column + 1, row - 1},
                 {column - 1, row}, {column + 1, row}, {column - 1, row + 1},
                 {column, row + 1}, {column + 1, row + 1}}};
    }

    count = 6;
    if ((column & 1) == 0) {
        return {{{column, row - 1}, {column + 1, row - 1}, {column + 1, row},
                 {column, row + 1}, {column - 1, row}, {column - 1, row - 1},
                 {column, row}, {column, row}}};
    }
    return {{{column, row - 1}, {column + 1, row}, {column + 1, row + 1},
             {column, row + 1}, {column - 1, row + 1}, {column - 1, row},
             {column, row}, {column, row}}};
}

double TileDistance(int32_t startColumn, int32_t startRow, int32_t endColumn, int32_t endRow,
                    bool hexGrid) {
    if (!hexGrid) {
        return std::hypot(static_cast<double>(endColumn) - startColumn,
                          static_cast<double>(endRow) - startRow);
    }
    auto axialRow = [](int32_t column, int32_t row) {
        return row - (column - (column & 1)) / 2;
    };
    int64_t q1 = startColumn;
    int64_t r1 = axialRow(startColumn, startRow);
    int64_t q2 = endColumn;
    int64_t r2 = axialRow(endColumn, endRow);
    int64_t dq = q2 - q1;
    int64_t dr = r2 - r1;
    return static_cast<double>((std::llabs(dq) + std::llabs(dr) + std::llabs(dq + dr)) / 2);
}

void BoundsForWorldRect(double minX, double minY, double maxX, double maxY, bool hexGrid,
                        int32_t &minColumn, int32_t &minRow, int32_t &maxColumn, int32_t &maxRow) {
    if (!hexGrid) {
        minColumn = static_cast<int32_t>(std::floor(minX / kTileSize)) - 1;
        minRow = static_cast<int32_t>(std::floor(minY / kTileSize)) - 1;
        maxColumn = static_cast<int32_t>(std::floor(maxX / kTileSize)) + 1;
        maxRow = static_cast<int32_t>(std::floor(maxY / kTileSize)) + 1;
        return;
    }
    minColumn = static_cast<int32_t>(std::floor((minX - kTileSize) / kHexColSpacing)) - 1;
    maxColumn = static_cast<int32_t>(std::ceil((maxX + kTileSize) / kHexColSpacing)) + 1;
    minRow = static_cast<int32_t>(std::floor((minY - kHexRowHeight) / kHexRowHeight)) - 1;
    maxRow = static_cast<int32_t>(std::ceil((maxY + kHexRowHeight) / kHexRowHeight)) + 1;
}

} // namespace grid_geometry

