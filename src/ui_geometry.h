#pragma once

#include "app_types.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ui_geometry {

void AppendRect(std::vector<float> &vertices, float x, float y, float width, float height,
                const Vec3 &color, float alpha = 1.0f);
void AppendText(std::vector<float> &vertices, std::string text, float x, float y, float scale,
                const Vec3 &color, std::size_t maxCharacters = 64);
void AppendButton(std::vector<float> &vertices, std::vector<UiHit> &hits,
                  float x, float y, float width, float height, const std::string &label,
                  UiAction action, int value = 0, bool selected = false);

} // namespace ui_geometry
