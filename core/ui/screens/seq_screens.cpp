// App: sequencer screens — phrase / chain / song views (update + draw).
// Split out of app.cpp. Owns update_phrase, clear_step, clear_cell,
// update_chain, update_song, edit_value, draw_phrase, draw_chain, draw_song.
#include "../app.h"
#include "../ui_internal.h"
#include "../../sequencer/scale.h"
#include "../../audio/fixed.h"
#include "../../sequencer/fx.h"
#include "../../synth/fm.h"
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

    constexpr int PT_X = 24, PT_Y = 94, PT_W = 272, PT_ROW_H = 11;
    static const char* const kPhraseToolNames[] = {
        "ROTATE UP", "ROTATE DOWN", "REVERSE",
        "TRANSPOSE +1", "TRANSPOSE -1", "OCTAVE +12", "OCTAVE -12",
        "VELOCITY +8", "VELOCITY -8", "VELOCITY RAMP UP", "VELOCITY RAMP DOWN",
    };
    constexpr int PT_COUNT = (int)(sizeof(kPhraseToolNames) / sizeof(kPhraseToolNames[0]));
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

void App::open_phrase_tools() {
    const auto& ph = project_.phrases[cur_phrase_];
    phrase_tools_range_ = sel_mode_;
    if (sel_mode_) {
        phrase_tools_lo_ = std::min(sel_anchor_, cursor_row_);
        phrase_tools_hi_ = std::max(sel_anchor_, cursor_row_);
    } else {
        phrase_tools_lo_ = 0;
        phrase_tools_hi_ = seq::phrase_len(ph) - 1;
    }
    phrase_tools_on_ = true;
    fx_help_ = false;
}

void App::apply_phrase_tool(PhraseTool tool) {
    int lo = phrase_tools_lo_, hi = phrase_tools_hi_;
    if (lo < 0) lo = 0;
    if (hi >= seq::PHRASE_STEPS) hi = seq::PHRASE_STEPS - 1;
    if (lo > hi) return;

    auto& ph = project_.phrases[cur_phrase_];
    seq::PhraseStep before[seq::PHRASE_STEPS];
    for (int r = lo; r <= hi; ++r) before[r] = ph.steps[r];

    const int n = hi - lo + 1;
    switch (tool) {
        case PhraseTool::RotateUp:
            if (n > 1) {
                seq::PhraseStep first = ph.steps[lo];
                for (int r = lo; r < hi; ++r) ph.steps[r] = ph.steps[r + 1];
                ph.steps[hi] = first;
            }
            break;
        case PhraseTool::RotateDown:
            if (n > 1) {
                seq::PhraseStep last = ph.steps[hi];
                for (int r = hi; r > lo; --r) ph.steps[r] = ph.steps[r - 1];
                ph.steps[lo] = last;
            }
            break;
        case PhraseTool::Reverse:
            for (int a = lo, b = hi; a < b; ++a, --b) std::swap(ph.steps[a], ph.steps[b]);
            break;
        case PhraseTool::TransposeUp:
        case PhraseTool::TransposeDown:
        case PhraseTool::OctaveUp:
        case PhraseTool::OctaveDown: {
            int delta = tool == PhraseTool::TransposeUp ? 1
                      : tool == PhraseTool::TransposeDown ? -1
                      : tool == PhraseTool::OctaveUp ? 12 : -12;
            for (int r = lo; r <= hi; ++r) {
                if (ph.steps[r].note == seq::EMPTY) continue;
                int note = (int)ph.steps[r].note + delta;
                if (note < 0) note = 0;
                if (note > 127) note = 127;
                ph.steps[r].note = (uint8_t)note;
            }
            break;
        }
        case PhraseTool::VelocityUp:
        case PhraseTool::VelocityDown: {
            int delta = tool == PhraseTool::VelocityUp ? 8 : -8;
            for (int r = lo; r <= hi; ++r) {
                if (ph.steps[r].note == seq::EMPTY) continue;
                int vel = (int)ph.steps[r].velocity + delta;
                if (vel < 1) vel = 1;
                if (vel > 127) vel = 127;
                ph.steps[r].velocity = (uint8_t)vel;
            }
            break;
        }
        case PhraseTool::RampUp:
        case PhraseTool::RampDown:
            for (int r = lo; r <= hi; ++r) {
                if (ph.steps[r].note == seq::EMPTY) continue;
                int pos = r - lo;
                int vel = n <= 1 ? 127 : 32 + pos * 95 / (n - 1);
                if (tool == PhraseTool::RampDown) vel = 159 - vel;
                ph.steps[r].velocity = (uint8_t)vel;
            }
            break;
        default: break;
    }

    // A transform is one user action even though the compact undo ring stores
    // per-row deltas. Grouping preserves a single ZL+B undo/redo operation.
    // Record only changed rows: no-op records do not exist, and this also keeps
    // a sparse velocity operation comfortably within the small fixed ring.
    bool changed = false;
    undo_.begin_group();
    for (int r = lo; r <= hi; ++r) {
        if (std::memcmp(&before[r], &ph.steps[r], sizeof(seq::PhraseStep)) == 0) continue;
        changed = true;
        seq::EditRecord::Payload b{}, a{};
        b.step = before[r];
        a.step = ph.steps[r];
        undo_.record(seq::EditKind::Step, cur_phrase_, (uint16_t)r, b, a, frame_, 0);
    }
    undo_.end_group();
    if (changed) {
        edit_flash_frame_ = frame_;
        dirty = true;
    }
    phrase_tools_on_ = false;
    sel_mode_ = false;
}

void App::draw_phrase_tools(Draw& d) {
    constexpr int H = 20 + PT_COUNT * PT_ROW_H;
    d.rect(PT_X - 2, PT_Y - 2, PT_W + 4, H + 4, pal::BG_HI);
    d.rect(PT_X, PT_Y, PT_W, H, pal::BG);
    char title[40];
    std::snprintf(title, sizeof(title), "PHRASE TOOLS  %02X-%02X%s",
                  phrase_tools_lo_, phrase_tools_hi_, phrase_tools_range_ ? " SEL" : "");
    d.text(PT_X + 6, PT_Y + 4, title, pal::HEADER);
    d.text(PT_X + PT_W - 78, PT_Y + 4, "A=DO B=X", pal::FG_DIM);
    for (int i = 0; i < PT_COUNT; ++i) {
        int y = PT_Y + 18 + i * PT_ROW_H;
        bool sel = i == phrase_tool_sel_;
        if (sel) d.rect(PT_X + 3, y - 1, PT_W - 6, PT_ROW_H, with_alpha(pal::CURSOR, 90));
        d.text(PT_X + 9, y + 1, kPhraseToolNames[i], sel ? pal::FG : pal::FG_DIM);
    }
}

bool App::phrase_tools_touch(int x, int y) {
    if (x < PT_X || x >= PT_X + PT_W) return false;
    int row = (y - (PT_Y + 18)) / PT_ROW_H;
    if (y < PT_Y + 18 || row < 0 || row >= PT_COUNT) return false;
    if (row == phrase_tool_sel_) apply_phrase_tool((PhraseTool)row);
    else phrase_tool_sel_ = row;
    return true;
}

void App::update_phrase(const InputState& in) {
    static constexpr int N_COLS = 9;  // note inst vel fx1c fx1v fx2c fx2v fx3c fx3v
    const bool nav_up = in.up;
    const bool nav_down = in.down;
    const bool nav_left = in.left;
    const bool nav_right = in.right;

    // R+X opens phrase transforms. If a block selection is active its current
    // range is captured; otherwise tools operate on the playable phrase length.
    // The popup owns input until apply/cancel, so X can never fall through to CUT.
    if (phrase_tools_on_) {
        if (in.up || in.analog_y > 0 || in.encoder_delta > 0)
            phrase_tool_sel_ = (phrase_tool_sel_ - 1 + PT_COUNT) % PT_COUNT;
        if (in.down || in.analog_y < 0 || in.encoder_delta < 0)
            phrase_tool_sel_ = (phrase_tool_sel_ + 1) % PT_COUNT;
        if (in.a) apply_phrase_tool((PhraseTool)phrase_tool_sel_);
        if (in.b || (in.held_r && in.x)) phrase_tools_on_ = false;
        return;
    }
    if (in.held_r && in.x) {
        open_phrase_tools();
        return;
    }

    // === selection mode (m8-style): ZL+SELECT toggles, cursor extends the range ===
    if (in.held_zl && in.select_) {
        sel_mode_ = !sel_mode_;
        if (sel_mode_) sel_anchor_ = cursor_row_;
        return;
    }
    if (sel_mode_) {
        if (nav_up)   cursor_row_ = (cursor_row_ - 1 + seq::PHRASE_STEPS) % seq::PHRASE_STEPS;
        if (nav_down) cursor_row_ = (cursor_row_ + 1) % seq::PHRASE_STEPS;
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

    // === SELECT on the INSTRUMENT column: drill into the instrument editor ===
    // completes the m8 navigation chain song > chain > phrase > instrument.
    // resolves the same way the engine (and the side panel) does: the step's own
    // instrument, else the last one declared above it, else the edit instrument -
    // so it opens what this row actually PLAYS, even on an empty inst cell.
    if (in.select_ && !in.held_zl && !in.held_l && !in.held_r && cursor_col_ == 1) {
        cur_inst_ = step_instrument_at(cursor_row_);
        screen_ = Screen::Instrument;
        return;
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
        if (nav_up)   fx_help_sel_ = (fx_help_sel_ - 1 + N_FX_CMDS) % N_FX_CMDS;
        if (nav_down) fx_help_sel_ = (fx_help_sel_ + 1) % N_FX_CMDS;
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
        if (nav_left)  cursor_col_ = (cursor_col_ - 1 + N_COLS) % N_COLS;
        if (nav_right) cursor_col_ = (cursor_col_ + 1) % N_COLS;
        if (cursor_col_ != 3 && cursor_col_ != 5 && cursor_col_ != 7) fx_help_ = false;
        return;
    }

    if (nav_up)    cursor_row_ = (cursor_row_ - 1 + seq::PHRASE_STEPS) % seq::PHRASE_STEPS;
    if (nav_down)  cursor_row_ = (cursor_row_ + 1) % seq::PHRASE_STEPS;
    if (nav_left)  cursor_col_ = (cursor_col_ - 1 + N_COLS) % N_COLS;
    if (nav_right) cursor_col_ = (cursor_col_ + 1) % N_COLS;

    // === quick erase / phrase tools (held R) ===
    //   R + B - clear the WHOLE step (note/inst/vel/all fx)
    //   R + A - clear only the cell under the cursor
    //   R + Y - clear the ENTIRE phrase (one undo record per step)
    //   R + X - open PHRASE TOOLS (caught above, before selection/CUT handling)
    if (in.held_r) {
        if (in.b) { snapshot_step(cursor_row_); clear_step(cursor_row_); commit_step(cursor_row_); dirty = true; }
        if (in.a) { snapshot_step(cursor_row_); clear_cell(cursor_row_, cursor_col_); commit_step(cursor_row_); dirty = true; }
        if (in.y) {
            for (int r = 0; r < seq::PHRASE_STEPS; ++r) {
                snapshot_step(r); clear_step(r); commit_step(r);
            }
            edit_flash_frame_ = frame_;
            dirty = true;
        }
        return;  // under held_r don't edit values
    }

    // editing (each wrapped in snapshot/commit so undo captures the before/after)
    if (in.a) { snapshot_step(cursor_row_); edit_value(+1);  commit_step(cursor_row_); }
    if (in.b) { snapshot_step(cursor_row_); edit_value(-1);  commit_step(cursor_row_); }
    if (in.x) { snapshot_step(cursor_row_); edit_value(+12); commit_step(cursor_row_); }  // big step
    if (in.y) { snapshot_step(cursor_row_); edit_value(-12); commit_step(cursor_row_); }
    if (in.encoder_delta) {
        snapshot_step(cursor_row_);
        edit_value(in.encoder_delta);
        commit_step(cursor_row_);
    }
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

// snapshot/commit for the CURRENT instrument. same before/after discipline as
// steps, but the payload is the whole Instrument - the only way to make preset
// loads and type switches reversible.
void App::snapshot_inst() {
    inst_undo_.snapshot(project_, cur_inst_);
}

void App::commit_inst() {
    inst_undo_.commit(project_, cur_inst_, frame_, 30);
}

void App::do_undo() {
    // in the instrument view ZL+B walks the INSTRUMENT history - the user is
    // looking at instrument params, so that is what they expect to come back.
    if (screen_ == Screen::Instrument) {
        uint16_t id;
        if (inst_undo_.undo(project_, id)) {
            cur_inst_ = (uint8_t)id;
            dirty = true;
            edit_flash_frame_ = frame_;
            push_live_inst_params(cur_inst_);
            return;
        }
        // nothing in the instrument history - fall through to the cell history
    }
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
    if (screen_ == Screen::Instrument) {
        uint16_t id;
        if (inst_undo_.redo(project_, id)) {
            cur_inst_ = (uint8_t)id;
            dirty = true;
            edit_flash_frame_ = frame_;
            push_live_inst_params(cur_inst_);
            return;
        }
    }
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
    const bool nav_up = in.up;
    const bool nav_down = in.down;
    const bool nav_left = in.left;
    const bool nav_right = in.right;
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

    if (nav_up)    chain_row_ = (chain_row_ - 1 + seq::CHAIN_ROWS) % seq::CHAIN_ROWS;
    if (nav_down)  chain_row_ = (chain_row_ + 1) % seq::CHAIN_ROWS;
    if (nav_left)  chain_col_ = (chain_col_ - 1 + 2) % 2;
    if (nav_right) chain_col_ = (chain_col_ + 1) % 2;

    auto& cell = project_.chains[cur_chain_].rows[chain_row_];
    int delta = 0;
    if (in.a) delta = +1;
    if (in.b) delta = -1;
    if (in.encoder_delta) delta = in.encoder_delta;
    if (delta) {
        edit_flash_frame_ = frame_;
        mark_dirty();                 // same omission as the song view had
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
    const bool nav_up = in.up || in.analog_y > 0;
    const bool nav_down = in.down || in.analog_y < 0;
    const bool nav_left = in.left || in.analog_x < 0;
    const bool nav_right = in.right || in.analog_x > 0;
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
        if (nav_left)  song_row_ = (song_row_ - 1 + seq::SONG_ROWS) % seq::SONG_ROWS;
        if (nav_right) song_row_ = (song_row_ + 1) % seq::SONG_ROWS;
        if (nav_up)    song_col_ = (song_col_ - 1 + seq::NUM_TRACKS) % seq::NUM_TRACKS;
        if (nav_down)  song_col_ = (song_col_ + 1) % seq::NUM_TRACKS;
    } else {
        if (nav_up)    song_row_ = (song_row_ - 1 + seq::SONG_ROWS) % seq::SONG_ROWS;
        if (nav_down)  song_row_ = (song_row_ + 1) % seq::SONG_ROWS;
        if (nav_left)  song_col_ = (song_col_ - 1 + seq::NUM_TRACKS) % seq::NUM_TRACKS;
        if (nav_right) song_col_ = (song_col_ + 1) % seq::NUM_TRACKS;
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
    if (in.encoder_delta) delta = in.encoder_delta;
    if (delta) {
        edit_flash_frame_ = frame_;   // cell-edit feedback, same as the phrase view
        mark_dirty();                 // song cell edits were NOT marking the project
                                      // dirty at all - autosave/`*` silently missed them
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

// resolve which instrument a phrase step actually plays. the instrument column
// is STICKY in the engine: an empty cell keeps whatever was declared above it.
// the panel, the SELECT drill-down and anything else asking "what is this row?"
// must agree, so the rule lives here once.
uint8_t App::step_instrument_at(int row, bool* inherited) const {
    const auto& ph = project_.phrases[cur_phrase_];
    if (row >= 0 && row < seq::PHRASE_STEPS && ph.steps[row].instrument != seq::EMPTY) {
        if (inherited) *inherited = false;
        return ph.steps[row].instrument;
    }
    if (inherited) *inherited = true;
    for (int r = row - 1; r >= 0; --r)
        if (ph.steps[r].instrument != seq::EMPTY) return ph.steps[r].instrument;
    return cur_inst_;
}

// === PHRASE SIDE PANEL (right third of the top screen) ===
// the 3DS top screen is 400px - 80 wider than an M8's entire display. the grid
// only needs 250, so the leftover third becomes a PERMANENT inspector instead
// of the old 14px hint strip at the bottom.
//
// the rule for what earns a place here: it must be something you CANNOT see in
// the grid but that decides how the step sounds. hence - the resolved
// instrument (the column is sticky), its source (sample/wave/algo), its
// envelope, its always-on FX defaults (applied before every trigger, invisible
// in the phrase), and the step's own FX decoded into words.
//
// px = left edge of the panel (grid width).
void App::draw_phrase_panel(Draw& d, int px) {
    const int PX = px + 2, PW = 400 - PX, PY = 16;
    d.rect(PX, PY, PW, 240 - PY, pal::PANEL);
    d.rect(px, PY, 1, 240 - PY, pal::GRID);

    bool inherited = false;
    const auto& step = project_.phrases[cur_phrase_].steps[cursor_row_];
    const uint8_t inst_id = step_instrument_at(cursor_row_, &inherited);
    const auto& inst = project_.instruments[inst_id];
    static const char* kType[] = { "NONE", "WAV", "SMP", "KIT", "FM", "DSN" };
    const int ti = (int)inst.type < seq::INSTRUMENT_TYPE_COUNT ? (int)inst.type : 0;
    const int trk_id = (song_col_ >= 0 && song_col_ < audio::NUM_TRACKS) ? song_col_ : 0;
    const auto& trk = mixer_.track(trk_id);
    char b[48];

    auto rule = [&](int y) { d.rect(PX + 4, y, PW - 9, 1, pal::GRID); };
    auto pct = [](int32_t v) -> int {
        if (v < 0) v = 0;
        if (v > fx::Q15_ONE) v = fx::Q15_ONE;
        return (int)((int64_t)v * 99 / fx::Q15_ONE);
    };
    auto signed_pct = [](int32_t v) -> int {
        if (v < -fx::Q15_ONE) v = -fx::Q15_ONE;
        if (v >  fx::Q15_ONE) v =  fx::Q15_ONE;
        return (int)((int64_t)v * 99 / fx::Q15_ONE);
    };
    auto time_text = [](uint32_t frames, char out[12]) {
        uint32_t ms = frames / 32;
        if (ms < 1000) std::snprintf(out, 12, "%ums", (unsigned)ms);
        else std::snprintf(out, 12, "%u.%us", (unsigned)(ms / 1000),
                           (unsigned)((ms % 1000) / 100));
    };

    // identity: resolved sticky instrument, name, and sound source.
    std::snprintf(b, sizeof(b), "I%02X %s %s", inst_id, kType[ti],
                  inst.poly ? "POLY" : "MONO");
    d.text(PX + 4, 19, b, inherited ? pal::FG_DIM : pal::HEADER);
    std::snprintf(b, sizeof(b), "%.22s", inst.name[0] ? inst.name : "(unnamed)");
    d.text(PX + 4, 29, b, pal::FG);
    b[0] = 0;
    switch (inst.type) {
        case seq::InstrumentType::Sampler: {
            const auto& sm = synth::SampleBank::instance().slot(inst.sampler.sample_slot);
            std::snprintf(b, sizeof(b), "%02d %.17s", inst.sampler.sample_slot,
                          sm.name[0] ? sm.name : "EMPTY");
            break;
        }
        case seq::InstrumentType::Wavsynth: {
            static const char* kW[] = {"SINE","SAW","SQUARE","TRI","NOISE","USER"};
            int wi = (int)inst.wavsynth.shape;
            std::snprintf(b, sizeof(b), "%s  UNI %d", wi >= 0 && wi < 6 ? kW[wi] : "?",
                          inst.wavsynth.unison);
            break;
        }
        case seq::InstrumentType::FmSynth:
            std::snprintf(b, sizeof(b), "ALGO %d %s", inst.fm.algorithm,
                          synth::fm_algo_name(inst.fm.algorithm));
            break;
        case seq::InstrumentType::DsnSynth:
            std::snprintf(b, sizeof(b), "%s+%s%s",
                          synth::dsn_wave_name(inst.dsn.vco1_wave),
                          synth::dsn_wave_name(inst.dsn.vco2_wave),
                          inst.dsn.vco2_sync ? " SYNC" : "");
            break;
        case seq::InstrumentType::DrumKit:
            std::snprintf(b, sizeof(b), "16 PAD KIT");
            break;
        default: break;
    }
    d.text(PX + 4, 39, b[0] ? b : "NO SOURCE", pal::FG_DIM);
    if (inst.table_id != seq::EMPTY) {
        std::snprintf(b, sizeof(b), "T%02X", inst.table_id);
        d.text(PX + PW - 22, 39, b, pal::CURSOR);
    }
    rule(49);

    // primary audible envelope, with honest millisecond values.
    uint32_t a = 0, dc = 0, r = 0; int32_t s = 0;
    int env_idx = 0; bool has_env = true;
    switch (inst.type) {
        case seq::InstrumentType::Wavsynth:
            a = inst.wavsynth.attack; dc = inst.wavsynth.decay;
            s = inst.wavsynth.sustain; r = inst.wavsynth.release; break;
        case seq::InstrumentType::Sampler:
            a = inst.sampler.attack; dc = inst.sampler.decay;
            s = inst.sampler.sustain; r = inst.sampler.release; break;
        case seq::InstrumentType::DsnSynth:
            a = inst.dsn.eg1_attack; dc = inst.dsn.eg1_decay;
            s = inst.dsn.eg1_sustain; r = inst.dsn.eg1_release; break;
        case seq::InstrumentType::FmSynth: {
            uint8_t cm = synth::fm_algo_carrier_mask(inst.fm.algorithm);
            int best = 0, best_lvl = -1;
            for (int i = 0; i < synth::FM_NUM_OPS; ++i)
                if ((cm >> i) & 1 && inst.fm.ops[i].level > best_lvl) {
                    best = i; best_lvl = inst.fm.ops[i].level;
                }
            const auto& op = inst.fm.ops[best];
            a = op.attack; dc = op.decay; r = op.release;
            s = (int32_t)op.sustain * fx::Q15_ONE / 127; env_idx = best;
            std::snprintf(b, sizeof(b), "OP%d", best + 1);
            d.text(PX + PW - 22, 52, b, pal::FG_DIM);
            break;
        }
        default: has_env = false; break;
    }
    d.text(PX + 4, 52, "ENV", pal::HEADER);
    const int EX = PX + 4, EY = 61, EW = PW - 10, EH = 25;
    d.rect(EX, EY, EW, EH, with_alpha(pal::BG_HI, 0x80));
    if (has_env) {
        env_curve(d, EX, EY, EW, EH, a, dc, s, r, -1, pal::CURSOR, pal::PLAY);
        bool done = false;
        for (int t = 0; t < audio::NUM_TRACKS && !done; ++t)
            for (int k = 0; k < audio::TRACK_POLY; ++k) {
                auto* v = mixer_.track(t).voices[k];
                if (!v || !v->active() || v->inst_id != inst_id) continue;
                int st = v->ui_env_stage(env_idx); if (st <= 0) continue;
                env_live_dot(d, EX, EY, EW, EH, a, dc, s, r, st,
                             v->ui_env_level(env_idx), breathe_pulse(frame_, 24));
                done = true; break;
            }
    } else {
        d.text(EX + 4, EY + 8,
               inst.type == seq::InstrumentType::DrumKit ? "ONE SHOT" : "NO ENVELOPE",
               pal::FG_DIM);
    }
    static const char* env_l[] = {"A", "D", "S", "R"};
    char tv[4][12];
    time_text(a, tv[0]); time_text(dc, tv[1]);
    std::snprintf(tv[2], 12, "%d%%", pct(s)); time_text(r, tv[3]);
    for (int i = 0; i < 4; ++i) {
        int x = EX + i * EW / 4;
        d.text(x, 89, env_l[i], pal::HEADER);
        d.text(x, 98, has_env ? tv[i] : "--", pal::FG_HEX);
    }
    rule(108);

    // Modulation monitor. Values and phase come from the running DSP, while
    // rate is translated back to the actual 0.1..20 Hz range used by Mg.
    static const char* kMW[] = {"TRI", "SAW", "SQR", "S&H"};
    auto hz10 = [](int32_t rate) -> int {
        if (rate < 0) rate = 0;
        if (rate > fx::Q15_ONE) rate = fx::Q15_ONE;
        return 1 + (int)((int64_t)rate * 199 / fx::Q15_ONE);
    };
    auto wave_value = [](uint8_t wave, uint32_t phase, int16_t held) -> int32_t {
        const uint32_t p = phase >> 16;
        switch (wave & 3) {
            case 0: return p < 32768 ? (int32_t)p * 2 - 32768
                                     : 32767 - ((int32_t)p - 32768) * 2;
            case 1: return (int32_t)p - 32768;
            case 2: return p < 32768 ? 32767 : -32767;
            default: return held;
        }
    };
    auto draw_wave = [&](int x, int y, int w, int h, uint8_t wave,
                         uint32_t phase, int32_t live, bool dot) {
        d.rect(x, y, w, h, with_alpha(pal::BG_HI, 0x80));
        int prev = y + h / 2;
        for (int ix = 0; ix < w; ++ix) {
            uint32_t ph = (uint32_t)((uint64_t)ix * 0xffffffffu / (w - 1));
            // S&H's preview is deterministic; its dot still uses the real hold.
            int16_t sh = (int16_t)((((ix * 5 + 3) & 7) - 4) * 7000);
            int32_t v = wave_value(wave, ph, sh);
            int yy = y + h / 2 - v * (h / 2 - 1) / 32768;
            int ya = yy < prev ? yy : prev;
            int yh = yy < prev ? prev - yy + 1 : yy - prev + 1;
            d.rect(x + ix, ya, 1, yh, pal::PLAY);
            prev = yy;
        }
        if (dot) {
            int dx = x + (int)((uint64_t)(phase >> 16) * (w - 1) / 65535);
            int dy = y + h / 2 - live * (h / 2 - 1) / 32768;
            d.rect(dx - 1, dy - 1, 3, 3, pal::CURSOR);
        }
    };
    auto route = [&](char* out, size_t n, const char* a, int32_t av,
                     const char* c, int32_t cv) {
        if (!av && !cv) std::snprintf(out, n, "OFF");
        else if (av && cv) std::snprintf(out, n, "%s%+d %s%+d", a, signed_pct(av), c, signed_pct(cv));
        else if (av) std::snprintf(out, n, "%s%+d", a, signed_pct(av));
        else std::snprintf(out, n, "%s%+d", c, signed_pct(cv));
    };

    if (inst.type == seq::InstrumentType::DsnSynth) {
        uint32_t phase[2] = {0, 0};
        int32_t value[2] = {0, 0};
        bool live = false;
        for (int t = 0; t < audio::NUM_TRACKS && !live; ++t)
            for (int k = 0; k < audio::TRACK_POLY; ++k) {
                auto* v = mixer_.track(t).voices[k];
                if (!v || !v->active() || v->inst_id != inst_id) continue;
                for (int m = 0; m < 2; ++m) {
                    phase[m] = v->ui_mg_phase(m);
                    value[m] = v->ui_mg_value(m);
                }
                live = true;
                break;
            }
        const uint8_t waves[2] = {inst.dsn.mg1_wave, inst.dsn.mg2_wave};
        const int32_t rates[2] = {inst.dsn.mg1_rate, inst.dsn.mg2_rate};
        const int32_t r1[2] = {inst.dsn.mg1_to_pitch, inst.dsn.mg2_to_pw};
        const int32_t r2[2] = {inst.dsn.mg1_to_cutoff, inst.dsn.mg2_to_vca};
        const char* n1[2] = {"P", "PW"};
        const char* n2[2] = {"C", "V"};
        for (int m = 0; m < 2; ++m) {
            int y = 111 + m * 19;
            int h10 = hz10(rates[m]);
            std::snprintf(b, sizeof(b), "MG%d %s %d.%dHz", m + 1,
                          kMW[waves[m] & 3], h10 / 10, h10 % 10);
            d.text(PX + 4, y, b, pal::HEADER);
            draw_wave(PX + 4, y + 9, 40, 8, waves[m], phase[m], value[m], live);
            route(b, sizeof(b), n1[m], r1[m], n2[m], r2[m]);
            d.text(PX + 49, y + 9, b, b[0] == 'O' ? pal::FG_DIM : pal::FG);
        }
    } else {
        const uint8_t wave = trk.mg_wave & 3;
        const int h10 = hz10(trk.mg_rate);
        std::snprintf(b, sizeof(b), "MG %s %d.%dHz", kMW[wave], h10 / 10, h10 % 10);
        d.text(PX + 4, 111, b, pal::HEADER);
        const int32_t live = wave_value(wave, trk.mg.phase, trk.mg.sh_value);
        draw_wave(PX + 4, 121, 56, 23, wave, trk.mg.phase, live, true);
        route(b, sizeof(b), "C", trk.mg_to_cutoff, "V", trk.mg_to_vca);
        d.text(PX + 65, 123, b, b[0] == 'O' ? pal::FG_DIM : pal::FG);
        d.text(PX + 65, 134, "CUT / VCA", pal::FG_DIM);
    }
    rule(149);

    // The selected row's three actual commands are more useful here than
    // duplicate CUT/RES/PAN knobs: the grid gives raw cells, this decodes them.
    d.text(PX + 4, 152, "STEP FX", pal::HEADER);
    std::snprintf(b, sizeof(b), "VEL %02X", step.velocity);
    d.text(PX + PW - 40, 152, b, pal::FG_DIM);
    for (int slot = 0; slot < 3; ++slot) {
        const int y = 162 + slot * 12;
        const uint8_t cmd = step.fx[slot].cmd;
        const bool sel = cursor_col_ >= 3 && cursor_col_ <= 8 &&
                         (cursor_col_ - 3) / 2 == slot;
        if (sel) {
            uint8_t br = breathe_pulse(frame_, 48);
            d.rect(PX + 2, y - 2, PW - 6, 11,
                   with_alpha(pal::CURSOR, (uint8_t)(30 + br / 8)));
            d.rect(PX + 2, y - 2, 2, 11, pal::CURSOR);
        }
        std::snprintf(b, sizeof(b), "%d", slot + 1);
        d.text(PX + 5, y, b, sel ? pal::FG : pal::FG_DIM);
        if (!cmd) {
            d.text(PX + 16, y, "---", pal::FG_DIM);
            d.text(PX + 58, y, "EMPTY", pal::FG_DIM);
            continue;
        }
        d.text(PX + 16, y, seq::fx_name_short(cmd), pal::HEADER);
        d.hex2(PX + 40, y, step.fx[slot].value, pal::FG);
        const int max_name = (PW - 64) / 6;
        std::snprintf(b, sizeof(b), "%.*s", max_name, seq::fx_name_long(cmd));
        d.text(PX + 58, y, b, pal::FG_DIM);
    }
    rule(198);

    // actual master stereo peaks plus a live master waveform. Nothing decorative:
    // both read the same post-mix buffers as the full-screen scope.
    d.text(PX + 4, 201, "MASTER", pal::HEADER);
    int32_t pl = 0, pr = 0;
    for (int i = 0; i < 128; ++i) {
        std::size_t si = (mixer_.scope_write_pos + audio::Mixer::SCOPE_SIZE - 1 - i) % audio::Mixer::SCOPE_SIZE;
        int32_t l = mixer_.scope_l[si]; if (l < 0) l = -l;
        int32_t rr = mixer_.scope_r[si]; if (rr < 0) rr = -rr;
        if (l > pl) pl = l;
        if (rr > pr) pr = rr;
    }
    const int BW = 54;
    d.text(PX + 4, 212, "L", pal::FG_DIM); d.rect(PX + 11, 213, BW, 5, pal::BG_HI);
    d.text(PX + 4, 222, "R", pal::FG_DIM); d.rect(PX + 11, 223, BW, 5, pal::BG_HI);
    d.rect(PX + 11, 213, (int)(pl * BW / 32767), 5, pl > 29490 ? pal::RECORD : pal::PLAY);
    d.rect(PX + 11, 223, (int)(pr * BW / 32767), 5, pr > 29490 ? pal::RECORD : pal::PLAY);
    const int SX = PX + 70, SY = 207, SW = PW - 76, SH = 29, SM = SY + SH / 2;
    d.rect(SX, SY, SW, SH, with_alpha(pal::BG_HI, 0x80));
    int py = SM;
    for (int x = 0; x < SW; ++x) {
        std::size_t si = (mixer_.scope_write_pos + (std::size_t)(x * audio::Mixer::SCOPE_SIZE / SW)) % audio::Mixer::SCOPE_SIZE;
        int yy = SM - (int32_t)mixer_.scope[si] * (SH / 2 - 1) / 32768;
        int ya = yy < py ? yy : py, yh = yy < py ? py - yy + 1 : yy - py + 1;
        d.rect(SX + x, ya, 1, yh, pal::PLAY); py = yy;
    }
}

void App::draw_phrase(Draw& d) {
    // layout: header (Y=20), 16 rows x columns
    // compact: row#  NOTE INST VEL  FX1 FX2 FX3
    // the grid now lives in the LEFT 250px; the right third is a permanent
    // inspector panel (instrument identity + envelope + decoded FX). every
    // full-width element below is clipped to GRID_W so nothing bleeds under it.
    constexpr int Y0 = 22;
    constexpr int ROW_H = 12;   // 16 rows * 12 = 192px (34..226), leaves the FX hint bar
                                // at y=228 a clean strip below the grid (no more step-16 overlap)
    constexpr int GRID_W = 250;  // playfield width; panel owns 252..400
    constexpr int COL_X[] = {
        6,   // row#
        28,  // note
        62,  // inst
        90,  // vel
        122, // fx1 cmd+val
        166, // fx2
        210  // fx3
    };
    // Subtle full-height rules make the dense tracker cells read as columns,
    // matching the generated layout without spending pixels on decoration.
    // Each rule sits in the dead space between two editable cells.
    constexpr int COL_SEP[] = { 22, 55, 84, 115, 159, 203 };

    // header row
    d.text(COL_X[0], Y0, "##", pal::HEADER);
    d.text(COL_X[1], Y0, "NOT", pal::HEADER);
    d.text(COL_X[2], Y0, "IN", pal::HEADER);
    d.text(COL_X[3], Y0, "VL", pal::HEADER);
    d.text(COL_X[4], Y0, "FX1", pal::HEADER);
    d.text(COL_X[5], Y0, "FX2", pal::HEADER);
    d.text(COL_X[6], Y0, "FX3", pal::HEADER);

    // playhead: ANY track can be playing the phrase we're looking at - bass on T0,
    // hats on T3 is the norm. scanning only track 0 meant the cursor row you were
    // editing showed no playhead at all whenever the phrase lived on another track.
    // preference order: the track the cursor is "on" (song_col_) first, so a phrase
    // used by two tracks follows the one you're working on; then the lowest track.
    int playing_step = -1;
    int playing_track = -1;
    if (player_.playing()) {
        auto match = [&](int t) {
            const auto& ts = player_.track_state(t);
            return ts.playing && ts.play_phrase_id == cur_phrase_;
        };
        if (song_col_ >= 0 && song_col_ < seq::NUM_TRACKS && match(song_col_)) {
            playing_track = song_col_;
        } else {
            for (int t = 0; t < seq::NUM_TRACKS; ++t)
                if (match(t)) { playing_track = t; break; }
        }
        if (playing_track >= 0)
            playing_step = player_.track_state(playing_track).play_step;
    }

    // which track the playhead follows (a phrase can be shared between tracks) -
    // so the moving bar is never ambiguous. tiny play triangle + track number.
    // sits at the right edge of the GRID now (the old x=300 is panel territory).
    if (playing_track >= 0) {
        for (int i = 0; i < 4; ++i) d.rect(GRID_W - 24 + i, Y0 + i, 1, 7 - i * 2, pal::PLAY);
        char tb[6];
        std::snprintf(tb, sizeof(tb), "T%d", playing_track);
        d.text(GRID_W - 18, Y0, tb, pal::PLAY);
    }

    for (int row = 0; row < seq::PHRASE_STEPS; ++row) {
        int y = Y0 + 12 + row * ROW_H;
        const auto& step = project_.phrases[cur_phrase_].steps[row];

        // zebra
        if (row & 1) d.rect(0, y - 1, GRID_W, ROW_H, pal::BG_HI);

        // playhead trail: current step bright + 2 previous fading out (Forza-style feedback)
        if (playing_step >= 0) {
            // how many steps back from the playing one (accounting for wrap)
            int back = (playing_step - row + seq::PHRASE_STEPS) % seq::PHRASE_STEPS;
            if (back == 0) {
                // steady backdrop + a gradient that flows across the row on each
                // step (breathes with the beat instead of blinking)
                d.rect(0, y - 1, GRID_W, ROW_H, with_alpha(pal::PLAYHEAD_BG, 0xC0));
                beat_glow(d, 0, y - 1, GRID_W, ROW_H, frame_ - step_change_frame_, pal::PLAYHEAD);
                // bright markers at the edges of the current step
                d.rect(0, y - 1, 3, ROW_H, pal::PLAYHEAD);
                d.rect(GRID_W - 3, y - 1, 3, ROW_H, pal::PLAYHEAD);
            } else if (back <= 2) {
                // trail tail: the farther back - the more transparent
                uint8_t a = (back == 1) ? 0x30 : 0x18;
                d.rect(0, y - 1, GRID_W, ROW_H, with_alpha(pal::PLAYHEAD_BG, a));
            }
        }

        // selection range highlight (m8-style block selection)
        if (sel_mode_) {
            int lo = sel_anchor_ < cursor_row_ ? sel_anchor_ : cursor_row_;
            int hi = sel_anchor_ < cursor_row_ ? cursor_row_ : sel_anchor_;
            if (row >= lo && row <= hi) {
                uint8_t br = breathe_pulse(frame_, 48);
                d.rect(0, y - 1, GRID_W, ROW_H, with_alpha(pal::CURSOR, (uint8_t)(0x28 + br / 8)));
                d.rect(0, y - 1, 2, ROW_H, pal::CURSOR);
                d.rect(GRID_W - 2, y - 1, 2, ROW_H, pal::CURSOR);
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
                d.rect(0, y - 1, GRID_W, ROW_H, with_alpha(pal::PANEL, 0xA0));
                if (row == plen) d.rect(0, y - 1, GRID_W, 1, pal::CURSOR);  // end-of-phrase line
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

    // Dotted column rules, like the reference: one quiet pixel followed by a
    // two-pixel gap. Draw last so zebra/playhead fills cannot erase the dots.
    constexpr int GRID_TOP = Y0 - 2;
    constexpr int GRID_BOTTOM = Y0 + 12 + seq::PHRASE_STEPS * ROW_H;
    for (int x : COL_SEP)
        for (int y = GRID_TOP; y < GRID_BOTTOM; y += 3)
            d.rect(x, y, 1, 1, pal::GRID);

    // === permanent inspector panel (right third) ===
    draw_phrase_panel(d, GRID_W);
}
void App::draw_chain(Draw& d) {
    constexpr int Y0 = 22;
    constexpr int ROW_H = 13;

    d.text(40, Y0, "##", pal::HEADER);
    d.text(80, Y0, "PHR", pal::HEADER);
    d.text(140, Y0, "TSP", pal::HEADER);

    // which chain row is currently playing? same rule as the phrase view: prefer
    // the track the cursor is on, then the lowest one. play_chain_* is the row that
    // is SOUNDING (chain_row already points at the next row after the last step).
    int playing_row = -1;
    int playing_track = -1;
    if (player_.playing()) {
        auto match = [&](int t) {
            const auto& ts = player_.track_state(t);
            return ts.playing && ts.play_chain_id != seq::EMPTY &&
                   ts.play_chain_id == cur_chain_;
        };
        if (song_col_ >= 0 && song_col_ < seq::NUM_TRACKS && match(song_col_)) {
            playing_track = song_col_;
        } else {
            for (int t = 0; t < seq::NUM_TRACKS; ++t)
                if (match(t)) { playing_track = t; break; }
        }
        if (playing_track >= 0)
            playing_row = player_.track_state(playing_track).play_chain_row;
    }
    if (playing_track >= 0) {
        for (int i = 0; i < 4; ++i) d.rect(300 + i, Y0 + i, 1, 7 - i * 2, pal::PLAY);
        char tb[6];
        std::snprintf(tb, sizeof(tb), "T%d", playing_track);
        d.text(306, Y0, tb, pal::PLAY);
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
    // per-track silence state, needed in three places below (header tag, column
    // wash, chain digits). muted = persisted project mask; solo-suppressed = the
    // momentary stage tool. both are INAUDIBLE, so both must be visible HERE -
    // the arrangement is what you stare at, the mixer is a place you visit.
    bool silent[seq::NUM_TRACKS];
    bool by_solo[seq::NUM_TRACKS];
    for (int t = 0; t < seq::NUM_TRACKS; ++t) {
        bool m = (project_.track_mute & (1u << t)) != 0;
        bool s = (solo_track_ >= 0 && t != solo_track_);
        silent[t]  = m || s;
        by_solo[t] = s && !m;
    }

    for (int t = 0; t < seq::NUM_TRACKS; ++t) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "T%d", t);
        d.text(40 + t * 42, Y0, buf, silent[t] ? pal::RECORD : pal::HEADER);
        // mute/solo tag on the free 6px at the right of the track column. M = muted,
        // S = the soloed track, s = silenced *because* someone else is soloed.
        // (queue badge lives at +14, cutoff bar above it - this slot stays clear.)
        if (silent[t]) {
            d.text(40 + t * 42 + 33, Y0 + 5, by_solo[t] ? "s" : "M",
                   by_solo[t] ? pal::FG_DIM : pal::RECORD);
        } else if (solo_track_ == t) {
            uint8_t br = breathe_pulse(frame_, 48);
            d.text(40 + t * 42 + 33, Y0 + 5, "S", lerp_color(pal::PLAY, pal::FG, br));
        }
        // mini cutoff/crush indicators alongside
        auto& tr = mixer_.track(t);
        // cutoff: horizontal bar 12px, filled by cutoff
        int cw = (tr.cutoff * 12) / fx::Q15_ONE;
        // cutoff bar color changes with filter_type - a visual indicator with no
        // extra pixels. THEMED (these used to be hardcoded cretaceous hexes, so on
        // vapor/ember the indicators stayed earthy-grey in a violet UI).
        uint32_t fc = pal::HEADER;
        switch (tr.filter_type) {
            case ::trackr::dsp::FilterType::LPF:   fc = pal::HEADER; break;  // label ochre
            case ::trackr::dsp::FilterType::HPF:   fc = pal::CURSOR; break;  // accent
            case ::trackr::dsp::FilterType::BPF:   fc = pal::FG_HEX; break;  // value tint
            case ::trackr::dsp::FilterType::Notch: fc = pal::TRACK3; break;  // 4th track hue
            case ::trackr::dsp::FilterType::Off:   fc = pal::GRID;   break;  // structure grey
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

    // where the arrangement ends: the player loops at song_length(), so everything
    // below it is unreachable until you put a chain there. 256 identical empty rows
    // gave zero hint of that - you scroll into the void wondering if it still plays.
    const int song_len = player_.song_length();
    int cursor_y = -1;

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

        // dead zone past the end of the song (same language as the phrase view's
        // out-of-length shading, lighter: these rows are still editable and writing
        // a chain here extends the song).
        if (song_len > 0 && row >= song_len)
            d.rect(0, y - 1, 400, ROW_H, with_alpha(pal::PANEL, 0x70));

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
                    d.rect(cx - 4, y - 1, 30, ROW_H, with_alpha(pal::PLAYHEAD_BG, a));
                }
            }
            if (!tps.playing || !tps.song_mode_ || (int)tps.song_row != row) continue;
            // cell backdrop + a small gradient flowing through it on each beat
            d.rect(cx - 4, y - 1, 30, ROW_H, with_alpha(pal::PLAYHEAD_BG, 0xC0));
            beat_glow(d, cx - 4, y - 1, 30, ROW_H, frame_ - step_change_frame_, pal::PLAYHEAD, 12);
            // left tick grows as the chain advances (row progress at a glance)
            int th = 2 + (int)tps.play_chain_row * (ROW_H - 2) / (seq::CHAIN_ROWS - 1);
            d.rect(cx - 4, y - 1, 2, th, pal::PLAYHEAD);
        }

        d.hex2(8, y, row & 0xFF, pal::FG_DIM);

        for (int t = 0; t < seq::NUM_TRACKS; ++t) {
            uint8_t c = sr.chain[t];
            // silenced tracks read dim even where they have content - otherwise a
            // muted column looks exactly as alive as a sounding one.
            Color live = silent[t] ? pal::FG_DIM : pal::FG;
            if (c == seq::EMPTY) d.text(40 + t * 42, y, "--", pal::FG_DIM);
            else d.hex2(40 + t * 42, y, c, live);
        }

        // end-of-song line, drawn ON TOP of the cells so it can't be buried
        if (song_len > 0 && row == song_len) {
            d.rect(0, y - 1, 400, 1, pal::CURSOR);
            d.text(346, y, "END", pal::CURSOR);
        }

        if (row == song_row_) cursor_y = y;   // drawn last, above the mute wash
    }

    // muted columns get a wash over the whole grid height: the fastest "why is this
    // silent" answer there is, and it survives scrolling. drawn OVER the playhead -
    // a muted track still advances, it just makes no sound, and that's the point.
    for (int t = 0; t < seq::NUM_TRACKS; ++t) {
        if (!silent[t]) continue;
        d.rect(40 + t * 42 - 5, Y0 + 11, 32, VISIBLE * ROW_H,
               with_alpha(pal::PANEL, by_solo[t] ? 0x50 : 0x70));
    }

    // cursor last: it must stay legible even inside a washed-out muted column
    if (cursor_y >= 0) {
        int cx = 39 + song_col_ * 42;
        // value-flash on edit - the phrase view has had it forever; the song
        // cursor felt dead under the fingers without it.
        uint8_t ft = edit_flash_t();
        if (ft) d.rect(cx, cursor_y - 1, 14, 10, with_alpha(pal::FLASH, ft / 2));
        uint8_t br = breathe_pulse(frame_, 64);
        ui::Color cur = lerp_color(with_alpha(pal::CURSOR, 130), pal::CURSOR, br);
        d.corner_brackets(cx, cursor_y - 1, 14, 10, cur, 3, 1);
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

    // end-of-song boundary: everything right of it never plays (the song loops at
    // song_length()). vertical line + tag, mirror of the list view's END row.
    const int song_len = player_.song_length();
    if (song_len > left_col && song_len < left_col + COLS) {
        int ex = LX + (song_len - left_col) * CELL_W - 2;
        d.rect(ex, LANE_Y0 - 2, 1, seq::NUM_TRACKS * LANE_H + 2, pal::CURSOR);
        d.text(ex + 2, Y0, "END", pal::CURSOR);
    }

    // same silence bookkeeping as the list view (mute mask + momentary solo)
    bool silent[seq::NUM_TRACKS], by_solo[seq::NUM_TRACKS];
    for (int t = 0; t < seq::NUM_TRACKS; ++t) {
        bool m = (project_.track_mute & (1u << t)) != 0;
        bool s = (solo_track_ >= 0 && t != solo_track_);
        silent[t]  = m || s;
        by_solo[t] = s && !m;
    }

    int cur_x = -1, cur_y = -1;

    for (int t = 0; t < seq::NUM_TRACKS; ++t) {
        int ly = LANE_Y0 + t * LANE_H;
        // lane separator
        d.rect(0, ly - 1, 400, 1, with_alpha(pal::GRID, 60));
        // track label + mute tag + live-queue badge
        char tb[4];
        std::snprintf(tb, sizeof(tb), "T%d", t);
        d.text(2, ly + 3, tb, silent[t] ? pal::RECORD
                                        : (t == song_col_ ? pal::CURSOR : pal::HEADER));
        if (silent[t]) {
            d.text(15, ly + 3, by_solo[t] ? "s" : "M",
                   by_solo[t] ? pal::FG_DIM : pal::RECORD);
        } else if (solo_track_ == t) {
            uint8_t br = breathe_pulse(frame_, 48);
            d.text(15, ly + 3, "S", lerp_color(pal::PLAY, pal::FG, br));
        }
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

            // past the end of the song: unreachable until a chain lands there
            if (song_len > 0 && row >= song_len)
                d.rect(x - 2, ly + 1, CELL_W, LANE_H - 2, with_alpha(pal::PANEL, 0x70));

            // ghost trail: the cell the playhead just left fades out
            if (song_ph_prev_[t] != 0xFFFF && (int)song_ph_prev_[t] == row) {
                uint32_t age = frame_ - song_ph_frame_[t];
                if (age < 24) {
                    uint8_t a = (uint8_t)(150 - age * 150 / 24);
                    d.rect(x - 2, ly + 1, CELL_W, LANE_H - 2, with_alpha(pal::PLAYHEAD_BG, a));
                }
            }
            // playhead cell + chain progress bar along the bottom edge
            if (tps.playing && tps.song_mode_ && (int)tps.song_row == row) {
                d.rect(x - 2, ly + 1, CELL_W, LANE_H - 2, with_alpha(pal::PLAYHEAD_BG, 0xC0));
                beat_glow(d, x - 2, ly + 1, CELL_W, LANE_H - 2,
                          frame_ - step_change_frame_, pal::PLAYHEAD, 12);
                int pw = 2 + (int)tps.play_chain_row * (CELL_W - 2) / (seq::CHAIN_ROWS - 1);
                d.rect(x - 2, ly + LANE_H - 3, pw, 2, pal::PLAYHEAD);
            }

            uint8_t ch = project_.song.rows[row].chain[t];
            if (ch == seq::EMPTY) d.text(x, ly + 7, "--", pal::FG_DIM);
            else                  d.hex2(x, ly + 7, ch, silent[t] ? pal::FG_DIM : pal::FG);

            if (row == song_row_ && t == song_col_) { cur_x = x; cur_y = ly; }
        }

        // muted lane wash across the full width, over the playhead
        if (silent[t])
            d.rect(LX - 2, ly + 1, COLS * CELL_W, LANE_H - 2,
                   with_alpha(pal::PANEL, by_solo[t] ? 0x50 : 0x70));
    }

    // cursor last so it stays readable inside a washed-out lane
    if (cur_x >= 0) {
        uint8_t ft = edit_flash_t();
        if (ft) d.rect(cur_x - 2, cur_y + 5, 16, 12, with_alpha(pal::FLASH, ft / 2));
        uint8_t br = breathe_pulse(frame_, 64);
        ui::Color cur = lerp_color(with_alpha(pal::CURSOR, 130), pal::CURSOR, br);
        d.corner_brackets(cur_x - 2, cur_y + 5, 16, 12, cur, 3, 1);
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
