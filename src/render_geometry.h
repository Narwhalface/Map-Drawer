#pragma once

#include "app_types.h"

#include <string>
#include <vector>

namespace render_geometry {

struct CameraView {
    double x = 0.0;
    double y = 0.0;
    double zoom = 1.0;
};

void AppendTile(std::vector<float> &vertices, double worldCenterX, double worldCenterY,
                const CameraView &view, bool hexGrid, const Vec3 &color, float alpha);
void AppendTileOutline(std::vector<float> &vertices, double worldCenterX, double worldCenterY,
                       const CameraView &view, bool hexGrid, const Vec3 &color);
void AppendRegularPolygon(std::vector<float> &vertices, float centerX, float centerY,
                          float radius, int sides, float rotation, const Vec3 &color);
void AppendThickWorldLine(std::vector<float> &vertices, double x0, double y0,
                          double x1, double y1, float thicknessWorld, const Vec3 &color,
                          const CameraView &view);
void AppendWorldText(std::vector<float> &vertices, const std::string &text,
                     double worldCenterX, double worldTopY, const Vec3 &color,
                     float pixelSize, float glyphAdvance, const CameraView &view);

} // namespace render_geometry
