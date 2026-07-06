#include "draw_3ds.h"
#include <citro2d.h>

namespace trackr::platform {

// citro2d uses ABGR for C2D_Color32. our ui::Color is ARGB (0xAARRGGBB):
// alpha in the high byte, then R, G, B. (matches how the palette is written.)
static u32 conv(ui::Color c) {
    u8 a = (c >> 24) & 0xFF;
    u8 r = (c >> 16) & 0xFF;
    u8 g = (c >> 8)  & 0xFF;
    u8 b = c & 0xFF;
    return C2D_Color32(r, g, b, a);
}

void Draw3DS::rect(int x, int y, int w, int h, ui::Color c) {
    C2D_DrawRectSolid((float)x, (float)y, 0.0f, (float)w, (float)h, conv(c));
}

// glyph: draw groups of consecutive 1-bits in each row
// instead of up to 6 rects per row = up to 3 rects (max 3 groups in a 6-bit pattern)
// plus skipping empty rows
void Draw3DS::glyph(int x, int y, char ch, ui::Color color, int scale) {
    u32 col = conv(color);
    for (int row = 0; row < ui::FONT_H; ++row) {
        uint8_t bits = ui::glyph_row(ch, row);
        if (!bits) continue;

        // find runs of consecutive 1-bits, draw in one rect
        int run_start = -1;
        for (int bx = 0; bx < ui::FONT_W; ++bx) {
            bool on = (bits & (1 << (5 - bx))) != 0;
            if (on && run_start < 0) {
                run_start = bx;
            } else if (!on && run_start >= 0) {
                // end of run - draw
                int len = bx - run_start;
                C2D_DrawRectSolid(
                    (float)(x + run_start * scale),
                    (float)(y + row * scale),
                    0.0f,
                    (float)(len * scale),
                    (float)scale,
                    col);
                run_start = -1;
            }
        }
        // run to the end of the row
        if (run_start >= 0) {
            int len = ui::FONT_W - run_start;
            C2D_DrawRectSolid(
                (float)(x + run_start * scale),
                (float)(y + row * scale),
                0.0f,
                (float)(len * scale),
                (float)scale,
                col);
        }
    }
}

} // namespace trackr::platform
