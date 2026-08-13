// App: phrase editor update, FX catalogue, value editing and inspector.
#include "../app.h"
#include "../ui_internal.h"
#include "../../sequencer/scale.h"
#include "../../audio/fixed.h"
#include "../../sequencer/fx.h"
#include "../../synth/fm.h"
#include <cstdio>

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
namespace {
    constexpr int FXH_X = 4, FXH_Y = 92, FXH_W = 312, FXH_H = 146;
    constexpr int FXH_COLS = 2, FXH_ROW_H = 11;
    constexpr int FXH_ROWS_VIS = 12;
    constexpr int FXH_COL_W = FXH_W / FXH_COLS;
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
    if (fx_help_closing_) {
        uint32_t ca = frame_ - overlay_close_frame_;
        uint8_t a = (uint8_t)clamp_int((int)ca * 52, 0, 210);
        d.rect(FXH_X, FXH_Y, FXH_W, FXH_H, with_alpha(pal::BG, a));
        int in = (int)motion_in(ca, 4) * 18 / 255;
        d.corner_brackets(FXH_X + in, FXH_Y + in / 2, FXH_W - in * 2, FXH_H - in,
                          with_alpha(pal::CURSOR, (uint8_t)(220 - a)), 6, 1);
        return;
    }
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
        // Centre the 8px glyphs inside the 11px menu row. `y + 2` made the
        // bottom gap one pixel smaller and the whole list looked like it sagged.
        const int ty = y + (FXH_ROW_H - 8) / 2;
        d.text(x + 4, ty, seq::fx_name_short(cmd), sel ? pal::FG : pal::FG_HEX);
        d.text(x + 30, ty, seq::fx_name_long(cmd), sel ? pal::FG : pal::FG_DIM);
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
        }
        begin_overlay_close(fx_help_closing_);
    } else {
        fx_help_sel_ = i;
    }
    return true;
}

void App::update_phrase(const InputState& in) {
    static constexpr int N_COLS = 9;  // note inst vel fx1c fx1v fx2c fx2v fx3c fx3v
    static constexpr int N_PHRASE_TOOLS = static_cast<int>(PhraseTool::COUNT);
    const bool nav_up = in.up;
    const bool nav_down = in.down;
    const bool nav_left = in.left;
    const bool nav_right = in.right;

    // R+X opens phrase transforms. If a block selection is active its current
    // range is captured; otherwise tools operate on the playable phrase length.
    // The popup owns input until apply/cancel, so X can never fall through to CUT.
    if (phrase_tools_on_) {
        if (phrase_tools_closing_) return;
        const int range_len = phrase_tools_hi_ - phrase_tools_lo_ + 1;
        if (phrase_tool_config_) {
            int delta = 0;
            if (in.left || in.encoder_delta < 0) delta = -1;
            if (in.right || in.encoder_delta > 0) delta = +1;
            if (in.x) delta = +8;
            if (in.y) delta = -8;
            if (delta) phrase_tool_amount_ = clamp_int(
                phrase_tool_amount_ + delta,
                phrase_tool_sel_ == (int)PhraseTool::Euclidean ? 0
                    : phrase_tool_sel_ == (int)PhraseTool::RandomNotes ? 1
                    : phrase_tool_sel_ == (int)PhraseTool::Every ? 2 : 0,
                phrase_tool_sel_ == (int)PhraseTool::Euclidean ? range_len
                    : phrase_tool_sel_ == (int)PhraseTool::Humanize ? 63
                    : phrase_tool_sel_ == (int)PhraseTool::RandomNotes ? 48
                    : phrase_tool_sel_ == (int)PhraseTool::Every ? 15 : 100);
            if (in.up) ++phrase_tool_seed_;
            if (in.down) --phrase_tool_seed_;
            if (in.a) apply_phrase_tool((PhraseTool)phrase_tool_sel_);
            if (in.b || (in.held_r && in.x)) phrase_tool_config_ = false;
            return;
        }
        if (in.up || in.analog_y > 0 || in.encoder_delta > 0)
            phrase_tool_sel_ = wrap_index(phrase_tool_sel_, -1, N_PHRASE_TOOLS);
        if (in.down || in.analog_y < 0 || in.encoder_delta < 0)
            phrase_tool_sel_ = wrap_index(phrase_tool_sel_, +1, N_PHRASE_TOOLS);
        if (in.a) {
            if (phrase_tool_sel_ >= (int)PhraseTool::Euclidean) {
                phrase_tool_config_ = true;
                switch ((PhraseTool)phrase_tool_sel_) {
                    case PhraseTool::Euclidean:   phrase_tool_amount_ = (range_len + 1) / 2; break;
                    case PhraseTool::Density:     phrase_tool_amount_ = 70; break;
                    case PhraseTool::Humanize:    phrase_tool_amount_ = 8; break;
                    case PhraseTool::Ratchet:     phrase_tool_amount_ = 35; break;
                    case PhraseTool::Mutate:      phrase_tool_amount_ = 25; break;
                    case PhraseTool::RandomNotes: phrase_tool_amount_ = 12; break;
                    case PhraseTool::ChanceSpread:phrase_tool_amount_ = 35; break;
                    case PhraseTool::Every:       phrase_tool_amount_ = 4; break;
                    default: break;
                }
            } else apply_phrase_tool((PhraseTool)phrase_tool_sel_);
        }
        if (in.b || (in.held_r && in.x)) {
            begin_overlay_close(phrase_tools_closing_);
            phrase_tool_config_ = false;
        }
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
        if (nav_up)   cursor_row_ = wrap_index(cursor_row_, -1, seq::PHRASE_STEPS);
        if (nav_down) cursor_row_ = wrap_index(cursor_row_, +1, seq::PHRASE_STEPS);
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
            const uint8_t next = (uint8_t)clamp_int(seq::phrase_len(ph) + (in.up ? 1 : -1),
                                                    1, seq::PHRASE_STEPS);
            if (next != ph.length) {
                ph.length = next;
                mark_project_dirty();
                edit_flash_frame_ = frame_;
            }
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
            return;
        }
        if (in.y && has_clip_) {  // paste single step
            snapshot_step(cursor_row_);
            project_.phrases[cur_phrase_].steps[cursor_row_] = clipboard_step_;
            commit_step(cursor_row_);
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
        fx_help_closing_ = false;
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
        if (fx_help_closing_) return;
        // picker owns up/down/A/B while open
        if (nav_up)   fx_help_sel_ = wrap_index(fx_help_sel_, -1, N_FX_CMDS);
        if (nav_down) fx_help_sel_ = wrap_index(fx_help_sel_, +1, N_FX_CMDS);
        if (in.a) {
            int slot = (cursor_col_ - 3) / 2;
            snapshot_step(cursor_row_);
            project_.phrases[cur_phrase_].steps[cursor_row_].fx[slot].cmd = (uint8_t)kFxLetters[fx_help_sel_];
            commit_step(cursor_row_);
            begin_overlay_close(fx_help_closing_);
            return;
        }
        if (in.b) { begin_overlay_close(fx_help_closing_); return; }
        // left/right move the cursor to another fx column and keep the picker open
        if (nav_left)  cursor_col_ = wrap_index(cursor_col_, -1, N_COLS);
        if (nav_right) cursor_col_ = wrap_index(cursor_col_, +1, N_COLS);
        if (cursor_col_ != 3 && cursor_col_ != 5 && cursor_col_ != 7)
            begin_overlay_close(fx_help_closing_);
        return;
    }

    if (nav_up)    cursor_row_ = wrap_index(cursor_row_, -1, seq::PHRASE_STEPS);
    if (nav_down)  cursor_row_ = wrap_index(cursor_row_, +1, seq::PHRASE_STEPS);
    if (nav_left)  cursor_col_ = wrap_index(cursor_col_, -1, N_COLS);
    if (nav_right) cursor_col_ = wrap_index(cursor_col_, +1, N_COLS);

    // === quick erase / phrase tools (held R) ===
    //   R + B - clear the WHOLE step (note/inst/vel/all fx)
    //   R + A - clear only the cell under the cursor
    //   R + Y - clear the ENTIRE phrase (one undo record per step)
    //   R + X - open PHRASE TOOLS (caught above, before selection/CUT handling)
    if (in.held_r) {
        if (in.b) { snapshot_step(cursor_row_); clear_step(cursor_row_); commit_step(cursor_row_); }
        if (in.a) { snapshot_step(cursor_row_); clear_cell(cursor_row_, cursor_col_); commit_step(cursor_row_); }
        if (in.y) {
            for (int r = 0; r < seq::PHRASE_STEPS; ++r) {
                snapshot_step(r); clear_step(r); commit_step(r);
            }
            edit_flash_frame_ = frame_;
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
void App::edit_value(int delta) {
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
            bump_clamped(step.velocity, delta, 0, 127);
            break;
        }
        // fx slots - cmd is a letter, step through valid ones
        case 3: case 5: case 7: {
            int slot = (cursor_col_ - 3) / 2;
            uint8_t cur = step.fx[slot].cmd;
            int idx = -1;
            if (cur == 0) idx = -1;
            else for (int i = 0; i < N_FX_CMDS; ++i) if ((uint8_t)kFxLetters[i] == cur) { idx = i; break; }
            idx = clamp_int(idx + delta, -1, N_FX_CMDS - 1);
            if (idx == -1) step.fx[slot].cmd = 0;
            else step.fx[slot].cmd = (uint8_t)kFxLetters[idx];
            break;
        }
        case 4: case 6: case 8: {
            int slot = (cursor_col_ - 4) / 2;
            int vmax = seq::fx_value_max(step.fx[slot].cmd);
            bump_clamped(step.fx[slot].value, delta, 0, vmax);
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
    d.rect(px, PY, 1, 240 - PY, lerp_color(pal::BG, pal::GRID, 112));

    bool inherited = false;
    const auto& step = project_.phrases[cur_phrase_].steps[cursor_row_];
    const uint8_t inst_id = step_instrument_at(cursor_row_, &inherited);
    const auto& inst = project_.instruments[inst_id];
    static const char* kType[] = { "NONE", "WAV", "SMP", "KIT", "FM", "DSN" };
    const int ti = (int)inst.type < seq::INSTRUMENT_TYPE_COUNT ? (int)inst.type : 0;
    const int trk_id = (song_col_ >= 0 && song_col_ < audio::NUM_TRACKS) ? song_col_ : 0;
    const auto& trk = mixer_.track(trk_id);
    char b[48];

    auto rule = [&](int y) {
        d.rect(PX + 4, y, PW - 9, 1, lerp_color(pal::PANEL, pal::GRID, 112));
    };
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
    // layout: header (Y=20), then 16 rows stretched to the physical bottom.
    // row#  NOTE INST VEL  FX1 FX2 FX3
    // the grid now lives in the LEFT 250px; the right third is a permanent
    // inspector panel (instrument identity + envelope + decoded FX). every
    // full-width element below is clipped to GRID_W so nothing bleeds under it.
    constexpr int Y0 = 22;
    // 16 * 13 = 208px: first row starts at 34, final row reaches y=241
    // (the renderer clips the last pixel). No dead footer under the phrase grid.
    constexpr int ROW_H = 13;
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
        const int row_y = Y0 + 12 + row * ROW_H;
        const int y = row_y + (ROW_H - 8) / 2;  // glyph baseline centered in the row
        const auto& step = project_.phrases[cur_phrase_].steps[row];

        // zebra
        if (row & 1) d.rect(0, row_y, GRID_W, ROW_H, pal::BG_HI);

        // playhead trail: current step bright + 2 previous fading out (Forza-style feedback)
        if (playing_step >= 0) {
            // how many steps back from the playing one (accounting for wrap)
            int back = (playing_step - row + seq::PHRASE_STEPS) % seq::PHRASE_STEPS;
            if (back == 0) {
                // steady backdrop + a gradient that flows across the row on each
                // step (breathes with the beat instead of blinking)
                d.rect(0, row_y, GRID_W, ROW_H, with_alpha(pal::PLAYHEAD_BG, 0xC0));
                beat_glow(d, 0, row_y, GRID_W, ROW_H, frame_ - step_change_frame_, pal::PLAYHEAD);
                // bright markers at the edges of the current step
                d.rect(0, row_y, 3, ROW_H, pal::PLAYHEAD);
                d.rect(GRID_W - 3, row_y, 3, ROW_H, pal::PLAYHEAD);
            } else if (back <= 2) {
                // trail tail: the farther back - the more transparent
                uint8_t a = (back == 1) ? 0x30 : 0x18;
                d.rect(0, row_y, GRID_W, ROW_H, with_alpha(pal::PLAYHEAD_BG, a));
            }
        }

        // selection range highlight (m8-style block selection)
        if (sel_mode_) {
            int lo = sel_anchor_ < cursor_row_ ? sel_anchor_ : cursor_row_;
            int hi = sel_anchor_ < cursor_row_ ? cursor_row_ : sel_anchor_;
            if (row >= lo && row <= hi) {
                uint8_t br = breathe_pulse(frame_, 48);
                d.rect(0, row_y, GRID_W, ROW_H, with_alpha(pal::CURSOR, (uint8_t)(0x28 + br / 8)));
                d.rect(0, row_y, 2, ROW_H, pal::CURSOR);
                d.rect(GRID_W - 2, row_y, 2, ROW_H, pal::CURSOR);
            }
        }

        // event cascade for tools / grouped history. Each row owns only a bit;
        // stagger math and two rectangles are the entire runtime cost.
        if (phrase_motion_mask_ & (uint16_t)(1u << row)) {
            const uint32_t age = frame_ - phrase_motion_frame_;
            const int order = phrase_motion_dir_ > 0 ? row : (seq::PHRASE_STEPS - 1 - row);
            const uint32_t start = (uint32_t)order;
            if (age >= start && age < start + 9) {
                const uint8_t t = motion_linear(age - start, 9);
                const uint8_t a = (uint8_t)(170 - (int)t * 150 / 255);
                const int sweep = GRID_W * (int)motion_out(age - start, 8) / 255;
                d.rect(0, row_y, GRID_W, ROW_H, with_alpha(pal::FLASH, a));
                d.rect(0, row_y, sweep, 2, pal::CURSOR);
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
                d.rect(0, row_y, GRID_W, ROW_H, with_alpha(pal::PANEL, 0xA0));
                if (row == plen) d.rect(0, row_y, GRID_W, 1, pal::CURSOR);  // end-of-phrase line
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
            if (ft) d.rect(cx, row_y + 1, cw, ROW_H - 2, with_alpha(pal::FLASH, ft / 2));
            // breathing corner-bracket selector (M8-style). pulses dusty-rose between
            // a dim and a bright tint so the cursor gently "breathes".
            uint8_t br = breathe_pulse(frame_, 64);
            ui::Color cur = lerp_color(with_alpha(pal::CURSOR, 130), pal::CURSOR, br);
            d.corner_brackets(cx, row_y + 1, cw, ROW_H - 2, cur, 3, 1);
        }
    }

    // Dotted column rules: structure stays one step below labels and values.
    // Draw last so zebra/playhead fills cannot erase the dots.
    constexpr int GRID_TOP = Y0 - 2;
    constexpr int GRID_BOTTOM = 240;
    const Color grid_dot = lerp_color(pal::BG, pal::GRID, 112);
    for (int x : COL_SEP)
        for (int y = GRID_TOP; y < GRID_BOTTOM; y += 3)
            d.rect(x, y, 1, 1, grid_dot);

    // === permanent inspector panel (right third) ===
    draw_phrase_panel(d, GRID_W);
}

} // namespace trackr::ui
