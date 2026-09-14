#pragma once

#include <cstdint>

struct Glyph3x5 {
    uint8_t rows[5];
};

Glyph3x5 GetGlyph(char character);

