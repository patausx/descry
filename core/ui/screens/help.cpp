// App: in-app HELP — paginated manual overlay on the bottom screen.
// opened by tapping the hint strip (or the "?" badge). owns all touches
// while open, closed with B / tapping the header / CLOSE button.
//
// content mirrors docs/GUIDE.md in compressed form: basics, keys, phrase
// editing, instruments, the full FX command list, performance and sampling.
// 6x8 font -> 53 chars per line max; keep lines <= 50 to breathe.
#include "../app.h"
#include "../ui_internal.h"
#include "../../sequencer/fx.h"
#include <cstdio>
#include <cstring>

namespace trackr::ui {

namespace {

struct HelpPage {
    const char* title;
    const char* const* lines;   // nullptr-terminated
};

// --- page 1: basics ---
static const char* const pg_basics[] = {
    "descry is a tracker. music is built from",
    "small parts nested inside bigger ones:",
    "",
    "  SONG > CHAIN > PHRASE (16 steps)",
    "chains string phrases; song stacks 8 trks",
    "",
    "L / R      switch view (or tap the tabs)",
    "START      play: phrase view = this phrase,",
    "           chain view = this chain, else song",
    "A          place / edit value under cursor",
    "SELECT     preview note (hold = sustain)",
    "",
    "REC btn cycles the touch-keyboard mode:",
    "  JAM   keys only preview - nothing writes",
    "  WRT   keys write notes at the cursor",
    "  LIVE  record onto the playing step",
    "CLR erases the step (tap-tap-tap = a run).",
    nullptr
};

// --- page 2: global keys ---
static const char* const pg_keys[] = {
    "GLOBAL COMBOS (any view)",
    "",
    "L+dpad     BPM  (up/dn +-1, l/r +-10)",
    "R+up/dn    groove (ticks per step)",
    "R+l/r      swing 0..50%",
    "ZL+B / A   undo / redo",
    "ZL+X / Y   copy / paste",
    "ZL+SELECT  selection mode / clone / fx row",
    "L+SELECT   fullscreen oscilloscope",
    "R+SELECT   screenshot -> SD /screens/",
    "SELECT+START  exit descry",
    "",
    "hold a modifier (L / R / ZL) and the hint",
    "bar below shows that modifier's live map.",
    "",
    "tap DESCRY logo = theme picker",
    "tap BPM readout = tap tempo",
    "tap KEY readout = root / scale cycle",
    nullptr
};

// --- page 3: phrase editing ---
static const char* const pg_phrase[] = {
    "PHRASE VIEW - the note grid",
    "",
    "columns: NOTE INST VEL FX1 FX2 FX3",
    "",
    "A on empty  insert last-entered note",
    "A/B         value +-1 (notes snap to key)",
    "X/Y         value +-12 (octave) / big step",
    "SELECT      note col: preview",
    "            FX col: command list picker",
    "R+A         clear cell   R+B  clear step",
    "R+Y         clear whole phrase",
    "ZL+SELECT   block select (A = copy, B = out)",
    "ZL+X / ZL+Y copy / paste step or block",
    "L+l/r       previous / next phrase",
    "",
    "FX cells: 3-letter command + hex value.",
    "the hint bar decodes whatever the cursor",
    "is on - watch it while you scroll.",
    nullptr
};

// --- page 4: instruments ---
static const char* const pg_inst[] = {
    "INSTRUMENT VIEW",
    "",
    "types: WAV (wavetable) SMP (sampler)",
    "       KIT (drumkit)   FM (4-op)",
    "       DSN (2vco analog voice)",
    "",
    "PRESET row: A/B cycles a bank of patches.",
    "preset 1 = INIT - a blank patch, build",
    "your own sound from scratch.",
    "",
    "L+l/r       previous / next instrument slot",
    "L+A         clone instrument to a free slot",
    "ZL+SELECT   focus the FX defaults strip",
    "            (FLT CUT RES DEL REV VOL PAN CRS)",
    "",
    "sampler bottom tabs: KB WAVE SLICE LOAD REC.",
    "envelope params pop an ADSR curve overlay.",
    "TBL view + SELECT assigns a table.",
    nullptr
};

// --- pages 5+6: FX command reference (same order as the fx picker) ---
static const char kHelpFx[] = "PJVFQYBSEGALMNWRKXDOCHT";
constexpr int N_HELP_FX = (int)sizeof(kHelpFx) - 1;   // 23
constexpr int FX_SPLIT = 12;                          // page 5 rows

// --- page 7: performance ---
static const char* const pg_perform[] = {
    "PERFORMANCE",
    "",
    "KAOS pad (bottom btn KEYS>PADS>KAOS):",
    "  XY field writes into the track DSP.",
    "  tap X / Y buttons to assign dests",
    "  (cut res del rev bit dsm lfo vol pan..)",
    "  TRK = track under cursor, ALL = all 8.",
    "  release ramps back - nothing sticks.",
    "",
    "left stick   = the kaoss pad on a stick",
    "right stick  = X: delay+reverb send",
    "               Y pull down: bitcrush",
    "",
    "song view: bottom pads = SOLO toggles.",
    "mixer view: faders, mute (SELECT), master",
    "strip: delay / reverb / duck params.",
    "REC btn on LIVE + play = live-record the",
    "notes you tap; CLR erases the playing step.",
    nullptr
};

// --- page 8: sampling + SD ---
static const char* const pg_files[] = {
    "SAMPLING + FILES",
    "",
    "mic: REC panel in a sampler instrument -",
    "record straight into a sample slot.",
    "WAVE tab: trim, normalize, fades, crop.",
    "SLICE tab: chops, auto x16, chop -> kit.",
    "LOAD tab: wav browser (8/16/24/32 bit).",
    "",
    "SD layout - sdmc:/3ds/descry/",
    "  projects/    16 project slots",
    "  wav/         your samples (subfolders ok)",
    "  wavetable/   single-cycle waves (USER osc)",
    "  screens/     screenshots (R+SELECT)",
    "  render.wav   song export",
    "",
    "projects autosave on exit; the PRJ view",
    "shows a NOW banner + dirty marker.",
    nullptr
};

static const HelpPage kPages[] = {
    { "1. START HERE",   pg_basics  },
    { "2. GLOBAL KEYS",  pg_keys    },
    { "3. PHRASE",       pg_phrase  },
    { "4. INSTRUMENTS",  pg_inst    },
    { "5. FX LIST A",    nullptr    },   // generated from fx.h
    { "6. FX LIST B",    nullptr    },   // generated from fx.h
    { "7. PERFORM",      pg_perform },
    { "8. SAMPLING",     pg_files   },
};
constexpr int N_PAGES = (int)(sizeof(kPages) / sizeof(kPages[0]));

// overlay geometry (mirrors the theme picker language)
constexpr int HLP_X = 2, HLP_Y = 2, HLP_W = 316, HLP_H = 236;
constexpr int HLP_HDR = 16;
constexpr int HLP_LINE_H = 11;
constexpr int HLP_FOOT_Y = HLP_Y + HLP_H - 18;

} // anon namespace

void App::open_help() {
    // land on the page matching the current screen
    switch (screen_) {
        case Screen::Phrase:     help_page_ = 2; break;
        case Screen::Instrument: help_page_ = 3; break;
        case Screen::Mixer:      help_page_ = 6; break;
        case Screen::Project:    help_page_ = 7; break;
        default:                 help_page_ = 0; break;
    }
    help_on_ = true;
    help_frame_ = frame_;
}

void App::update_help(const InputState& in) {
    if (in.b || in.start) { help_on_ = false; return; }
    if (in.left  || in.up)   help_page_ = (help_page_ + N_PAGES - 1) % N_PAGES;
    if (in.right || in.down || in.a) help_page_ = (help_page_ + 1) % N_PAGES;
}

void App::draw_help(Draw& d) {
    // unfold shutter (same feel as the fx picker / theme menu)
    {
        uint32_t age = frame_ - help_frame_;
        constexpr uint32_t UNFOLD = 6;
        if (age < UNFOLD) {
            int hh = HLP_H * (int)(age + 1) / (int)UNFOLD;
            int yy = HLP_Y + (HLP_H - hh) / 2;
            d.rect(HLP_X - 2, yy - 2, HLP_W + 4, hh + 4, pal::BG_HI);
            if (hh > 8) d.rect(HLP_X, yy, HLP_W, hh, pal::PANEL);
            return;
        }
    }

    d.rect(HLP_X - 2, HLP_Y - 2, HLP_W + 4, HLP_H + 4, pal::BG_HI);
    d.rect(HLP_X, HLP_Y, HLP_W, HLP_H, pal::PANEL);

    const HelpPage& pg = kPages[help_page_];
    d.text(HLP_X + 6, HLP_Y + 5, "MANUAL", pal::HEADER);
    d.text(HLP_X + 56, HLP_Y + 5, pg.title, pal::FG);
    {
        char pb[8];
        std::snprintf(pb, sizeof(pb), "%d/%d", help_page_ + 1, N_PAGES);
        d.text(HLP_X + HLP_W - 28, HLP_Y + 5, pb, pal::FG_DIM);
    }
    d.rect(HLP_X + 4, HLP_Y + HLP_HDR - 1, HLP_W - 8, 1, pal::GRID);

    const int y0 = HLP_Y + HLP_HDR + 4;
    if (pg.lines) {
        for (int i = 0; pg.lines[i]; ++i) {
            const char* s = pg.lines[i];
            // section-ish lines (first line / ALL CAPS heads) get the header color
            Color c = (i == 0) ? pal::HEADER : pal::FG_HEX;
            d.text(HLP_X + 8, y0 + i * HLP_LINE_H, s, c);
        }
    } else {
        // FX reference pages: generated from the engine's own tables so the
        // help can never drift from what the player actually executes.
        int base = (help_page_ == 4) ? 0 : FX_SPLIT;
        int n    = (help_page_ == 4) ? FX_SPLIT : N_HELP_FX - FX_SPLIT;
        d.text(HLP_X + 8, y0, "CMD  what it does", pal::HEADER);
        for (int i = 0; i < n; ++i) {
            uint8_t cmd = (uint8_t)kHelpFx[base + i];
            int y = y0 + (i + 1) * HLP_LINE_H + 3;
            d.text(HLP_X + 8,  y, seq::fx_name_short(cmd), pal::CURSOR);
            d.text(HLP_X + 36, y, seq::fx_name_long(cmd), pal::FG_HEX);
        }
        d.text(HLP_X + 8, HLP_FOOT_Y - 13,
               "SELECT on an FX column = interactive picker", pal::FG_DIM);
    }

    // footer: prev / close / next
    d.rect(HLP_X + 4, HLP_FOOT_Y - 3, HLP_W - 8, 1, pal::GRID);
    d.text(HLP_X + 8, HLP_FOOT_Y + 3, "<PREV", pal::HEADER);
    d.text(HLP_X + HLP_W / 2 - 24, HLP_FOOT_Y + 3, "B=CLOSE", pal::FG_DIM);
    d.text(HLP_X + HLP_W - 38, HLP_FOOT_Y + 3, "NEXT>", pal::HEADER);
}

bool App::help_touch(int x, int y) {
    // header row = close
    if (y < HLP_Y + HLP_HDR) { help_on_ = false; return true; }
    // footer: left third prev, right third next, middle close
    if (y >= HLP_FOOT_Y - 3) {
        if (x < HLP_X + HLP_W / 3)          help_page_ = (help_page_ + N_PAGES - 1) % N_PAGES;
        else if (x >= HLP_X + 2 * HLP_W / 3) help_page_ = (help_page_ + 1) % N_PAGES;
        else                                 help_on_ = false;
        return true;
    }
    // body: left/right half = page flip (fast thumbing)
    if (x < HLP_X + HLP_W / 2) help_page_ = (help_page_ + N_PAGES - 1) % N_PAGES;
    else                       help_page_ = (help_page_ + 1) % N_PAGES;
    return true;
}

} // namespace trackr::ui
