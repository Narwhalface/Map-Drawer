#include "render_geometry.h"

#include "app_config.h"
#include "tiny_font.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <utility>

namespace render_geometry {
namespace {

std::array<std::pair<float, float>, 6> TileCorners(float centerX, float centerY,
                                                   double zoom, bool hexGrid) {
    std::array<std::pair<float, float>, 6> corners{};
    if (hexGrid) {
        const float radius = static_cast<float>(kTileSize * 0.5 * zoom);
        for (int index = 0; index < 6; ++index) {
            const double angle = index * kPi / 3.0;
            corners[static_cast<std::size_t>(index)] =
                {centerX + radius * static_cast<float>(std::cos(angle)),
                 centerY + radius * static_cast<float>(std::sin(angle))};
        }
    } else {
        const float half = static_cast<float>(kTileSize * 0.5 * zoom);
        corners[0] = {centerX - half, centerY - half};
        corners[1] = {centerX + half, centerY - half};
        corners[2] = {centerX + half, centerY + half};
        corners[3] = {centerX - half, centerY + half};
    }
    return corners;
}

} // namespace

void AppendTile(std::vector<float> &vertices, double worldCenterX, double worldCenterY,
                const CameraView &view, bool hexGrid, const Vec3 &color, float alpha) {
    const float centerX = static_cast<float>((worldCenterX - view.x) * view.zoom);
    const float centerY = static_cast<float>((worldCenterY - view.y) * view.zoom);
    if (kTileSize * view.zoom < 3.0) {
        const float half = std::max(0.6f, static_cast<float>(kTileSize * view.zoom * 0.5));
        const float quad[] = {
            centerX - half, centerY - half, color.r, color.g, color.b, alpha,
            centerX + half, centerY - half, color.r, color.g, color.b, alpha,
            centerX + half, centerY + half, color.r, color.g, color.b, alpha,
            centerX - half, centerY - half, color.r, color.g, color.b, alpha,
            centerX + half, centerY + half, color.r, color.g, color.b, alpha,
            centerX - half, centerY + half, color.r, color.g, color.b, alpha,
        };
        vertices.insert(vertices.end(), std::begin(quad), std::end(quad));
        return;
    }
    const int cornerCount = hexGrid ? 6 : 4;
    const auto corners = TileCorners(centerX, centerY, view.zoom, hexGrid);
    for (int index = 0; index < cornerCount; ++index) {
        const auto &a = corners[static_cast<std::size_t>(index)];
        const auto &b = corners[static_cast<std::size_t>((index + 1) % cornerCount)];
        vertices.insert(vertices.end(),
                        {centerX, centerY, color.r, color.g, color.b, alpha,
                         a.first, a.second, color.r, color.g, color.b, alpha,
                         b.first, b.second, color.r, color.g, color.b, alpha});
    }
}

void AppendTileOutline(std::vector<float> &vertices, double worldCenterX, double worldCenterY,
                       const CameraView &view, bool hexGrid, const Vec3 &color) {
    const float centerX = static_cast<float>((worldCenterX - view.x) * view.zoom);
    const float centerY = static_cast<float>((worldCenterY - view.y) * view.zoom);
    const int cornerCount = hexGrid ? 6 : 4;
    const auto corners = TileCorners(centerX, centerY, view.zoom, hexGrid);
    for (int index = 0; index < cornerCount; ++index) {
        const auto &a = corners[static_cast<std::size_t>(index)];
        const auto &b = corners[static_cast<std::size_t>((index + 1) % cornerCount)];
        vertices.insert(vertices.end(), {a.first, a.second, color.r, color.g, color.b, 1.0f,
                                         b.first, b.second, color.r, color.g, color.b, 1.0f});
    }
}

void AppendRegularPolygon(std::vector<float> &vertices, float centerX, float centerY,
                          float radius, int sides, float rotation, const Vec3 &color) {
    for (int index = 0; index < sides; ++index) {
        const float angle0 = rotation + static_cast<float>(index * 2.0 * kPi / sides);
        const float angle1 = rotation + static_cast<float>((index + 1) * 2.0 * kPi / sides);
        vertices.insert(vertices.end(),
                        {centerX, centerY, color.r, color.g, color.b, 1.0f,
                         centerX + std::cos(angle0) * radius,
                         centerY + std::sin(angle0) * radius, color.r, color.g, color.b, 1.0f,
                         centerX + std::cos(angle1) * radius,
                         centerY + std::sin(angle1) * radius, color.r, color.g, color.b, 1.0f});
    }
}

void AppendThickWorldLine(std::vector<float> &vertices, double x0, double y0,
                          double x1, double y1, float thicknessWorld, const Vec3 &color,
                          const CameraView &view) {
    const double deltaX = x1 - x0;
    const double deltaY = y1 - y0;
    const double length = std::hypot(deltaX, deltaY);
    if (length < 1e-6) return;
    const double normalX = -deltaY / length * thicknessWorld * 0.5;
    const double normalY = deltaX / length * thicknessWorld * 0.5;
    const auto toScreen = [&](double worldX, double worldY) {
        return std::pair<float, float>{static_cast<float>((worldX - view.x) * view.zoom),
                                       static_cast<float>((worldY - view.y) * view.zoom)};
    };
    const auto [ax, ay] = toScreen(x0 + normalX, y0 + normalY);
    const auto [bx, by] = toScreen(x1 + normalX, y1 + normalY);
    const auto [cx, cy] = toScreen(x1 - normalX, y1 - normalY);
    const auto [dx, dy] = toScreen(x0 - normalX, y0 - normalY);
    const float quad[] = {
        ax, ay, color.r, color.g, color.b, 1.0f, bx, by, color.r, color.g, color.b, 1.0f,
        cx, cy, color.r, color.g, color.b, 1.0f, ax, ay, color.r, color.g, color.b, 1.0f,
        cx, cy, color.r, color.g, color.b, 1.0f, dx, dy, color.r, color.g, color.b, 1.0f,
    };
    vertices.insert(vertices.end(), std::begin(quad), std::end(quad));
}

void AppendWorldText(std::vector<float> &vertices, const std::string &text,
                     double worldCenterX, double worldTopY, const Vec3 &color,
                     float pixelSize, float glyphAdvance, const CameraView &view) {
    if (text.empty()) return;
    double startX = worldCenterX -
                    (static_cast<float>(text.size()) * glyphAdvance - pixelSize) * 0.5;
    for (char character : text) {
        const Glyph3x5 glyph = GetGlyph(character);
        for (int row = 0; row < 5; ++row) {
            for (int column = 0; column < 3; ++column) {
                if (!((glyph.rows[row] >> (2 - column)) & 1)) continue;
                const double wx0 = startX + column * pixelSize;
                const double wy0 = worldTopY + row * pixelSize;
                const float x0 = static_cast<float>((wx0 - view.x) * view.zoom);
                const float y0 = static_cast<float>((wy0 - view.y) * view.zoom);
                const float x1 = static_cast<float>((wx0 + pixelSize - view.x) * view.zoom);
                const float y1 = static_cast<float>((wy0 + pixelSize - view.y) * view.zoom);
                const float quad[] = {
                    x0, y0, color.r, color.g, color.b, 1.0f, x1, y0, color.r, color.g, color.b, 1.0f,
                    x1, y1, color.r, color.g, color.b, 1.0f, x0, y0, color.r, color.g, color.b, 1.0f,
                    x1, y1, color.r, color.g, color.b, 1.0f, x0, y1, color.r, color.g, color.b, 1.0f,
                };
                vertices.insert(vertices.end(), std::begin(quad), std::end(quad));
            }
        }
        startX += glyphAdvance;
    }
}

} // namespace render_geometry
