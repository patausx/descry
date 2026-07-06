// theme presets for the pal:: runtime palette.
// rules that keep a theme from being garbage (learned the hard way):
//  1. BG is NEVER pure black unless the whole theme is built for it - use a
//     tinted near-black so panels (PANEL) can sit BELOW it.
//  2. contrast ladder must hold: FG(text) > FG_HEX > HEADER > FG_DIM > GRID > BG_HI > BG.
//  3. exactly ONE "cold/odd" accent that marks MOTION (playhead/scopes) - PLAY.
//  4. CURSOR must differ from HEADER in hue AND lightness (not just tint).
//  5. RECORD stays reddish in every theme - alarm semantics don't theme.
#include "draw.h"

namespace trackr::ui::pal {

struct Theme {
    const char* name;
    Color bg, bg_hi, panel, grid, fg, fg_dim, fg_hex, cursor,
          play, play_bg, header, t0, t1, t2, t3, record, flash;
};

static const Theme kThemes[] = {
    // 0: cretaceous (the shipping default) - warm graphite + grass green motion
    { "CRETAC",
      0xFF313432, 0xFF323E42, 0xFF0A0A0E, 0xFF454B4B,
      0xFFBCB7A5, 0xFF625055, 0xFFB4AB8F, 0xFFAC9086,
      0xFF718245, 0xFF3A5F3B, 0xFF9E805C,
      0xFFAC9086, 0xFF718245, 0xFF9E805C, 0xFF796C64,
      0xFF7C4545, 0xFFBCB7A5 },
    // 1: midnite - m8-style near-black, mint motion, ice-blue cursor
    { "MIDNIT",
      0xFF101216, 0xFF1A1E26, 0xFF060608, 0xFF2A3038,
      0xFFD8DCE0, 0xFF4A5058, 0xFFA8C0B8, 0xFF7EC8E0,
      0xFF50C878, 0xFF1E4A32, 0xFF8890A0,
      0xFF7EC8E0, 0xFF50C878, 0xFFC8B060, 0xFF9078B0,
      0xFFC04848, 0xFFFFFFFF },
    // 2: ember - amber CRT: warm near-black, amber text, orange cursor
    { "EMBER ",
      0xFF1A1410, 0xFF261C14, 0xFF0C0806, 0xFF3A2E22,
      0xFFE0B060, 0xFF6A5038, 0xFFC09850, 0xFFE87838,
      0xFFB8D848, 0xFF3A4A1E, 0xFFA07840,
      0xFFE87838, 0xFFB8D848, 0xFFE0B060, 0xFF907050,
      0xFFD04030, 0xFFFFE0A0 },
    // 3: frost - cold slate + ice text, aurora green motion, rose cursor
    { "FROST ",
      0xFF2E3440, 0xFF3B4252, 0xFF14181E, 0xFF4C566A,
      0xFFE5E9F0, 0xFF616E88, 0xFFB8C4D8, 0xFFD08790,
      0xFFA3BE8C, 0xFF3E5244, 0xFF81A1C1,
      0xFFD08790, 0xFFA3BE8C, 0xFFEBCB8B, 0xFFB48EAD,
      0xFFBF616A, 0xFFECEFF4 },
    // 4: acid - the brand colorway: olive-black + acid lime motion, amber cursor
    { "ACID  ",
      0xFF14160C, 0xFF1C2010, 0xFF0A0C06, 0xFF32381E,
      0xFFD8E4B0, 0xFF5A6038, 0xFFB8C878, 0xFFE0B050,
      0xFFC8E030, 0xFF3A4410, 0xFF98A840,
      0xFFE0B050, 0xFFC8E030, 0xFF98A840, 0xFF6A7048,
      0xFFD04838, 0xFFF0FFC0 },
    // 5: vapor - synthwave dusk: deep violet, turquoise motion, hot-pink cursor
    { "VAPOR ",
      0xFF16121E, 0xFF201A2E, 0xFF0A0812, 0xFF3A3050,
      0xFFE8DCF0, 0xFF554868, 0xFFC0A8D8, 0xFFFF74A8,
      0xFF40E0D0, 0xFF1A4A44, 0xFF9080B8,
      0xFFFF74A8, 0xFF40E0D0, 0xFFE8C060, 0xFF8878A8,
      0xFFE04848, 0xFFFFFFFF },
};

int theme_count() { return (int)(sizeof(kThemes) / sizeof(kThemes[0])); }
const char* theme_name(int i) {
    if (i < 0 || i >= theme_count()) return "?";
    return kThemes[i].name;
}

ThemeColors theme_colors(int i) {
    if (i < 0 || i >= theme_count()) i = 0;
    const Theme& t = kThemes[i];
    return { t.bg, t.play, t.cursor, t.fg, t.header };
}

void apply_theme(int i) {
    if (i < 0 || i >= theme_count()) return;
    const Theme& t = kThemes[i];
    BG = t.bg;   BG_HI = t.bg_hi; PANEL = t.panel; GRID = t.grid;
    FG = t.fg;   FG_DIM = t.fg_dim; FG_HEX = t.fg_hex; CURSOR = t.cursor;
    PLAY = t.play; PLAY_BG = t.play_bg; HEADER = t.header;
    TRACK0 = t.t0; TRACK1 = t.t1; TRACK2 = t.t2; TRACK3 = t.t3;
    RECORD = t.record; FLASH = t.flash;
}

// recording tint: shift the whole UI toward "hot" red while audio is being
// captured (mic / resample). t animates 0..255 -> the tint breathes.
void apply_record_tint(uint8_t t) {
    // backgrounds go deep warm red, structure lines rust, accents hot
    BG      = lerp_color(BG,      0xFF2A0F0D, t);
    BG_HI   = lerp_color(BG_HI,   0xFF3A1512, t);
    PANEL   = lerp_color(PANEL,   0xFF160605, t);
    GRID    = lerp_color(GRID,    0xFF5C2823, t);
    HEADER  = lerp_color(HEADER,  0xFFC05A38, t);
    CURSOR  = lerp_color(CURSOR,  0xFFE86850, t);
    PLAY    = lerp_color(PLAY,    0xFFE87840, t);   // motion accent runs hot too
    PLAY_BG = lerp_color(PLAY_BG, 0xFF55201A, t);
    FG_HEX  = lerp_color(FG_HEX,  0xFFD8A890, t);
    FG_DIM  = lerp_color(FG_DIM,  0xFF7A4038, t);
    // text stays readable - only warms slightly
    FG      = lerp_color(FG,      0xFFE8CCC0, t / 2);
}

} // namespace trackr::ui::pal
