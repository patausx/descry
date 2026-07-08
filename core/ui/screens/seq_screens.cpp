// App: sequencer screens — phrase / chain / song views (update + draw).
// Split out of app.cpp. Owns update_phrase, clear_step, clear_cell,
// update_chain, update_song, edit_value, draw_phrase, draw_chain, draw_song.
#include "../app.h"
#include "../ui_internal.h"
#include "../../sequencer/scale.h"
#include "../../audio/fixed.h"
#include "../../sequencer/fx.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace trackr::ui {

namespace {
// === FX command catalogue (shared by the cmd cycle and the FX help view) ===
// essential set (M8-style). order = most-used first. removed: V(=dup of VEL),
// L/M/N/W(=MG, lives on instrument).
// full command set the phrase editor can type: everything the player executes.
// ordering = help-view layout: pitch/arp | volume | filter | crush | sends |
// pan | LFO section | note lifecycle | conditions | flow/tempo.
static const char kFxLetters[] = "PJVFQYBSEGALMNWRKXDOCHT";
constexpr int N_FX_CMDS = (int)sizeof(kFxLetters) - 1;   // 23

// === FX value widget: a typed visual indicator for the hint bar ===
// draws a small graphic that represents the FX value's meaning, at (x,y), width w.
// bx/by = bar origin; everything fits in a ~120px strip.
static void draw_fx_widget(Draw& d, uint8_t cmd, uint8_t val, int x, int y, int w) {
    using namespace seq;
    const int h = 6;
    const Color track = pal::GRID;   // unfilled bar background
    const Color fillc = pal::HEADER;  // ochre fill
    const Color markc = pal::CURSOR;  // dusty-rose marker

    switch (cmd) {
        case fx_cmd::PAN: {
            // two end posts + a centre tick + a dot at the pan position.
            // val: 00=hard L, 80=centre, FF=hard R.
            d.rect(x, y - 1, 1, h + 2, fillc);             // left post
            d.rect(x + w - 1, y - 1, 1, h + 2, fillc);     // right post
            d.rect(x, y + h / 2, w, 1, track);             // the track line
            d.rect(x + w / 2, y, 1, h, pal::FG_DIM);        // centre tick (dim)
            int dot = x + (val * (w - 3)) / 255;           // dot position
            d.rect(dot, y, 3, h, markc);                   // the pan dot
            break;
        }
        case fx_cmd::PIT: {
            // bipolar bar growing from centre. 80=0, >80 right(+), <80 left(-).
            int mid = x + w / 2;
            d.rect(x, y + h / 2, w, 1, track);
            d.rect(mid, y - 1, 1, h + 2, pal::FG_DIM);      // zero line
            int p = (int)val - 0x80;                        // -128..+127
            if (p > 12) p = 12; if (p < -12) p = -12;       // matches engine clamp
            int len = (p * (w / 2)) / 12;
            if (len >= 0) d.rect(mid, y + 1, len + 1, h - 2, fillc);
            else          d.rect(mid + len, y + 1, -len, h - 2, fillc);
            break;
        }
        case fx_cmd::VOL: case fx_cmd::FIL: case fx_cmd::RES:
        case fx_cmd::SND: case fx_cmd::SDL: case fx_cmd::SRV: case fx_cmd::CRU: {
            // simple fill bar 0..FF
            d.rect(x, y, w, h, track);
            int fw = (val * w) / 255;
            d.rect(x, y, fw, h, fillc);
            break;
        }
        case fx_cmd::FTY: {
            // show the selected filter type as text
            const char* nm = "LPF";
            switch (val) { case 1: nm="HPF"; break; case 2: nm="BPF"; break;
                           case 3: nm="NTC"; break; case 4: nm="OFF"; break; }
            d.text(x, y - 1, nm, markc);
            break;
        }
        case fx_cmd::ARP: {
            // two nibbles as +hi +lo semitone offsets
            char b[12];
            std::snprintf(b, sizeof(b), "+%d +%d", (val >> 4) & 0xF, val & 0xF);
            d.text(x, y - 1, b, fillc);
            break;
        }
        case fx_cmd::EVN: {
            // "x/y" + a row of y pips with pip x lit (which pass fires)
            int ex = (val >> 4) & 0xF, ey = val & 0xF;
            if (ey <= 1) { d.text(x, y - 1, "ALWAYS", pal::FG_DIM); break; }
            if (ex < 1) ex = 1; if (ex > ey) ex = ey;
            char b[8];
            std::snprintf(b, sizeof(b), "%d/%d", ex, ey);
            d.text(x, y - 1, b, markc);
            int px = x + 24;
            for (int i = 0; i < ey && px + 3 <= x + w; ++i, px += 4)
                d.rect(px, y, 3, h, (i == ex - 1) ? markc : track);
            break;
        }
        case fx_cmd::KIL: case fx_cmd::OFF: case fx_cmd::DLY: {
            // sub-tick gauge: TICKS_PER_STEP cells, lit up to val
            int n = val; if (n > TICKS_PER_STEP) n = TICKS_PER_STEP;
            for (int i = 0; i < TICKS_PER_STEP; ++i) {
                Color c = (i < n) ? fillc : track;
                d.rect(x + i * 4, y, 3, h, c);
            }
            break;
        }
        default:
            break;   // HOP/TMP/LFO etc: number alone is clear enough
    }
}
} // anon namespace

// === FX help view (m8-style command picker on the bottom screen) ===
// two-column list of all commands: [XXX] FULL NAME. selected row highlighted;
// tap = assign+close, tap selected = confirm. drawn instead of the keyboard
// while fx_help_ is on and the phrase cursor is on an FX-cmd column.
namespace {
    constexpr int FXH_X = 4, FXH_Y = 92, FXH_W = 312, FXH_H = 146;
    constexpr int FXH_COLS = 2, FXH_ROW_H = 11;        // tight rows: 23 cmds fit 2x12
    constexpr int FXH_ROWS_VIS = 12;                   // rows per column
    constexpr int FXH_COL_W = FXH_W / FXH_COLS;        // 156
}

void App::draw_fx_help(Draw& d) {
    // unfold: the panel opens like a vertical shutter (~6 frames), contents
    // appear once it's fully open. cheap and reads as "a thing popped up".
    {
        uint32_t age = frame_ - fx_help_frame_;
        constexpr uint32_t UNFOLD = 6;
        if (age < UNFOLD) {
            int hh = (int)((FXH_H + 4) * (age + 1) / UNFOLD);
            int yy = FXH_Y - 2 + (FXH_H + 4 - hh) / 2;
            d.rect(FXH_X - 2, yy, FXH_W + 4, hh, pal::BG_HI);
            if (hh > 8) d.rect(FXH_X, yy + 2, FXH_W, hh - 4, pal::BG);
            return;
        }
    }
    d.rect(FXH_X - 2, FXH_Y - 2, FXH_W + 4, FXH_H + 4, pal::BG_HI);
    d.rect(FXH_X, FXH_Y, FXH_W, FXH_H, pal::BG);
    d.text(FXH_X + 2, FXH_Y + 2, "FX COMMANDS", pal::HEADER);
    d.text(FXH_X + FXH_W - 130, FXH_Y + 2, "A=SET B=CLOSE", pal::FG_DIM);

    const int list_y = FXH_Y + 14;
    for (int i = 0; i < N_FX_CMDS; ++i) {
        int col = i / FXH_ROWS_VIS;
        int row = i % FXH_ROWS_VIS;
        if (col >= FXH_COLS) break;
        int x = FXH_X + col * FXH_COL_W;
        int y = list_y + row * FXH_ROW_H;
        bool sel = (i == fx_help_sel_);
        if (sel) {
            uint8_t br = breathe_pulse(frame_, 48);
            d.rect(x, y - 1, FXH_COL_W - 4, FXH_ROW_H - 2,
                   lerp_color(with_alpha(pal::CURSOR, 60), with_alpha(pal::CURSOR, 110), br));
        }
        uint8_t cmd = (uint8_t)kFxLetters[i];
        d.text(x + 4, y + 2, seq::fx_name_short(cmd), sel ? pal::FG : pal::FG_HEX);
        d.text(x + 30, y + 2, seq::fx_name_long(cmd), sel ? pal::FG : pal::FG_DIM);
    }
}

// tap on the list: select; tap the already-selected row: assign + close.
bool App::fx_help_touch(int x, int y) {
    if (x < FXH_X || x >= FXH_X + FXH_W) return false;
    const int list_y = FXH_Y + 14;
    if (y < list_y || y >= list_y + FXH_ROWS_VIS * FXH_ROW_H) return false;
    int col = (x - FXH_X) / FXH_COL_W;
    int row = (y - list_y) / FXH_ROW_H;
    int i = col * FXH_ROWS_VIS + row;
    if (i < 0 || i >= N_FX_CMDS) return false;
    if (i == fx_help_sel_) {
        // confirm: write the command into the current fx slot
        if (cursor_col_ == 3 || cursor_col_ == 5 || cursor_col_ == 7) {
            int slot = (cursor_col_ - 3) / 2;
            snapshot_step(cursor_row_);
            project_.phrases[cur_phrase_].steps[cursor_row_].fx[slot].cmd = (uint8_t)kFxLetters[i];
            commit_step(cursor_row_);
            dirty = true;
        }
        fx_help_ = false;
    } else {
        fx_help_sel_ = i;
    }
    return true;
}

void App::update_phrase(const InputState& in) {
    static constexpr int N_COLS = 9;  // note inst vel fx1c fx1v fx2c fx2v fx3c fx3v

    // === selection mode (m8-style): ZL+SELECT toggles, cursor extends the range ===
    if (in.held_zl && in.select_) {
        sel_mode_ = !sel_mode_;
        if (sel_mode_) sel_anchor_ = cursor_row_;
        return;
    }
    if (sel_mode_) {
        if (in.up)   cursor_row_ = (cursor_row_ - 1 + seq::PHRASE_STEPS) % seq::PHRASE_STEPS;
        if (in.down) cursor_row_ = (cursor_row_ + 1) % seq::PHRASE_STEPS;
        int lo = sel_anchor_ < cursor_row_ ? sel_anchor_ : cursor_row_;
        int hi = sel_anchor_ < cursor_row_ ? cursor_row_ : sel_anchor_;
        // A (or ZL+X, matching the single-step chord) = copy range + exit
        if (in.a || (in.held_zl && in.x)) {
            clip_block_len_ = hi - lo + 1;
            for (int i = 0; i < clip_block_len_; ++i)
                clip_block_[i] = project_.phrases[cur_phrase_].steps[lo + i];
            sel_mode_ = false;
            return;
        }
        // X (without ZL) = CUT: copy + clear the range
        if (in.x) {
            clip_block_len_ = hi - lo + 1;
            for (int i = 0; i < clip_block_len_; ++i) {
                clip_block_[i] = project_.phrases[cur_phrase_].steps[lo + i];
                snapshot_step(lo + i);
                project_.phrases[cur_phrase_].steps[lo + i] = seq::PhraseStep{};
                commit_step(lo + i);
            }
            sel_mode_ = false;
            dirty = true;
            return;
        }
        // B = cancel selection
        if (in.b) { sel_mode_ = false; return; }
        return;   // selection mode swallows everything else
    }

    // === copy/paste step (ZL+X copy, ZL+Y paste; block paste if a block was copied) ===
    if (in.held_zl) {
        // ZL+UP/DOWN = phrase length 1..16 (polymetry). data past the end is kept.
        if (in.up || in.down) {
            auto& ph = project_.phrases[cur_phrase_];
            int len = seq::phrase_len(ph) + (in.up ? 1 : -1);
            if (len < 1) len = 1;
            if (len > seq::PHRASE_STEPS) len = seq::PHRASE_STEPS;
            ph.length = (uint8_t)len;
            dirty = true;
            edit_flash_frame_ = frame_;
            return;
        }
        if (in.x) {  // copy the step under the cursor (clears any block clipboard)
            clipboard_step_ = project_.phrases[cur_phrase_].steps[cursor_row_];
            has_clip_ = true;
            clip_block_len_ = 0;
            return;  // don't let X fall through to value editing
        }
        if (in.y && clip_block_len_ > 0) {   // paste the copied block at the cursor
            for (int i = 0; i < clip_block_len_; ++i) {
                int row = cursor_row_ + i;
                if (row >= seq::PHRASE_STEPS) break;   // clip at phrase end
                snapshot_step(row);
                project_.phrases[cur_phrase_].steps[row] = clip_block_[i];
                commit_step(row);
            }
            dirty = true;
            return;
        }
        if (in.y && has_clip_) {  // paste single step
            snapshot_step(cursor_row_);
            project_.phrases[cur_phrase_].steps[cursor_row_] = clipboard_step_;
            commit_step(cursor_row_);
            dirty = true;
            return;
        }
        if (in.y) return;  // ZL+Y with empty clipboard: swallow, no-op
    }

    // === FX help view (m8-style): SELECT on an FX-cmd column toggles the picker ===
    if (in.select_ && !in.held_zl && !in.held_l && !in.held_r &&
        (cursor_col_ == 3 || cursor_col_ == 5 || cursor_col_ == 7)) {
        fx_help_ = !fx_help_;
        fx_help_frame_ = frame_;
        if (fx_help_) {
            // seed the picker selection from the current command
            uint8_t cur = project_.phrases[cur_phrase_].steps[cursor_row_].fx[(cursor_col_ - 3) / 2].cmd;
            fx_help_sel_ = 0;
            for (int i = 0; i < N_FX_CMDS; ++i)
                if ((uint8_t)kFxLetters[i] == cur) { fx_help_sel_ = i; break; }
        }
        return;
    }
    if (fx_help_) {
        // picker owns up/down/A/B while open
        if (in.up)   fx_help_sel_ = (fx_help_sel_ - 1 + N_FX_CMDS) % N_FX_CMDS;
        if (in.down) fx_help_sel_ = (fx_help_sel_ + 1) % N_FX_CMDS;
        if (in.a) {
            int slot = (cursor_col_ - 3) / 2;
            snapshot_step(cursor_row_);
            project_.phrases[cur_phrase_].steps[cursor_row_].fx[slot].cmd = (uint8_t)kFxLetters[fx_help_sel_];
            commit_step(cursor_row_);
            fx_help_ = false;
            dirty = true;
            return;
        }
        if (in.b) { fx_help_ = false; return; }
        // left/right move the cursor to another fx column and keep the picker open
        if (in.left)  cursor_col_ = (cursor_col_ - 1 + N_COLS) % N_COLS;
        if (in.right) cursor_col_ = (cursor_col_ + 1) % N_COLS;
        if (cursor_col_ != 3 && cursor_col_ != 5 && cursor_col_ != 7) fx_help_ = false;
        return;
    }

    if (in.up)    cursor_row_ = (cursor_row_ - 1 + seq::PHRASE_STEPS) % seq::PHRASE_STEPS;
    if (in.down)  cursor_row_ = (cursor_row_ + 1) % seq::PHRASE_STEPS;
    if (in.left)  cursor_col_ = (cursor_col_ - 1 + N_COLS) % N_COLS;
    if (in.right) cursor_col_ = (cursor_col_ + 1) % N_COLS;

    // === quick erase (held R) ===
    //   R + B - clear the WHOLE step (note/inst/vel/all fx)
    //   R + A - clear only the cell under the cursor
    if (in.held_r) {
        if (in.b) { snapshot_step(cursor_row_); clear_step(cursor_row_); commit_step(cursor_row_); dirty = true; }
        if (in.a) { snapshot_step(cursor_row_); clear_cell(cursor_row_, cursor_col_); commit_step(cursor_row_); dirty = true; }
        return;  // under held_r don't edit values
    }

    // editing (each wrapped in snapshot/commit so undo captures the before/after)
    if (in.a) { snapshot_step(cursor_row_); edit_value(+1);  commit_step(cursor_row_); }
    if (in.b) { snapshot_step(cursor_row_); edit_value(-1);  commit_step(cursor_row_); }
    if (in.x) { snapshot_step(cursor_row_); edit_value(+12); commit_step(cursor_row_); }  // big step
    if (in.y) { snapshot_step(cursor_row_); edit_value(-12); commit_step(cursor_row_); }
}

// === undo/redo glue ===
// snapshot the step under the cursor BEFORE a mutation; commit records before+after.
void App::snapshot_step(int row) {
    seq::UndoStack::read_cell(project_, seq::EditKind::Step, cur_phrase_, (uint16_t)row, step_before_);
    step_snap_taken_ = true;
}

void App::commit_step(int row) {
    if (!step_snap_taken_) return;
    step_snap_taken_ = false;
    seq::EditRecord::Payload after;
    if (!seq::UndoStack::read_cell(project_, seq::EditKind::Step, cur_phrase_, (uint16_t)row, after)) return;
    // coalesce consecutive edits of the same cell within ~30 frames (~0.5s @60fps)
    undo_.record(seq::EditKind::Step, cur_phrase_, (uint16_t)row, step_before_, after, frame_, 30);
}

void App::do_undo() {
    seq::EditKind k; uint16_t a, b;
    if (!undo_.undo(project_, k, a, b)) return;
    dirty = true;
    edit_flash_frame_ = frame_;
    // move the user to where the change happened so the undo is visible
    if (k == seq::EditKind::Step) {
        cur_phrase_ = (uint8_t)a;
        cursor_row_ = (int)b;
        screen_ = Screen::Phrase;
    }
}

void App::do_redo() {
    seq::EditKind k; uint16_t a, b;
    if (!undo_.redo(project_, k, a, b)) return;
    dirty = true;
    edit_flash_frame_ = frame_;
    if (k == seq::EditKind::Step) {
        cur_phrase_ = (uint8_t)a;
        cursor_row_ = (int)b;
        screen_ = Screen::Phrase;
    }
}

// clear the whole step - reset to defaults
void App::clear_step(int row) {
    project_.phrases[cur_phrase_].steps[row] = seq::PhraseStep{};
}

// clear a single cell under the cursor
void App::clear_cell(int row, int col) {
    auto& step = project_.phrases[cur_phrase_].steps[row];
    switch (col) {
        case 0: step.note = seq::EMPTY; break;
        case 1: step.instrument = seq::EMPTY; break;
        case 2: step.velocity = 0x7F; break;
        case 3: case 5: case 7: step.fx[(col - 3) / 2].cmd = 0; break;
        case 4: case 6: case 8: step.fx[(col - 4) / 2].value = 0; break;
    }
}

void App::update_chain(const InputState& in) {
    // === ZL+SELECT = clone phrase under cursor (lsdj-style "make unique") ===
    // copies the phrase into a free slot and points THIS row at the copy;
    // every other place that used the old phrase keeps playing it untouched.
    if (in.held_zl && in.select_) {
        auto& cell = project_.chains[cur_chain_].rows[chain_row_];
        if (cell.phrase != seq::EMPTY) {
            int np = project_.clone_phrase(cell.phrase);
            if (np >= 0) {
                seq::EditRecord::Payload before, after;
                before.chain = cell;
                cell.phrase = (uint8_t)np;
                after.chain = cell;
                undo_.record(seq::EditKind::ChainRow, cur_chain_, (uint16_t)chain_row_,
                             before, after, frame_, 0);
                dirty = true;
                edit_flash_frame_ = frame_;
                std::snprintf(clone_msg_, sizeof(clone_msg_), "CLONE %02X", np);
            } else {
                std::snprintf(clone_msg_, sizeof(clone_msg_), "BANK FULL");
            }
            clone_msg_frame_ = frame_;
        }
        return;
    }

    if (in.up)    chain_row_ = (chain_row_ - 1 + seq::CHAIN_ROWS) % seq::CHAIN_ROWS;
    if (in.down)  chain_row_ = (chain_row_ + 1) % seq::CHAIN_ROWS;
    if (in.left)  chain_col_ = (chain_col_ - 1 + 2) % 2;
    if (in.right) chain_col_ = (chain_col_ + 1) % 2;

    auto& cell = project_.chains[cur_chain_].rows[chain_row_];
    int delta = 0;
    if (in.a) delta = +1;
    if (in.b) delta = -1;
    if (delta) {
        if (chain_col_ == 0) {
            int v = (cell.phrase == seq::EMPTY) ? 0 : (cell.phrase + delta);
            if (v < 0) cell.phrase = seq::EMPTY;
            else if (v >= seq::MAX_PHRASES) cell.phrase = seq::MAX_PHRASES - 1;
            else cell.phrase = v;
        } else {
            int v = cell.transpose + delta;
            if (v < -64) v = -64;
            if (v > 64) v = 64;
            cell.transpose = v;
        }
    }
    // open phrase under cursor
    if (in.select_ && cell.phrase != seq::EMPTY) {
        cur_phrase_ = cell.phrase;
        screen_ = Screen::Phrase;
    }
}

void App::update_song(const InputState& in) {
    // === ZL+SELECT = DEEP clone chain under cursor ===
    // copies the chain AND all phrases inside it into free slots, so the new
    // chain is fully independent (edit anything without touching the original).
    if (in.held_zl && in.select_) {
        auto& cell = project_.song.rows[song_row_].chain[song_col_];
        if (cell != seq::EMPTY) {
            int nc = project_.clone_chain_deep(cell);
            if (nc >= 0) {
                seq::EditRecord::Payload before, after;
                before.song_cell = cell;
                cell = (uint8_t)nc;
                after.song_cell = cell;
                undo_.record(seq::EditKind::SongCell, (uint16_t)song_row_, (uint16_t)song_col_,
                             before, after, frame_, 0);
                dirty = true;
                edit_flash_frame_ = frame_;
                std::snprintf(clone_msg_, sizeof(clone_msg_), "CLONE %02X", nc);
            } else {
                std::snprintf(clone_msg_, sizeof(clone_msg_), "BANK FULL");
            }
            clone_msg_frame_ = frame_;
        }
        return;
    }

    // === ZL+LEFT/RIGHT = flip the song view orientation ===
    // vertical M8-style list <-> horizontal DAW-style timeline (DoubleSprattt).
    // same data, same cursor - just rotated axes.
    if (in.held_zl && (in.left || in.right)) {
        song_timeline_ = !song_timeline_;
        return;
    }

    // dpad nav: in timeline mode the axes rotate with the view -
    // left/right walk time (song rows), up/down hop tracks (lanes).
    if (song_timeline_) {
        if (in.left)  song_row_ = (song_row_ - 1 + seq::SONG_ROWS) % seq::SONG_ROWS;
        if (in.right) song_row_ = (song_row_ + 1) % seq::SONG_ROWS;
        if (in.up)    song_col_ = (song_col_ - 1 + seq::NUM_TRACKS) % seq::NUM_TRACKS;
        if (in.down)  song_col_ = (song_col_ + 1) % seq::NUM_TRACKS;
    } else {
        if (in.up)    song_row_ = (song_row_ - 1 + seq::SONG_ROWS) % seq::SONG_ROWS;
        if (in.down)  song_row_ = (song_row_ + 1) % seq::SONG_ROWS;
        if (in.left)  song_col_ = (song_col_ - 1 + seq::NUM_TRACKS) % seq::NUM_TRACKS;
        if (in.right) song_col_ = (song_col_ + 1) % seq::NUM_TRACKS;
    }

    // === live mode: Y = queue the chain under the cursor on its track (launches
    // on the next 16-step bar), X = queue a stop for the track. same press again
    // cancels. when nothing plays, Y starts immediately and sets the bar clock.
    if (in.y) {
        uint8_t c = project_.song.rows[song_row_].chain[song_col_];
        if (c != seq::EMPTY) player_.queue_chain(song_col_, c);
        return;
    }
    if (in.x && !in.held_zl) {
        if (player_.track_state(song_col_).playing)
            player_.queue_chain(song_col_, seq::Player::QUEUE_STOP);
        return;
    }

    auto& cell = project_.song.rows[song_row_].chain[song_col_];
    int delta = 0;
    if (in.a) delta = +1;
    if (in.b) delta = -1;
    if (delta) {
        int v = (cell == seq::EMPTY) ? 0 : (cell + delta);
        if (v < 0) cell = seq::EMPTY;
        else if (v >= seq::MAX_CHAINS) cell = seq::MAX_CHAINS - 1;
        else cell = v;
    }
    if (in.select_ && cell != seq::EMPTY) {
        cur_chain_ = cell;
        screen_ = Screen::Chain;
    }
}

void App::edit_value(int delta) {
    dirty = true;
    edit_flash_frame_ = frame_;   // animation: kick off value-flash on the cell under the cursor
    auto& step = project_.phrases[cur_phrase_].steps[cursor_row_];
    switch (cursor_col_) {
        case 0: {
            // note editing honors the song key: +/-1 walks IN-SCALE notes
            // (chromatic when scale OFF - mask passes everything). +/-12 stays
            // an octave jump. stored notes are never rewritten by the scale.
            const uint8_t st = project_.song.scale_type;
            const uint8_t sr = project_.song.scale_root;
            int v;
            if (step.note == seq::EMPTY) {
                // sticky entry (m8-style): an empty step takes the LAST note you
                // entered, not a hardcoded C4 - melodies build way faster.
                v = seq::scale_snap(st, sr, last_note_entered_);
            } else if (delta == 1 || delta == -1) {
                v = seq::scale_step(st, sr, step.note, delta);
                if (v == (int)step.note && delta < 0) v = -1;   // bottom of scale -> clear
            } else {
                v = (int)step.note + delta;
            }
            if (v < 0) step.note = seq::EMPTY;
            else if (v > 127) step.note = 127;
            else step.note = (uint8_t)v;
            // if editing a note for the first time - set instrument 0
            if (step.instrument == seq::EMPTY) step.instrument = cur_inst_;
            if (step.note != seq::EMPTY) last_note_entered_ = step.note;
            break;
        }
        case 1: {
            int v = (step.instrument == seq::EMPTY) ? 0 : ((int)step.instrument + delta);
            if (v < 0) step.instrument = seq::EMPTY;
            else if (v >= seq::MAX_INSTRUMENTS) step.instrument = seq::MAX_INSTRUMENTS - 1;
            else step.instrument = (uint8_t)v;
            break;
        }
        case 2: {
            int v = (int)step.velocity + delta;
            if (v < 0) v = 0;
            if (v > 127) v = 127;
            step.velocity = (uint8_t)v;
            break;
        }
        // fx slots - cmd is a letter, step through valid ones
        case 3: case 5: case 7: {
            int slot = (cursor_col_ - 3) / 2;
            uint8_t cur = step.fx[slot].cmd;
            int idx = -1;
            if (cur == 0) idx = -1;
            else for (int i = 0; i < N_FX_CMDS; ++i) if ((uint8_t)kFxLetters[i] == cur) { idx = i; break; }
            idx += delta;
            if (idx < -1) idx = -1;
            if (idx >= N_FX_CMDS) idx = N_FX_CMDS - 1;
            if (idx == -1) step.fx[slot].cmd = 0;
            else step.fx[slot].cmd = (uint8_t)kFxLetters[idx];
            break;
        }
        case 4: case 6: case 8: {
            int slot = (cursor_col_ - 4) / 2;
            int vmax = seq::fx_value_max(step.fx[slot].cmd);
            int v = (int)step.fx[slot].value + delta;
            if (v < 0) v = 0;
            if (v > vmax) v = vmax;
            step.fx[slot].value = (uint8_t)v;
            break;
        }
    }
}

// === draw ===

void App::draw_phrase(Draw& d) {
    // layout: header (Y=20), 16 rows x columns
    // compact: row#  NOTE INST VEL  FX1 FX2 FX3
    // 6 main columns
    constexpr int Y0 = 22;
    constexpr int ROW_H = 12;   // 16 rows * 12 = 192px (34..226), leaves the FX hint bar
                                // at y=228 a clean strip below the grid (no more step-16 overlap)
    constexpr int COL_X[] = {
        14,  // row#
        38,  // note
        72,  // inst
        100, // vel
        132, // fx1 cmd+val
        180, // fx2
        228  // fx3
    };

    // header row
    d.text(COL_X[0], Y0, "##", pal::HEADER);
    d.text(COL_X[1], Y0, "NOT", pal::HEADER);
    d.text(COL_X[2], Y0, "IN", pal::HEADER);
    d.text(COL_X[3], Y0, "VL", pal::HEADER);
    d.text(COL_X[4], Y0, "FX1", pal::HEADER);
    d.text(COL_X[5], Y0, "FX2", pal::HEADER);
    d.text(COL_X[6], Y0, "FX3", pal::HEADER);

    // phrase length readout (right side of the header). accent when shortened.
    {
        const auto& ph = project_.phrases[cur_phrase_];
        int plen = seq::phrase_len(ph);
        char lb[12];
        std::snprintf(lb, sizeof(lb), "LEN %02X", plen);
        d.text(340, Y0, lb, plen < seq::PHRASE_STEPS ? pal::CURSOR : pal::FG_DIM);
    }

    int playing_step = -1;
    if (player_.playing() && player_.track_state(0).phrase_id == cur_phrase_) {
        // sync with the playing phrase if we're viewing this same phrase during playback
        playing_step = player_.track_state(0).step;
    }

    for (int row = 0; row < seq::PHRASE_STEPS; ++row) {
        int y = Y0 + 12 + row * ROW_H;
        const auto& step = project_.phrases[cur_phrase_].steps[row];

        // zebra
        if (row & 1) d.rect(0, y - 1, 400, ROW_H, pal::BG_HI);

        // playhead trail: current step bright + 2 previous fading out (Forza-style feedback)
        if (playing_step >= 0) {
            // how many steps back from the playing one (accounting for wrap)
            int back = (playing_step - row + seq::PHRASE_STEPS) % seq::PHRASE_STEPS;
            if (back == 0) {
                // steady backdrop + a gradient that flows across the row on each
                // step (breathes with the beat instead of blinking)
                d.rect(0, y - 1, 400, ROW_H, with_alpha(pal::PLAY, 0x60));
                beat_glow(d, 0, y - 1, 400, ROW_H, frame_ - step_change_frame_, pal::PLAY);
                // bright markers at the edges of the current step
                d.rect(0, y - 1, 3, ROW_H, pal::PLAY);
                d.rect(397, y - 1, 3, ROW_H, pal::PLAY);
            } else if (back <= 2) {
                // trail tail: the farther back - the more transparent
                uint8_t a = (back == 1) ? 0x30 : 0x18;
                d.rect(0, y - 1, 400, ROW_H, with_alpha(pal::PLAY, a));
            }
        }

        // selection range highlight (m8-style block selection)
        if (sel_mode_) {
            int lo = sel_anchor_ < cursor_row_ ? sel_anchor_ : cursor_row_;
            int hi = sel_anchor_ < cursor_row_ ? cursor_row_ : sel_anchor_;
            if (row >= lo && row <= hi) {
                uint8_t br = breathe_pulse(frame_, 48);
                d.rect(0, y - 1, 400, ROW_H, with_alpha(pal::CURSOR, (uint8_t)(0x28 + br / 8)));
                d.rect(0, y - 1, 2, ROW_H, pal::CURSOR);
                d.rect(398, y - 1, 2, ROW_H, pal::CURSOR);
            }
        }

        // row number
        d.hex2(COL_X[0], y, row, pal::FG_DIM);

        // note. out-of-key notes get the accent colour as a gentle warning
        // (data untouched - the scale is a lens, not an eraser).
        Color note_c = (step.note == seq::EMPTY) ? pal::FG_DIM : pal::FG;
        if (step.note != seq::EMPTY &&
            !seq::scale_has(project_.song.scale_type, project_.song.scale_root, step.note))
            note_c = pal::CURSOR;
        d.note(COL_X[1], y, step.note, note_c);

        // instrument
        Color inst_c = (step.instrument == seq::EMPTY) ? pal::FG_DIM : pal::FG_HEX;
        if (step.instrument == seq::EMPTY) d.text(COL_X[2], y, "--", inst_c);
        else d.hex2(COL_X[2], y, step.instrument, inst_c);

        // velocity
        d.hex2(COL_X[3], y, step.velocity, pal::FG_DIM);

        // fx slots
        for (int s = 0; s < 3; ++s) {
            int x = COL_X[4 + s];
            if (step.fx[s].cmd == 0) {
                d.text(x, y, "---", pal::FG_DIM);
            } else {
                // single command letter + 2 hex value digits
                char letter = (char)step.fx[s].cmd;
                if (letter < 0x20 || letter > 0x7E) letter = '?';
                d.glyph(x, y, letter, pal::FG_HEX);
                d.hex2(x + 7, y, step.fx[s].value, pal::FG);
            }
        }

        // dead zone: steps past the phrase length are kept but don't play - shade them
        {
            int plen = seq::phrase_len(project_.phrases[cur_phrase_]);
            if (row >= plen) {
                d.rect(0, y - 1, 400, ROW_H, with_alpha(pal::PANEL, 0xA0));
                if (row == plen) d.rect(0, y - 1, 400, 1, pal::CURSOR);  // end-of-phrase line
            }
        }

        // cursor
        if (row == cursor_row_) {
            // highlight the column under the cursor
            int cx, cw;
            switch (cursor_col_) {
                case 0: cx = COL_X[1] - 1; cw = 20; break;  // note
                case 1: cx = COL_X[2] - 1; cw = 14; break;  // inst
                case 2: cx = COL_X[3] - 1; cw = 14; break;  // vel
                case 3: cx = COL_X[4] - 1; cw = 14; break;  // fx1c
                case 4: cx = COL_X[4] + 13; cw = 14; break;  // fx1v
                case 5: cx = COL_X[5] - 1; cw = 14; break;
                case 6: cx = COL_X[5] + 13; cw = 14; break;
                case 7: cx = COL_X[6] - 1; cw = 14; break;
                case 8: cx = COL_X[6] + 13; cw = 14; break;
                default: cx = COL_X[1] - 1; cw = 20;
            }
            // value-flash: brief cell-background flash at the moment a value is edited
            uint8_t ft = edit_flash_t();
            if (ft) d.rect(cx, y - 1, cw, 10, with_alpha(pal::FLASH, ft / 2));
            // breathing corner-bracket selector (M8-style). pulses dusty-rose between
            // a dim and a bright tint so the cursor gently "breathes".
            uint8_t br = breathe_pulse(frame_, 64);
            ui::Color cur = lerp_color(with_alpha(pal::CURSOR, 130), pal::CURSOR, br);
            d.corner_brackets(cx, y - 1, cw, 10, cur, 3, 1);
        }
    }

    // === FX hint bar: typed inspector when the cursor is on an FX cell ===
    // layout:  [FXn] [MNEMONIC]  <visual widget>  [hexval]
    // cursor_col_ 3/5/7 = fx cmd of slot 0/1/2; 4/6/8 = its value.
    if (cursor_col_ >= 3 && cursor_col_ <= 8) {
        int slot = (cursor_col_ - 3) / 2;
        const auto& step = project_.phrases[cur_phrase_].steps[cursor_row_];
        uint8_t cmd = step.fx[slot].cmd;
        uint8_t val = step.fx[slot].value;
        const int HY = 228;

        // opaque backing bar so the phrase grid underneath doesn't bleed through
        // (the last step rows reach down to ~y=242 and were colliding with this text).
        d.rect(0, HY - 2, 400, 14, pal::BG_HI);
        d.rect(0, HY - 3, 400, 1, pal::HEADER);   // thin ochre separator on top

        // slot label
        char sl[5];
        std::snprintf(sl, sizeof(sl), "FX%d", slot + 1);
        d.text(8, HY, sl, pal::FG_DIM, 1);

        if (cmd == 0) {
            d.text(34, HY, "---  (A/B pick effect)", pal::FG_DIM, 1);
        } else {
            // 3-letter mnemonic, ALL CAPS, accent colour
            d.text(34, HY, seq::fx_name_short(cmd), pal::HEADER, 1);
            // typed visual widget in the middle strip
            draw_fx_widget(d, cmd, val, 64, HY, 56);
            // raw hex value on the right
            d.hex2(130, HY, val, pal::FG, 1);
            // full english name, dim, far right
            d.text(150, HY, seq::fx_name_long(cmd), pal::FG_DIM, 1);
        }
    }
}
void App::draw_chain(Draw& d) {
    constexpr int Y0 = 22;
    constexpr int ROW_H = 13;

    d.text(40, Y0, "##", pal::HEADER);
    d.text(80, Y0, "PHR", pal::HEADER);
    d.text(140, Y0, "TSP", pal::HEADER);

    // which chain row is currently playing? (any track whose active chain is this one)
    int playing_row = -1;
    if (player_.playing()) {
        for (int t = 0; t < seq::NUM_TRACKS; ++t) {
            const auto& tss = player_.track_state(t);
            if (tss.playing && tss.chain_id != seq::EMPTY && tss.chain_id == cur_chain_) {
                playing_row = tss.chain_row;
                break;
            }
        }
    }

    for (int row = 0; row < seq::CHAIN_ROWS; ++row) {
        int y = Y0 + 12 + row * ROW_H;
        const auto& r = project_.chains[cur_chain_].rows[row];

        if (row & 1) d.rect(0, y - 1, 400, ROW_H, pal::BG_HI);

        // playhead: highlight the row currently playing this chain, with the same
        // flowing-gradient feedback as the phrase view.
        if (row == playing_row) {
            d.rect(0, y - 1, 400, ROW_H, with_alpha(pal::PLAY, 0x60));
            beat_glow(d, 0, y - 1, 400, ROW_H, frame_ - step_change_frame_, pal::PLAY);
            d.rect(0, y - 1, 3, ROW_H, pal::PLAY);
            d.rect(397, y - 1, 3, ROW_H, pal::PLAY);
        }

        d.hex2(40, y, row, pal::FG_DIM);
        if (r.phrase == seq::EMPTY) d.text(80, y, "--", pal::FG_DIM);
        else d.hex2(80, y, r.phrase, pal::FG);

        // transpose as signed
        if (r.transpose == 0) d.text(140, y, "00", pal::FG_DIM);
        else {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%+03d", r.transpose);
            d.text(140, y, buf, pal::FG);
        }

        if (row == chain_row_) {
            int cx = (chain_col_ == 0) ? 79 : 139;
            uint8_t br = breathe_pulse(frame_, 64);
            ui::Color cur = lerp_color(with_alpha(pal::CURSOR, 130), pal::CURSOR, br);
            d.corner_brackets(cx, y - 1, 14, 10, cur, 3, 1);
        }
    }
}

void App::draw_song(Draw& d) {
    if (song_timeline_) { draw_song_timeline(d); return; }

    constexpr int Y0 = 22;
    constexpr int ROW_H = 11;
    constexpr int VISIBLE = 18;

    // show a window around song_row
    int top_row = song_row_ - VISIBLE / 2;
    if (top_row < 0) top_row = 0;
    if (top_row > seq::SONG_ROWS - VISIBLE) top_row = seq::SONG_ROWS - VISIBLE;

    d.text(8, Y0, "##", pal::HEADER);
    // live-mode bar countdown: steps until the next quantized launch boundary.
    // shown only while something is queued (that's when you're counting).
    {
        bool any_q = false;
        for (int t = 0; t < seq::NUM_TRACKS; ++t)
            if (player_.queued(t) != seq::EMPTY) { any_q = true; break; }
        if (any_q && player_.playing()) {
            char qb[12];
            std::snprintf(qb, sizeof(qb), "BAR-%02d", player_.steps_to_bar());
            uint8_t br = breathe_pulse(frame_, 32);
            d.text(344, Y0, qb, lerp_color(pal::FG_DIM, pal::CURSOR, br));
        }
    }
    for (int t = 0; t < seq::NUM_TRACKS; ++t) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "T%d", t);
        d.text(40 + t * 42, Y0, buf, pal::HEADER);
        // mini cutoff/crush indicators alongside
        auto& tr = mixer_.track(t);
        // cutoff: horizontal bar 12px, filled by cutoff
        int cw = (tr.cutoff * 12) / fx::Q15_ONE;
        // cutoff bar color changes with filter_type - a visual indicator with no extra pixels
        uint32_t fc = pal::HEADER;  // LPF - ochre
        switch (tr.filter_type) {
            case ::trackr::dsp::FilterType::LPF:   fc = pal::HEADER; break;  // ochre
            case ::trackr::dsp::FilterType::HPF:   fc = 0xFFAC9086; break;  // rose
            case ::trackr::dsp::FilterType::BPF:   fc = 0xFFB4AB8F; break;  // khaki
            case ::trackr::dsp::FilterType::Notch: fc = 0xFF796C64; break;  // warm grey-brown
            case ::trackr::dsp::FilterType::Off:   fc = pal::GRID; break;  // steel grey
        }
        d.rect(40 + t * 42 + 14, Y0 + 2, 12, 2, pal::GRID);
        d.rect(40 + t * 42 + 14, Y0 + 2, cw, 2, fc);
        // bits: height-bar on the right
        if (tr.bits < 16) {
            int bh = (16 - tr.bits) * 8 / 15;
            d.rect(40 + t * 42 + 27, Y0, 2, bh, pal::HEADER);
        }
        // live queue badge under the track header: queued chain id (or STP)
        uint8_t q = player_.queued(t);
        if (q != seq::EMPTY) {
            uint8_t br = breathe_pulse(frame_, 40);
            ui::Color qc = lerp_color(with_alpha(pal::CURSOR, 140), pal::CURSOR, br);
            if (q == seq::Player::QUEUE_STOP) d.text(40 + t * 42 + 14, Y0 + 5, "STP", qc, 1);
            else                              d.hex2(40 + t * 42 + 14, Y0 + 5, q, qc, 1);
        }
    }

    // === song playhead trail bookkeeping: detect per-track row moves ===
    // (cached here because draw runs every frame and rows move rarely)
    for (int t = 0; t < seq::NUM_TRACKS; ++t) {
        const auto& tps = player_.track_state(t);
        uint16_t cur = (tps.playing && tps.song_mode_) ? (uint16_t)tps.song_row : 0xFFFF;
        if (cur != song_ph_row_[t]) {
            song_ph_prev_[t]  = song_ph_row_[t];
            song_ph_row_[t]   = cur;
            song_ph_frame_[t] = frame_;
        }
    }

    for (int i = 0; i < VISIBLE; ++i) {
        int row = top_row + i;
        int y = Y0 + 12 + i * ROW_H;
        const auto& sr = project_.song.rows[row];

        if (row & 0xF) {
            // normal
        } else {
            // every 16th highlighted
            d.rect(0, y - 1, 400, ROW_H, pal::BG_HI);
        }

        // per-track playheads: each track advances through the song independently
        // (m8 model - chains of different lengths drift). so the playhead is a
        // CELL highlight per track, not a full-row line.
        for (int t = 0; t < seq::NUM_TRACKS; ++t) {
            const auto& tps = player_.track_state(t);
            int cx = 40 + t * 42;
            // ghost trail: the cell we just left fades out over ~24 frames
            if (song_ph_prev_[t] != 0xFFFF && (int)song_ph_prev_[t] == row) {
                uint32_t age = frame_ - song_ph_frame_[t];
                if (age < 24) {
                    uint8_t a = (uint8_t)(150 - age * 150 / 24);
                    d.rect(cx - 4, y - 1, 30, ROW_H, with_alpha(pal::PLAY_BG, a));
                }
            }
            if (!tps.playing || !tps.song_mode_ || (int)tps.song_row != row) continue;
            // cell backdrop + a small gradient flowing through it on each beat
            d.rect(cx - 4, y - 1, 30, ROW_H, with_alpha(pal::PLAY_BG, 0xC0));
            beat_glow(d, cx - 4, y - 1, 30, ROW_H, frame_ - step_change_frame_, pal::PLAY, 12);
            // left tick grows as the chain advances (row progress at a glance)
            int th = 2 + (int)tps.chain_row * (ROW_H - 2) / (seq::CHAIN_ROWS - 1);
            d.rect(cx - 4, y - 1, 2, th, pal::PLAY);
        }

        d.hex2(8, y, row & 0xFF, pal::FG_DIM);

        for (int t = 0; t < seq::NUM_TRACKS; ++t) {
            uint8_t c = sr.chain[t];
            if (c == seq::EMPTY) d.text(40 + t * 42, y, "--", pal::FG_DIM);
            else d.hex2(40 + t * 42, y, c, pal::FG);
        }

        if (row == song_row_) {
            int cx = 39 + song_col_ * 42;
            uint8_t br = breathe_pulse(frame_, 64);
            ui::Color cur = lerp_color(with_alpha(pal::CURSOR, 130), pal::CURSOR, br);
            d.corner_brackets(cx, y - 1, 14, 10, cur, 3, 1);
        }
    }
}

// === horizontal song timeline (M01D/DAW mental model) ===
// tracks are horizontal lanes, time flows left to right. same song data and
// cursor as the vertical list - ZL+LEFT/RIGHT flips between the two.
// per-track playheads drift independently (polymeter is VISIBLE here).
void App::draw_song_timeline(Draw& d) {
    constexpr int Y0      = 22;    // column-number header line
    constexpr int LX      = 26;    // lanes start x (track labels live left of it)
    constexpr int CELL_W  = 17;
    constexpr int COLS    = 22;    // 26 + 22*17 = 400
    constexpr int LANE_Y0 = 34;
    constexpr int LANE_H  = 23;    // 8 lanes -> 34..218

    // window of song rows around the cursor
    int left_col = song_row_ - COLS / 2;
    if (left_col < 0) left_col = 0;
    if (left_col > seq::SONG_ROWS - COLS) left_col = seq::SONG_ROWS - COLS;

    // === playhead trail bookkeeping (same cache as the vertical view) ===
    for (int t = 0; t < seq::NUM_TRACKS; ++t) {
        const auto& tps = player_.track_state(t);
        uint16_t cur = (tps.playing && tps.song_mode_) ? (uint16_t)tps.song_row : 0xFFFF;
        if (cur != song_ph_row_[t]) {
            song_ph_prev_[t]  = song_ph_row_[t];
            song_ph_row_[t]   = cur;
            song_ph_frame_[t] = frame_;
        }
    }

    // column stripes (every 16th row) + header numbers (every 4th)
    for (int c = 0; c < COLS; ++c) {
        int row = left_col + c;
        int x = LX + c * CELL_W;
        if ((row & 0xF) == 0)
            d.rect(x - 2, LANE_Y0 - 2, CELL_W, seq::NUM_TRACKS * LANE_H + 2, pal::BG_HI);
        if ((row & 3) == 0)
            d.hex2(x, Y0, (uint8_t)(row & 0xFF), pal::FG_DIM);
    }
    d.text(2, Y0, "##", pal::HEADER);

    for (int t = 0; t < seq::NUM_TRACKS; ++t) {
        int ly = LANE_Y0 + t * LANE_H;
        // lane separator
        d.rect(0, ly - 1, 400, 1, with_alpha(pal::GRID, 60));
        // track label + live-queue badge
        char tb[4];
        std::snprintf(tb, sizeof(tb), "T%d", t);
        d.text(2, ly + 3, tb, t == song_col_ ? pal::CURSOR : pal::HEADER);
        uint8_t q = player_.queued(t);
        if (q != seq::EMPTY) {
            uint8_t br = breathe_pulse(frame_, 40);
            ui::Color qc = lerp_color(with_alpha(pal::CURSOR, 140), pal::CURSOR, br);
            if (q == seq::Player::QUEUE_STOP) d.text(2, ly + 12, "STP", qc, 1);
            else                              d.hex2(2, ly + 12, q, qc, 1);
        }

        const auto& tps = player_.track_state(t);
        for (int c = 0; c < COLS; ++c) {
            int row = left_col + c;
            int x = LX + c * CELL_W;

            // ghost trail: the cell the playhead just left fades out
            if (song_ph_prev_[t] != 0xFFFF && (int)song_ph_prev_[t] == row) {
                uint32_t age = frame_ - song_ph_frame_[t];
                if (age < 24) {
                    uint8_t a = (uint8_t)(150 - age * 150 / 24);
                    d.rect(x - 2, ly + 1, CELL_W, LANE_H - 2, with_alpha(pal::PLAY_BG, a));
                }
            }
            // playhead cell + chain progress bar along the bottom edge
            if (tps.playing && tps.song_mode_ && (int)tps.song_row == row) {
                d.rect(x - 2, ly + 1, CELL_W, LANE_H - 2, with_alpha(pal::PLAY_BG, 0xC0));
                beat_glow(d, x - 2, ly + 1, CELL_W, LANE_H - 2,
                          frame_ - step_change_frame_, pal::PLAY, 12);
                int pw = 2 + (int)tps.chain_row * (CELL_W - 2) / (seq::CHAIN_ROWS - 1);
                d.rect(x - 2, ly + LANE_H - 3, pw, 2, pal::PLAY);
            }

            uint8_t ch = project_.song.rows[row].chain[t];
            if (ch == seq::EMPTY) d.text(x, ly + 7, "--", pal::FG_DIM);
            else                  d.hex2(x, ly + 7, ch, pal::FG);

            if (row == song_row_ && t == song_col_) {
                uint8_t br = breathe_pulse(frame_, 64);
                ui::Color cur = lerp_color(with_alpha(pal::CURSOR, 130), pal::CURSOR, br);
                d.corner_brackets(x - 2, ly + 5, 16, 12, cur, 3, 1);
            }
        }
    }

    // bottom strip: view hint + live-mode bar countdown
    d.text(2, 228, "ZL+< > LIST VIEW", pal::GRID);
    {
        bool any_q = false;
        for (int t = 0; t < seq::NUM_TRACKS; ++t)
            if (player_.queued(t) != seq::EMPTY) { any_q = true; break; }
        if (any_q && player_.playing()) {
            char qb[12];
            std::snprintf(qb, sizeof(qb), "BAR-%02d", player_.steps_to_bar());
            uint8_t br = breathe_pulse(frame_, 32);
            d.text(344, 228, qb, lerp_color(pal::FG_DIM, pal::CURSOR, br));
        }
    }
}

} // namespace trackr::ui
