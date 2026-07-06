// 6x8 bitmap font - m8-style monospace
// printable ASCII 0x20-0x7F, each char = 8 bytes (one bit = pixel in a 6-wide row)
// the top 2 bits of each byte are unused (16 px high visually via 2x scale)
//
// format: bit 5 = leftmost pixel, bit 0 = rightmost (we use bit 5..0 = 6 pixels wide)
//
// hand-made the most common characters for the tracker: 0-9 a-z A-Z hex, brackets, hyphen, dot
#pragma once
#include <cstdint>

namespace trackr::ui {

constexpr int FONT_W = 6;
constexpr int FONT_H = 8;

// glyph[char][row] = 8 rows of 6 bits (bit 5 = leftmost)
// printable ascii only, indexing: ch - 0x20
extern const uint8_t font_6x8[96][8];

// get the byte-row for a character (with bounds check)
inline uint8_t glyph_row(char c, int row) {
    int idx = (unsigned char)c - 0x20;
    if (idx < 0 || idx >= 96) idx = 0;
    if (row < 0 || row >= FONT_H) return 0;
    return font_6x8[idx][row];
}

} // namespace trackr::ui
