// App: theme and scope-style settings picker.
#include "app.h"
#include "ui_internal.h"

namespace trackr::ui {

// === theme picker overlay ===
// a row per theme: name + live swatch strip (bg / play / cursor / fg / header).
// tap a row = apply instantly (main persists it), tap outside = close.
namespace {
    constexpr int THM_X = 32, THM_W = 256;
    constexpr int THM_ROW_H = 26, THM_HDR = 18;
}

void App::draw_theme_menu(Draw& d) {
    const int n = pal::theme_count();
    // +1 row: scope style selector (WAVE/BARS/DOTS/X-Y) lives with the themes -
    // same "how descry looks" family, same muscle memory (tap the wordmark).
    const int ph = THM_HDR + (n + 1) * THM_ROW_H + 6;
    const int py = (240 - ph) / 2;

    // unfold: vertical shutter like the fx help
    {
        uint32_t age = frame_ - theme_menu_frame_;
        constexpr uint32_t UNFOLD = 6;
        if (age < UNFOLD) {
            int hh = ph * (int)(age + 1) / (int)UNFOLD;
            int yy = py + (ph - hh) / 2;
            d.rect(THM_X - 2, yy - 2, THM_W + 4, hh + 4, pal::BG_HI);
            if (hh > 8) d.rect(THM_X, yy, THM_W, hh, pal::PANEL);
            return;
        }
    }

    d.rect(THM_X - 2, py - 2, THM_W + 4, ph + 4, pal::BG_HI);
    d.rect(THM_X, py, THM_W, ph, pal::PANEL);
    if (theme_menu_closing_) {
        uint32_t ca = frame_ - overlay_close_frame_;
        uint8_t a = (uint8_t)clamp_int((int)ca * 52, 0, 210);
        d.rect(THM_X, py, THM_W, ph, with_alpha(pal::BG, a));
        int in = (int)motion_in(ca, 4) * 20 / 255;
        d.corner_brackets(THM_X + in, py + in / 2, THM_W - in * 2, ph - in,
                          with_alpha(pal::CURSOR, (uint8_t)(220 - a)), 6, 1);
        return;
    }
    d.text(THM_X + 6, py + 5, "THEME", pal::HEADER);
    d.text(THM_X + THM_W - 100, py + 5, "TAP OUT=CLOSE", pal::FG_DIM);

    for (int i = 0; i < n; ++i) {
        int y = py + THM_HDR + i * THM_ROW_H;
        bool cur = (i == theme_idx);
        if (cur) {
            uint8_t br = breathe_pulse(frame_, 48);
            d.rect(THM_X + 2, y, THM_W - 4, THM_ROW_H - 3,
                   lerp_color(with_alpha(pal::CURSOR, 50), with_alpha(pal::CURSOR, 100), br));
        }
        d.text(THM_X + 8, y + 8, pal::theme_name(i), cur ? pal::FG : pal::FG_HEX);
        // swatch strip: the theme's own key colors, straight from the preset
        pal::ThemeColors tc = pal::theme_colors(i);
        const Color sw[5] = { tc.bg, tc.play, tc.cursor, tc.fg, tc.header };
        for (int s = 0; s < 5; ++s) {
            int sx = THM_X + 110 + s * 26;
            d.rect(sx, y + 4, 22, THM_ROW_H - 11, sw[s]);
            d.rect(sx, y + 4, 22, 1, pal::GRID);
            d.rect(sx, y + 4 + THM_ROW_H - 12, 22, 1, pal::GRID);
        }
    }

    // === scope style row (below the themes, separated by a grid line) ===
    {
        int y = py + THM_HDR + n * THM_ROW_H;
        d.rect(THM_X + 4, y - 1, THM_W - 8, 1, pal::GRID);
        d.text(THM_X + 8, y + 8, "SCOPE", pal::HEADER);
        // the 3 style names in a row - current one boxed + bright
        for (int s = 0; s < SCOPE_STYLES; ++s) {
            int sx = THM_X + 76 + s * 58;
            bool on = (s == scope_style_);
            if (on) {
                uint8_t br = breathe_pulse(frame_, 48);
                d.rect(sx - 4, y + 4, 52, THM_ROW_H - 10,
                       lerp_color(with_alpha(pal::PLAY, 50), with_alpha(pal::PLAY, 100), br));
            }
            d.text(sx, y + 8, scope_style_name(s), on ? pal::FG : pal::FG_DIM);
        }
    }
}

bool App::theme_menu_touch(int x, int y) {
    const int n = pal::theme_count();
    const int ph = THM_HDR + (n + 1) * THM_ROW_H + 6;
    const int py = (240 - ph) / 2;
    if (x < THM_X || x >= THM_X + THM_W || y < py || y >= py + ph) {
        begin_overlay_close(theme_menu_closing_);
        return true;
    }
    int row = (y - py - THM_HDR) / THM_ROW_H;
    if (row >= 0 && row < n) {
        set_theme(row);         // apply instantly; main.cpp persists the change
        begin_overlay_close(theme_menu_closing_);
    } else if (row == n) {
        // scope style row: tap a name directly, or anywhere in the row = cycle.
        // menu STAYS open - you want to see the strip react while you pick.
        int s = (x - (THM_X + 76 - 4)) / 58;
        scope_style_ = (s >= 0 && s < SCOPE_STYLES) ? s
                     : (scope_style_ + 1) % SCOPE_STYLES;
    }
    return true;
}

} // namespace trackr::ui
