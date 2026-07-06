// drawing abstraction - core draws through this interface,
// the platform implements it via citro2d
#pragma once
#include <cstdint>

namespace trackr::ui {

using Color = uint32_t;  // RGBA8

// palette: RUNTIME-SWITCHABLE (themes). defaults = "cretaceous-16" by Joao
// Vasconcelos (lospec) - earthy graphite/teal/green with warm terracotta +
// dusty rose accents. all UI code reads these every frame, so pal::apply_theme()
// recolors every view instantly. don't cache pal:: values in statics!
namespace pal {
    inline Color BG       = 0xFF313432;  // background
    inline Color BG_HI    = 0xFF323E42;  // zebra/header fill
    inline Color PANEL    = 0xFF0A0A0E;  // deep panel (scope/pads/kaoss field bg)
    inline Color GRID     = 0xFF454B4B;  // grid lines / inactive borders
    inline Color FG       = 0xFFBCB7A5;  // text
    inline Color FG_DIM   = 0xFF625055;  // dim/empty cells
    inline Color FG_HEX   = 0xFFB4AB8F;  // hex values
    inline Color CURSOR   = 0xFFAC9086;  // cursor (warm accent)
    inline Color PLAY     = 0xFF718245;  // playhead
    inline Color PLAY_BG  = 0xFF3A5F3B;  // playhead row backdrop
    inline Color HEADER   = 0xFF9E805C;  // labels
    inline Color TRACK0   = 0xFFAC9086;
    inline Color TRACK1   = 0xFF718245;
    inline Color TRACK2   = 0xFF9E805C;
    inline Color TRACK3   = 0xFF796C64;
    inline Color RECORD   = 0xFF7C4545;  // alarm
    inline Color FLASH    = 0xFFBCB7A5;  // value-edit flash

    // === themes ===
    int  theme_count();
    const char* theme_name(int i);
    void apply_theme(int i);   // copies preset i into the variables above
    // read-only peek at a preset's key colors (theme picker swatches)
    struct ThemeColors { Color bg, play, cursor, fg, header; };
    ThemeColors theme_colors(int i);
    // recording tint: pull the CURRENT palette toward hot red. t = 0..255.
    // call AFTER apply_theme (it mutates in place) - the caller re-applies the
    // base theme first each frame, so the pulse can animate.
    void apply_record_tint(uint8_t t);
}

// replace the alpha of an ARGB8 color (a: 0..255). format is ARGB - alpha in the HIGH byte.
constexpr Color with_alpha(Color c, uint8_t a) {
    return (c & 0x00FFFFFF) | ((Color)a << 24);
}
// linear blend of two ARGB colors across all channels: t=0 -> a, t=255 -> b
constexpr uint8_t lerp8(uint8_t x, uint8_t y, uint8_t t) {
    return (uint8_t)(x + ((int)(y - x) * t) / 255);
}
inline Color lerp_color(Color a, Color b, uint8_t t) {
    return ((Color)lerp8((a >> 24) & 0xFF, (b >> 24) & 0xFF, t) << 24)
         | ((Color)lerp8((a >> 16) & 0xFF, (b >> 16) & 0xFF, t) << 16)
         | ((Color)lerp8((a >> 8)  & 0xFF, (b >> 8)  & 0xFF, t) << 8)
         | ((Color)lerp8(a & 0xFF, b & 0xFF, t));
}

// "breathing" pulse: smooth 0..255..0 triangle over `period` frames.
// use to modulate a cursor's alpha/brightness so it gently pulses.
constexpr uint8_t breathe_pulse(uint32_t frame, uint32_t period = 64) {
    uint32_t p = frame % period;
    uint32_t half = period / 2;
    uint32_t tri = (p < half) ? p : (period - p);   // 0..half..0
    return (uint8_t)(tri * 255 / half);
}

class Draw {
public:
    virtual ~Draw() = default;

    // rectangle
    virtual void rect(int x, int y, int w, int h, Color c) = 0;

    // one glyph 6x8 (scale 1) or 12x16 (scale 2)
    virtual void glyph(int x, int y, char c, Color color, int scale = 1) = 0;

    // string
    void text(int x, int y, const char* s, Color color, int scale = 1) {
        while (*s) {
            glyph(x, y, *s, color, scale);
            x += 6 * scale;
            ++s;
        }
    }

    // selection corner brackets: four L-shaped corners hugging the box (x,y,w,h).
    // `len` = arm length in px, `t` = thickness. M8/LSDj-style selector instead of a
    // full rectangle. pair with breathe() on the alpha for a pulsing "breathing" cursor.
    void corner_brackets(int x, int y, int w, int h, Color c, int len = 3, int t = 1) {
        if (len * 2 > w) len = w / 2;
        if (len * 2 > h) len = h / 2;
        const int x2 = x + w - t;   // right edge start
        const int y2 = y + h - t;   // bottom edge start
        // top-left
        rect(x, y, len, t, c);          rect(x, y, t, len, c);
        // top-right
        rect(x + w - len, y, len, t, c); rect(x2, y, t, len, c);
        // bottom-left
        rect(x, y2, len, t, c);          rect(x, y + h - len, t, len, c);
        // bottom-right
        rect(x + w - len, y2, len, t, c); rect(x2, y + h - len, t, len, c);
    }

    // helpers
    void hex2(int x, int y, uint8_t v, Color color, int scale = 1) {
        static const char H[] = "0123456789ABCDEF";
        glyph(x, y, H[(v >> 4) & 0xF], color, scale);
        glyph(x + 6 * scale, y, H[v & 0xF], color, scale);
    }
    void hex4(int x, int y, uint16_t v, Color color, int scale = 1) {
        hex2(x, y, (v >> 8) & 0xFF, color, scale);
        hex2(x + 12 * scale, y, v & 0xFF, color, scale);
    }

    // note -> string like "C-4", "F#3"
    static void note_str(uint8_t note, char out[4]);
    void note(int x, int y, uint8_t note, Color color, int scale = 1);
};

} // namespace trackr::ui
