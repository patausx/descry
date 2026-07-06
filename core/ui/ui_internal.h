// internal shared helpers for the ui layer (App split across multiple .cpp files).
// keyboard geometry + hit-testing live here so both touch.cpp (input) and
// app.cpp (draw_bottom rendering) can use them without duplicating symbols.
#pragma once
#include <cstdint>
#include "draw.h"

namespace trackr::ui {

// === beat glow: breathing gradient for playhead strips ===
// instead of a uniform alpha blink, a bright edge FLOWS across the strip after
// each step change and dissolves - the row breathes with the music.
// pd = frames since the step change. draws OVER an existing backdrop.
inline void beat_glow(Draw& d, int x, int y, int w, int h,
                      uint32_t pd, Color hot, uint32_t travel = 14) {
    if (pd >= travel) return;
    int ex = x + (int)((int64_t)w * (int)pd / (int)travel);   // leading edge
    uint8_t ma = (uint8_t)(170 - pd * 170 / travel);          // master fade
    int sw = w / 8 + 2;                                       // tail slice width
    static const uint8_t divs[4] = {1, 2, 4, 9};              // tail falloff
    for (int i = 0; i < 4; ++i) {
        int sx = ex - i * sw;
        int ww = sw;
        if (sx < x) { ww -= (x - sx); sx = x; }
        if (sx + ww > x + w) ww = (x + w) - sx;
        if (ww <= 0) continue;
        d.rect(sx, y, ww, h, with_alpha(hot, (uint8_t)(ma / divs[i])));
    }
}

// === tactile button: vertical gradient + bevel edges ===
// replaces flat rects so touch buttons read as physical keys (same bevel
// language as the logo). base = face color, border = frame color.
// active = latched/hot state (stronger lift). optional centered label.
inline void ui_button(Draw& d, int x, int y, int w, int h,
                      Color base, Color border,
                      const char* lbl = nullptr, Color lbl_c = 0xFFFFFFFF,
                      bool active = false) {
    // gradient: lifted top quarter, soft upper mid, face, shaded bottom quarter
    const uint8_t lift = active ? 56 : 30;
    int hq = h / 4;
    if (hq < 1) hq = 1;
    Color c_top = lerp_color(base, 0xFFFFFFFF, lift);
    Color c_mid = lerp_color(base, 0xFFFFFFFF, (uint8_t)(lift / 3));
    Color c_bot = lerp_color(base, 0xFF000000, 55);
    d.rect(x, y,          w, hq,            c_top);
    d.rect(x, y + hq,     w, hq,            c_mid);
    d.rect(x, y + 2 * hq, w, h - 3 * hq,    base);
    d.rect(x, y + h - hq, w, hq,            c_bot);
    // bevel frame: bright crown, dark base, plain sides
    d.rect(x, y, w, 1, lerp_color(border, 0xFFFFFFFF, active ? 90 : 36));
    d.rect(x, y + h - 1, w, 1, lerp_color(border, 0xFF000000, 80));
    d.rect(x, y, 1, h, border);
    d.rect(x + w - 1, y, 1, h, border);
    if (lbl) {
        int len = 0; while (lbl[len]) ++len;
        d.text(x + (w - len * 6) / 2, y + (h - 8) / 2, lbl, lbl_c);
    }
}

// === touch keyboard dimensions (bottom screen) ===
inline constexpr int KB_X = 0;
inline constexpr int KB_Y = 140;
inline constexpr int KB_W = 320;
inline constexpr int KB_H = 100;
inline constexpr int WHITE_KEYS = 14;                 // 2 octaves
inline constexpr int WHITE_W = KB_W / WHITE_KEYS;     // ~22 px nominal (see note)

// NOTE: KB_W (320) is not evenly divisible by WHITE_KEYS (14): 14*22=308 leaves a
// 12px gap on the right. To use the full width we compute key edges proportionally
// so the last key lands exactly on KB_W (the few leftover px are spread across keys).
// white_key_x(i) = left edge of white key i; white_key_x(WHITE_KEYS) == KB_X + KB_W.
inline constexpr int white_key_x(int i) { return KB_X + KB_W * i / WHITE_KEYS; }
inline constexpr int white_key_w(int i) { return white_key_x(i + 1) - white_key_x(i); }

// mapping white-key index -> midi semitone offset (C D E F G A B)
inline constexpr int white_to_semi[7] = { 0, 2, 4, 5, 7, 9, 11 };
// black-key index (0..4 within octave) -> the white key after which it is drawn
// black keys: C# D# F# G# A# (after white 0,1,3,4,5)
inline constexpr int black_after_white[5] = { 0, 1, 3, 4, 5 };
inline constexpr int black_to_semi[5]     = { 1, 3, 6, 8, 10 };

// find a note from touch coordinates. returns -1 if not on the keyboard.
inline int find_keyboard_note(int x, int y, int octave) {
    if (y < KB_Y || y >= KB_Y + KB_H) return -1;
    if (x < KB_X || x >= KB_X + KB_W) return -1;

    // check black keys first (they're on top in the upper half)
    if (y < KB_Y + KB_H * 6 / 10) {
        // black keys in the top 60% of the keyboard
        for (int oct = 0; oct < 2; ++oct) {
            for (int b = 0; b < 5; ++b) {
                int wh = oct * 7 + black_after_white[b];
                int cx = white_key_x(wh + 1);          // sits on the seam between two white keys
                int bw = white_key_w(wh) * 6 / 10;
                if (x >= cx - bw / 2 && x < cx + bw / 2) {
                    return (octave + oct) * 12 + black_to_semi[b];
                }
            }
        }
    }
    // white keys: find which key's span contains x
    int wi = 0;
    for (int i = 0; i < WHITE_KEYS; ++i) {
        if (x >= white_key_x(i) && x < white_key_x(i + 1)) { wi = i; break; }
    }
    int oct = wi / 7;
    int idx = wi % 7;
    return (octave + oct) * 12 + white_to_semi[idx];
}

// === performance PADS (4x4, MPC-style alternative to the piano keyboard) ===
// pads get a TALLER band than the keyboard: they start right under the scope
// strip (y=116) for fatter, easier-to-hit cells (31px vs 25px).
// pad index = row-major from BOTTOM-left (like MPC: bottom row = first 4 notes).
inline constexpr int PADS_COLS = 4, PADS_ROWS = 4;
inline constexpr int PADS_Y = 116;
inline constexpr int PADS_H = 240 - PADS_Y;                 // 124
inline constexpr int pad_cell_w() { return KB_W / PADS_COLS; }     // 80
inline constexpr int pad_cell_h() { return PADS_H / PADS_ROWS; }   // 31

// find pad index (0..15) from touch coords. -1 = not on the pads.
inline int find_pad_index(int x, int y) {
    if (y < PADS_Y || y >= PADS_Y + PADS_H) return -1;
    if (x < KB_X || x >= KB_X + KB_W) return -1;
    int col = (x - KB_X) / pad_cell_w();
    int row = (y - PADS_Y) / pad_cell_h();
    if (col > 3) col = 3;
    if (row > 3) row = 3;
    return (PADS_ROWS - 1 - row) * PADS_COLS + col;   // bottom row = 0..3
}

} // namespace trackr::ui
