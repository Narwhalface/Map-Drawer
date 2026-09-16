#include "ui_geometry.h"

#include "tiny_font.h"

#include <algorithm>
#include <iterator>

namespace ui_geometry {

void AppendRect(std::vector<float> &vertices, float x, float y, float width, float height,
                const Vec3 &color, float alpha) {
    const float quad[] = {
        x, y, color.r, color.g, color.b, alpha,
        x + width, y, color.r, color.g, color.b, alpha,
        x + width, y + height, color.r, color.g, color.b, alpha,
        x, y, color.r, color.g, color.b, alpha,
        x + width, y + height, color.r, color.g, color.b, alpha,
        x, y + height, color.r, color.g, color.b, alpha,
    };
    vertices.insert(vertices.end(), std::begin(quad), std::end(quad));
}

void AppendText(std::vector<float> &vertices, std::string text, float x, float y, float scale,
                const Vec3 &color, std::size_t maxCharacters) {
    if (text.size() > maxCharacters) text.resize(maxCharacters);
    for (char character : text) {
        const Glyph3x5 glyph = GetGlyph(character);
        for (int row = 0; row < 5; ++row) {
            for (int column = 0; column < 3; ++column) {
                if ((glyph.rows[row] & (1u << (2 - column))) == 0) continue;
                AppendRect(vertices, x + column * scale, y + row * scale,
                           scale, scale, color);
            }
        }
        x += scale * 4.0f;
    }
}

void AppendButton(std::vector<float> &vertices, std::vector<UiHit> &hits,
                  float x, float y, float width, float height, const std::string &label,
                  UiAction action, int value, bool selected) {
    const Vec3 fill = selected ? Vec3{0.30f, 0.48f, 0.68f} : Vec3{0.18f, 0.20f, 0.24f};
    AppendRect(vertices, x, y, width, height, fill);
    AppendRect(vertices, x, y + height - 1.0f, width, 1.0f, {0.38f, 0.41f, 0.47f});
    AppendText(vertices, label, x + 6.0f, y + 7.0f, 1.5f, {0.92f, 0.93f, 0.95f},
               static_cast<std::size_t>(std::max(1.0f, (width - 10.0f) / 6.0f)));
    hits.push_back({x, y, width, height, action, value});
}

} // namespace ui_geometry
