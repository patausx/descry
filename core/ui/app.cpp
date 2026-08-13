#include "app.h"
#include "ui_internal.h"

namespace trackr::ui {

// === update ===

void App::update(const InputState& in) {

    // mirror held modifiers for the bottom-screen hint bar
    mod_l_ = in.held_l; mod_r_ = in.held_r; mod_zl_ = in.held_zl;

    // === in-app HELP overlay owns all input while open ===
    if (help_on_) { update_help(in); return; }

    // === UNDO / REDO (global, any screen) ===
    // ZL is the "history/clipboard" modifier: ZL+X/Y = copy/paste (per-screen),
    // ZL+B = undo, ZL+A = redo. Caught here before per-screen editing so B/A
    // don't fall through to value edits.
    if (in.held_zl && in.b) { do_undo(); return; }
    if (in.held_zl && in.a) { do_redo(); return; }

    // in instrument view L+A - clone the current instrument into the first
    // free slot and jump there. answers the "i edited one instrument and
    // another track changed" trap: tweak a copy, the original stays intact.
    if (in.held_l && in.a && screen_ == Screen::Instrument) {
        for (int i = 0; i < seq::MAX_INSTRUMENTS; ++i) {
            if (project_.instruments[i].type == seq::InstrumentType::None) {
                // record against the DESTINATION slot so ZL+B puts it back to empty
                uint8_t prev_inst = cur_inst_;
                cur_inst_ = (uint8_t)i;
                snapshot_inst();
                project_.instruments[i] = project_.instruments[prev_inst];
                commit_inst();
                edit_flash_frame_ = frame_;
                break;
            }
        }
        return;
    }

    // "global" shortcuts: held L + d-pad - edit BPM (up/down +/-1, left/right +/-10)
    //                      held R + up/down - edit groove (ticks per step)
    if (in.held_l && (in.up || in.down || in.left || in.right)) {
        // in table view L+left/right - switch cur_table_
        if (screen_ == Screen::Table && (in.left || in.right)) {
            bump_clamped(cur_table_, in.right ? 1 : -1, 0, seq::MAX_TABLES - 1);
            return;
        }
        // in instrument view L+left/right - switch cur_inst_
        if (screen_ == Screen::Instrument && (in.left || in.right)) {
            bump_clamped(cur_inst_, in.right ? 1 : -1, 0, seq::MAX_INSTRUMENTS - 1);
            return;
        }
        // in phrase view L+left/right - switch cur_phrase_
        if (screen_ == Screen::Phrase && (in.left || in.right)) {
            bump_clamped(cur_phrase_, in.right ? 1 : -1, 0, seq::MAX_PHRASES - 1);
            return;
        }
        // in chain view L+left/right - switch cur_chain_
        if (screen_ == Screen::Chain && (in.left || in.right)) {
            bump_clamped(cur_chain_, in.right ? 1 : -1, 0, seq::MAX_CHAINS - 1);
            return;
        }
        int bpm = project_.song.bpm;
        if (in.up)    bpm += 1;
        if (in.down)  bpm -= 1;
        if (in.right) bpm += 10;
        if (in.left)  bpm -= 10;
        const uint8_t next = (uint8_t)clamp_int(bpm, 30, 255);
        if (next != project_.song.bpm) {
            project_.song.bpm = next;
            mark_project_dirty();
        }
        return;
    }
    if (in.held_r && (in.up || in.down) && screen_ != Screen::Project) {
        if (bump_clamped(project_.song.groove, in.up ? 1 : -1, 1, 24))
            mark_project_dirty();
        return;
    }
    // held R + left/right - edit swing (shuffle/groove)
    if (in.held_r && (in.left || in.right) && screen_ != Screen::Project) {
        if (bump_clamped(project_.song.swing, in.right ? 2 : -2, 0, 50))
            mark_project_dirty();
        return;
    }

    // switch screen via L/R (without hold). 6 main tabs (Sample was removed).
    if (in.l && !in.held_r) {
        int s = wrap_index((int)screen_, -1, (int)Screen::NUM);
        prev_screen_ = (uint8_t)screen_;
        screen_ = (Screen)s;
        screen_change_frame_ = frame_;
        nav_dir_ = -1;
        return;
    }
    if (in.r && !in.held_l) {
        int s = wrap_index((int)screen_, +1, (int)Screen::NUM);
        prev_screen_ = (uint8_t)screen_;
        screen_ = (Screen)s;
        screen_change_frame_ = frame_;
        nav_dir_ = +1;
        return;
    }

    // play/stop via start (contextual: Phrase = single phrase, Chain = loop the
    // chain on track 0 (m8 chain preview), otherwise - song).
    // in the SONG view playback starts at the CURSOR row (m8/lsdj behaviour):
    // working on row 40 of an arrangement used to mean re-listening from row 0
    // on every press. L+START (or any other view) still starts from the top.
    if (in.start) {
        if (player_.playing()) {
            player_.stop();
        } else if (screen_ == Screen::Phrase) {
            // play the current phrase on track 0
            player_.play_phrase(0, cur_phrase_);
        } else if (screen_ == Screen::Chain) {
            player_.play_chain(0, cur_chain_);
        } else if (screen_ == Screen::Song && !in.held_l) {
            player_.play_song((uint16_t)song_row_);
        } else {
            player_.play_song(0);
        }
    }

    // select = preview note under cursor (trigger this instrument).
    // on FX-cmd columns SELECT opens the FX help picker instead (update_phrase),
    // and on the INSTRUMENT column it drills into the instrument editor
    // (update_phrase) - completing song > chain > phrase > instrument.
    // HOLD-TO-SUSTAIN: the note gates off when SELECT is released, so you can
    // hold a note one-handed while tweaking params (DoubleSprattt request).
    if (in.select_ && screen_ == Screen::Phrase &&
        cursor_col_ != 1 &&
        cursor_col_ != 3 && cursor_col_ != 5 && cursor_col_ != 7) {
        const auto& step = project_.phrases[cur_phrase_].steps[cursor_row_];
        if (step.note != seq::EMPTY && step.instrument != seq::EMPTY) {
            if (mixer_.start_voice(0, project_.make_voice(step.instrument),
                                   step.note, step.velocity))
                preview_gate_ = true;
        }
    }

    // preview gate release (works from ANY screen - survives screen switches)
    if (preview_gate_ && !in.held_select) {
        mixer_.note_off_all(0);
        preview_gate_ = false;
    }

    switch (screen_) {
        case Screen::Phrase:     update_phrase(in);     break;
        case Screen::Chain:      update_chain(in);      break;
        case Screen::Song:       update_song(in);       break;
        case Screen::Instrument: update_instrument(in); break;
        case Screen::Table:      update_table(in);      break;
        case Screen::Mixer:      update_mixer(in);      break;
        case Screen::Project:    update_project(in);    break;
        default: break;
    }
}
void App::tick() {
    // animation layer: global frame counter for trail/flash/pulses
    ++frame_;

    // Finish symmetric overlay closes after four frames. Flags stay active until
    // this point, so dismissing input never leaks into the screen underneath.
    if (overlay_close_frame_ && frame_ - overlay_close_frame_ >= 4) {
        if (theme_menu_closing_) { theme_menu_ = false; theme_menu_closing_ = false; }
        if (help_closing_) { help_on_ = false; help_closing_ = false; }
        if (fx_help_closing_) { fx_help_ = false; fx_help_closing_ = false; }
        if (phrase_tools_closing_) {
            phrase_tools_on_ = false; phrase_tool_config_ = false;
            phrase_tools_closing_ = false;
        }
        if (kaoss_menu_closing_) { kaoss_menu_ = 0; kaoss_menu_closing_ = 0; }
        overlay_close_frame_ = 0;
    }

    // === recording tint: the palette runs hot while capturing ===
    // mic/resample capture = strong breathing red (urgent);
    // live-REC mode armed = subtle constant warm tint (armed, not panicking).
    if (recording_now_) {
        pal::apply_theme(theme_idx);                    // fresh base
        // fast breathe (~0.7s period): urgency without strobing
        uint8_t b = breathe_pulse(frame_, 42);
        pal::apply_record_tint((uint8_t)(140 + (b * 100) / 255));   // 140..240
        rec_tint_on_ = true;
    } else if (rec_mode_ == RecMode::Live) {
        pal::apply_theme(theme_idx);
        pal::apply_record_tint(70);                     // armed: gentle warm shift
        rec_tint_on_ = true;
    } else if (rec_tint_on_) {
        pal::apply_theme(theme_idx);                    // restore clean theme
        rec_tint_on_ = false;
    }

    // KAOSS pad: release ramp + trail dissipation
    kaoss_tick();

    // detect playhead step change - for the pulse on each new step.
    // any playing track counts (the header PLAY dot beats on every screen);
    // track 0 preferred so the pulse follows the main groove.
    int ps = -1;
    if (player_.playing()) {
        for (int t = 0; t < seq::NUM_TRACKS; ++t) {
            if (player_.track_state(t).playing) { ps = player_.track_state(t).play_step; break; }
        }
    }
    if (ps != last_playing_step_) {
        last_playing_step_ = ps;
        if (ps >= 0) step_change_frame_ = frame_;
    }

    // detect queued-chain launches/stops without touching the sequencer contract.
    // A queue disappearing is either a launch at the bar boundary or a manual
    // toggle-cancel; verify the resulting track state so cancellations stay quiet.
    for (int t = 0; t < seq::NUM_TRACKS; ++t) {
        const uint8_t q = player_.queued(t);
        if (launch_prev_queue_[t] != seq::EMPTY && q == seq::EMPTY) {
            const auto& ts = player_.track_state(t);
            const bool stopped = launch_prev_queue_[t] == seq::Player::QUEUE_STOP && !ts.playing;
            const bool launched = launch_prev_queue_[t] != seq::Player::QUEUE_STOP &&
                                  ts.playing && ts.chain_id == launch_prev_queue_[t];
            if (stopped || launched) {
                launch_motion_mask_ |= (uint8_t)(1u << t);
                if (stopped) launch_stop_mask_ |= (uint8_t)(1u << t);
                else         launch_stop_mask_ &= (uint8_t)~(1u << t);
                launch_motion_frame_ = frame_;
            }
        }
        launch_prev_queue_[t] = q;
    }
    if (launch_motion_frame_ && frame_ - launch_motion_frame_ >= 14) {
        launch_motion_mask_ = 0;
        launch_stop_mask_ = 0;
    }

    // mixer peak-hold caps: track the recent maximum, let it fall slowly
    // (~3.4s full-scale). classic meter caps on both mixer meter columns.
    for (int t = 0; t < seq::NUM_TRACKS; ++t) {
        fx::q15 m = mixer_.track(t).meter;
        if (m > peak_hold_[t]) peak_hold_[t] = m;
        else {
            int nv = (int)peak_hold_[t] - 160;
            peak_hold_[t] = (fx::q15)(nv < 0 ? 0 : nv);
        }
    }
}

} // namespace trackr::ui
