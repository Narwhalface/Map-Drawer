#include "tiny_font.h"

#include <cctype>

Glyph3x5 GetGlyph(char character) {
    char c = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    switch (c) {
        case 'A': return {{0b010, 0b101, 0b111, 0b101, 0b101}};
        case 'B': return {{0b110, 0b101, 0b110, 0b101, 0b110}};
        case 'C': return {{0b011, 0b100, 0b100, 0b100, 0b011}};
        case 'D': return {{0b110, 0b101, 0b101, 0b101, 0b110}};
        case 'E': return {{0b111, 0b100, 0b110, 0b100, 0b111}};
        case 'F': return {{0b111, 0b100, 0b110, 0b100, 0b100}};
        case 'G': return {{0b011, 0b100, 0b101, 0b101, 0b011}};
        case 'H': return {{0b101, 0b101, 0b111, 0b101, 0b101}};
        case 'I': return {{0b111, 0b010, 0b010, 0b010, 0b111}};
        case 'J': return {{0b001, 0b001, 0b001, 0b101, 0b010}};
        case 'K': return {{0b101, 0b101, 0b110, 0b101, 0b101}};
        case 'L': return {{0b100, 0b100, 0b100, 0b100, 0b111}};
        case 'M': return {{0b101, 0b111, 0b101, 0b101, 0b101}};
        case 'N': return {{0b101, 0b111, 0b111, 0b111, 0b101}};
        case 'O': return {{0b010, 0b101, 0b101, 0b101, 0b010}};
        case 'P': return {{0b110, 0b101, 0b110, 0b100, 0b100}};
        case 'Q': return {{0b010, 0b101, 0b101, 0b110, 0b011}};
        case 'R': return {{0b110, 0b101, 0b110, 0b101, 0b101}};
        case 'S': return {{0b011, 0b100, 0b010, 0b001, 0b110}};
        case 'T': return {{0b111, 0b010, 0b010, 0b010, 0b010}};
        case 'U': return {{0b101, 0b101, 0b101, 0b101, 0b010}};
        case 'V': return {{0b101, 0b101, 0b101, 0b010, 0b010}};
        case 'W': return {{0b101, 0b101, 0b101, 0b111, 0b101}};
        case 'X': return {{0b101, 0b101, 0b010, 0b101, 0b101}};
        case 'Y': return {{0b101, 0b101, 0b010, 0b010, 0b010}};
        case 'Z': return {{0b111, 0b001, 0b010, 0b100, 0b111}};
        case '0': return {{0b010, 0b101, 0b101, 0b101, 0b010}};
        case '1': return {{0b010, 0b110, 0b010, 0b010, 0b111}};
        case '2': return {{0b110, 0b001, 0b010, 0b100, 0b111}};
        case '3': return {{0b110, 0b001, 0b010, 0b001, 0b110}};
        case '4': return {{0b101, 0b101, 0b111, 0b001, 0b001}};
        case '5': return {{0b111, 0b100, 0b110, 0b001, 0b110}};
        case '6': return {{0b011, 0b100, 0b110, 0b101, 0b010}};
        case '7': return {{0b111, 0b001, 0b010, 0b010, 0b010}};
        case '8': return {{0b010, 0b101, 0b010, 0b101, 0b010}};
        case '9': return {{0b010, 0b101, 0b011, 0b001, 0b010}};
        case '\'': return {{0b010, 0b010, 0b000, 0b000, 0b000}};
        case '.': return {{0b000, 0b000, 0b000, 0b000, 0b010}};
        case '-': return {{0b000, 0b000, 0b111, 0b000, 0b000}};
        case ':': return {{0b000, 0b010, 0b000, 0b010, 0b000}};
        case '+': return {{0b000, 0b010, 0b111, 0b010, 0b000}};
        case '/': return {{0b001, 0b001, 0b010, 0b100, 0b100}};
        case '?': return {{0b110, 0b001, 0b010, 0b000, 0b010}};
        case '[': return {{0b110, 0b100, 0b100, 0b100, 0b110}};
        case ']': return {{0b011, 0b001, 0b001, 0b001, 0b011}};
        case '_': return {{0b000, 0b000, 0b000, 0b000, 0b111}};
        default: return {{0b000, 0b000, 0b000, 0b000, 0b000}};
    }
}

