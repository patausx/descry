// App: in-app HELP — paginated manual overlay on the bottom screen.
// opened by tapping the hint strip (or the "?" badge). owns all touches
// while open, closed with B / tapping the header / CLOSE button.
//
// content mirrors docs/GUIDE.md in compressed form: basics, keys, phrase
// editing, instruments, the full FX command list, performance, sampling and
// files/export.
// 6x8 font -> 53 chars per line max; keep lines <= 50 to breathe.
// AND keep each page <= 18 lines: line 19 lands under the footer divider and is
// simply never seen (that silently swallowed the whole SD/export section once).
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
    "START      play: phrase=this phrase, chain=",
    "           this chain, song=from CURSOR row",
    "L+START    play the song from the top",
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
    "ZL+B / A   undo / redo (inst view: instrument)",
    "ZL+X / Y   copy / paste",
    "ZL+SELECT  selection mode / clone / fx row",
    "L+SELECT   fullscreen oscilloscope",
    "R+SELECT   screenshot -> SD /screens/",
    "SELECT+START  exit descry",
    "",
    "circle: SONG scroll / WAVE scrub+zoom",
    "C-stick up/dn value; sideways = coarse +-16",
    "",
    "hold L/R/ZL: hint bar shows its live map",
    "tap logo: theme/scope; BPM: tap tempo",
    "tap KEY readout: root / scale cycle",
    nullptr
};

// --- page 3: phrase editing ---
static const char* const pg_phrase[] = {
    "PHRASE VIEW - the note grid",
    "",
    "columns: NOTE INST VEL FX1 FX2 FX3",
    "A on empty  insert last-entered note",
    "A/B         value +-1 (notes snap to key)",
    "X/Y         value +-12 (octave) / big step",
    "SELECT      note: preview  inst: open inst",
    "            FX col: command list picker",
    "R+A cell  R+B step  R+Y whole phrase",
    "R+X         TOOLS: transforms + seeded GEN",
    "  EUC density human ratchet mutate random",
    "  chance/every; same seed = same result",
    "ZL+SELECT   block select (A = copy, B = out)",
    "ZL+X / ZL+Y copy / paste step or block",
    "L+l/r       previous / next phrase",
    "FX cells: 3-letter command + hex value.",
    "the side panel (right third) decodes it all:",
    "instrument, its envelope, all 3 FX in words.",
    nullptr
};

// --- page 3: reading the screen ---
// the indicators used to be undocumented, which defeats the point of having
// them: a wash or a letter you can't decode is just noise.
static const char* const pg_read[] = {
    "READING THE SCREEN",
    "",
    "header (always on):",
    "  view + index, BPM, GRV, REC>slot, clock",
    "  small pulsing DOT = unsaved changes",
    "",
    "song view track labels:",
    "  M  track is MUTED (set in mixer)",
    "  S  this track is SOLOed",
    "  s  muted only BECAUSE another is solo",
    "  washed-out column = you hear nothing",
    "  END + line = last row with a chain;",
    "  below it the song never plays (it loops)",
    "",
    "phrase / chain view:",
    "  >T3 = whose playhead you are watching",
    "  shaded rows = past the phrase LENGTH",
    nullptr
};

// --- page 4: instruments ---
static const char* const pg_inst[] = {
    "INSTRUMENT VIEW",
    "",
    "types: WAV (wavetable) SMP (sampler)",
    "       KIT (drumkit)   FM (4-op)",
    "       DSN (2vco analog)",
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
    "SMP tabs: KB WAVE SLICE LOAD REC.",
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
    "circle pad = fast SONG movement; in sampler",
    "WAVE it scrubs X and zooms Y.",
    "C-stick up/dn = value; sideways = coarse +-16.",
    "",
    "song view: bottom pads = SOLO toggles.",
    "mixer view: faders, mute (SELECT), master",
    "strip: delay / reverb / duck params.",
    "mixer GRV column: per-step tick pattern -",
    "A/B +-1, X straight 6, Y clear. swing=7,5",
    "REC LIVE + play records tapped notes; CLR erases.",
    nullptr
};

// --- page 8: sampling ---
// NOTE: a help page fits ~18 lines before it collides with the footer divider.
// this used to be one "sampling + files" page of 23 lines, so the entire SD
// layout block (including how to export) was drawn under the footer = invisible.
// keep each page at or below 18 lines.
static const char* const pg_files[] = {
    "SAMPLING",
    "",
    "mic: REC panel in a sampler instrument -",
    "record straight into a sample slot.",
    "WAVE: X or >WT turns the visible window",
    "into a persistent USER wavetable + WAV inst.",
    "WAVE tab: trim, normalize, fades, crop.",
    "SLICE tab: TRNS=transient chop (LO/MID/HI),",
    "EQ=equal 4/8/16/32, REV=reverse one slice,",
    "tap=select/play, DEL=remove selected boundary.",
    ">KIT >PHR SHUF=dice. drag marker=moves it.",
    "SYNC row: fit sample to N bars (repitch).",
    "LOAD tab: wav browser (8/16/24/32 bit).",
    nullptr
};

// --- page 9: files / SD / export ---
static const char* const pg_sd[] = {
    "FILES + EXPORT",
    "",
    "SD layout - sdmc:/3ds/descry/",
    "  projects/    16 project slots",
    "  wav/         your samples (subfolders ok)",
    "  wavetable/   single-cycle waves (USER osc)",
    "  screens/     screenshots (R+SELECT)",
    "  renders/     song exports",
    "",
    "EXPORT: SELECT in the PRJ view renders the",
    "song to renders/<project name>.wav",
    "(60s max, 32kHz stereo). the PRJ view shows",
    "the exact target filename before you press.",
    "",
    "rename the project first: hold R in the PRJ",
    "view, A/B cycle char, L/R move, X=clear.",
    "a second render never overwrites - it becomes",
    "NAME_01.wav, NAME_02.wav and so on.",
    nullptr
};

static const HelpPage kPages[] = {
    { "1. START HERE",   pg_basics  },
    { "2. GLOBAL KEYS",  pg_keys    },
    { "3. READING UI",   pg_read    },
    { "4. PHRASE",       pg_phrase  },
    { "5. INSTRUMENTS",  pg_inst    },
    { "6. FX LIST A",    nullptr    },   // generated from fx.h
    { "7. FX LIST B",    nullptr    },   // generated from fx.h
    { "8. PERFORM",      pg_perform },
    { "9. SAMPLING",     pg_files   },
    { "10.FILES+EXPORT", pg_sd      },
};
constexpr int N_PAGES = (int)(sizeof(kPages) / sizeof(kPages[0]));

// overlay geometry (mirrors the theme picker language)
constexpr int HLP_X = 2, HLP_Y = 2, HLP_W = 316, HLP_H = 236;
constexpr int HLP_HDR = 16;
constexpr int HLP_LINE_H = 11;
constexpr int HLP_FOOT_Y = HLP_Y + HLP_H - 18;

} // anon namespace

// page indices into kPages - named so adding a page can't silently repoint
// open_help() or the FX generator at the wrong content again.
enum : int {
    PG_BASICS = 0, PG_KEYS, PG_READ, PG_PHRASE, PG_INST,
    PG_FX_A, PG_FX_B, PG_PERFORM, PG_SAMPLING, PG_SD
};

void App::open_help() {
    help_closing_ = false;
    // land on the page matching the current screen
    switch (screen_) {
        case Screen::Song:       help_page_ = PG_READ;   break;   // mute / END markers
        case Screen::Phrase:     help_page_ = PG_PHRASE; break;
        case Screen::Instrument: help_page_ = PG_INST;   break;
        case Screen::Mixer:      help_page_ = PG_PERFORM; break;
        case Screen::Project:    help_page_ = PG_SD;     break;   // files + export page
        default:                 help_page_ = PG_BASICS; break;
    }
    help_on_ = true;
    help_closing_ = false;
    help_frame_ = frame_;
}

void App::update_help(const InputState& in) {
    if (help_closing_) return;
    if (in.b || in.start) { begin_overlay_close(help_closing_); return; }
    if (in.left  || in.up)   help_page_ = wrap_index(help_page_, -1, N_PAGES);
    if (in.right || in.down || in.a) help_page_ = wrap_index(help_page_, +1, N_PAGES);
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
    if (help_closing_) {
        uint32_t ca = frame_ - overlay_close_frame_;
        uint8_t a = (uint8_t)clamp_int((int)ca * 52, 0, 210);
        d.rect(HLP_X, HLP_Y, HLP_W, HLP_H, with_alpha(pal::BG, a));
        int in = (int)motion_in(ca, 4) * 18 / 255;
        d.corner_brackets(HLP_X + in, HLP_Y + in / 2, HLP_W - in * 2, HLP_H - in,
                          with_alpha(pal::CURSOR, (uint8_t)(220 - a)), 6, 1);
        return;
    }

    const HelpPage& pg = kPages[help_page_];
    d.text(HLP_X + 6, HLP_Y + 5, "MANUAL", pal::HEADER);
    d.text(HLP_X + 56, HLP_Y + 5, pg.title, pal::FG);
    {
        char pb[8];
        // help_page_ is bounded by N_PAGES; the cast tells the optimiser that too,
        // otherwise -Wformat-truncation assumes a full int and warns.
        std::snprintf(pb, sizeof(pb), "%d/%d", (int)(help_page_ + 1) & 0xFF, N_PAGES & 0xFF);
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
        int base = (help_page_ == PG_FX_A) ? 0 : FX_SPLIT;
        int n    = (help_page_ == PG_FX_A) ? FX_SPLIT : N_HELP_FX - FX_SPLIT;
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
    if (y < HLP_Y + HLP_HDR) { begin_overlay_close(help_closing_); return true; }
    // footer: left third prev, right third next, middle close
    if (y >= HLP_FOOT_Y - 3) {
        if (x < HLP_X + HLP_W / 3)          help_page_ = wrap_index(help_page_, -1, N_PAGES);
        else if (x >= HLP_X + 2 * HLP_W / 3) help_page_ = wrap_index(help_page_, +1, N_PAGES);
        else                                 begin_overlay_close(help_closing_);
        return true;
    }
    // body: left/right half = page flip (fast thumbing)
    if (x < HLP_X + HLP_W / 2) help_page_ = wrap_index(help_page_, -1, N_PAGES);
    else                       help_page_ = wrap_index(help_page_, +1, N_PAGES);
    return true;
}

} // namespace trackr::ui
