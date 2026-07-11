// App: touch input handling (bottom-screen 320x240).
// Split out of app.cpp. Handles the touch keyboard, mute pads (song view),
// and the sample-editor drag/zoom/pad interactions.
#include "../app.h"
#include "../ui_internal.h"
#include "../../sequencer/scale.h"
#include "../../audio/fixed.h"
#include "../../synth/sampler.h"
#include "../../synth/sample_utils.h"
#include <cstdio>

namespace trackr::ui {

void App::touch_release() {
    mixer_touch_active_ = false;
    if (touch_held_note_ != -1) {
        mixer_.note_off_all(0);
        touch_held_note_ = -1;
    }
    // KAOSS: finger up -> start the release ramp back to the baseline params
    if (kaoss_active_) {
        kaoss_active_ = false;
        kaoss_release_ = KAOSS_REL_FRAMES;
    }
    // finishing a drag in the instrument WAVE panel: loop markers (kinds 3/4)
    // get zero-crossing snapped on release for click-free seams. edge positions
    // (0 / total) are intentional - leave them alone.
    if (smp_touch_active_ && screen_ == Screen::Instrument &&
        project_.instruments[cur_inst_].type == seq::InstrumentType::Sampler &&
        (smp_drag_kind_ == 3 || smp_drag_kind_ == 4)) {
        auto& s = synth::SampleBank::instance()
                      .slot(project_.instruments[cur_inst_].sampler.sample_slot);
        if (!s.empty()) {
            if (smp_drag_kind_ == 3 && s.loop_start > 0) {
                s.loop_start = synth::find_zero_crossing_near(s, s.loop_start);
            } else if (smp_drag_kind_ == 4 && s.loop_end < s.num_frames()) {
                s.loop_end = synth::find_zero_crossing_near(s, s.loop_end);
            }
        }
    }
    smp_touch_active_ = false;
    smp_drag_kind_ = 0;
}

void App::touch_move(int x, int y) {
    // === Mixer faders: keep dragging volume ===
    if (mixer_touch_active_) {
        mixer_fader_touch(x, y, true);
        return;
    }
    // === KAOSS pad drag: keep applying XY while the finger moves ===
    if (kaoss_active_) {
        kaoss_touch(x, y, true);
        return;
    }
    // === instrument bottom panels: drag markers (slice chop / wave start-len) ===
    if (screen_ == Screen::Instrument &&
        project_.instruments[cur_inst_].type == seq::InstrumentType::Sampler) {
        int slot = project_.instruments[cur_inst_].sampler.sample_slot;
        if (inst_panel_ == InstPanel::Slice) { slice_panel_touch(x, y, slot, true); return; }
        if (inst_panel_ == InstPanel::Wave)  { wave_panel_touch(x, y, slot, true);  return; }
    }
    // === keyboard glissando: slide a finger across keys to retrigger notes (legato) ===
    // active whenever we're currently holding a keyboard note (set in touch()).
    if (touch_held_note_ != -1 && !smp_touch_active_) {
        int note = touch_note_at(x, y);
        if (note < 0 || note > 127) {
            // finger slid off the keyboard - go quiet but keep tracking the gesture
            if (touch_held_note_ >= 0) {
                mixer_.note_off_all(0);
                touch_held_note_ = -1;
            }
            return;
        }
        if (note != touch_held_note_) {
            // moved onto a different key - retrigger (keep the press velocity)
            mixer_.note_off_all(0);
            last_kb_note_ = note;
            touch_held_note_ = note;
            seq::Player::apply_inst_fx_defaults(project_.instruments[cur_inst_], mixer_.track(0));
            auto* v = project_.make_voice(cur_inst_);
            mixer_.replace_voice(0, v);
            if (v) v->note_on(note, touch_vel_);
        }
        return;
    }
    // (nothing else to drag - old Sample-screen drag handler removed)
}

void App::touch(int x, int y) {
    dirty = true;   // any touch - set the flag (wrote a note/pressed a button/etc)

    // === theme picker overlay owns ALL touches while open ===
    if (theme_menu_) {
        theme_menu_touch(x, y);
        return;
    }

    // === in-app HELP overlay owns ALL touches while open ===
    if (help_on_) {
        help_touch(x, y);
        return;
    }

    // === FX help picker owns the bottom screen while open ===
    if (screen_ == Screen::Phrase && fx_help_) {
        if (fx_help_touch(x, y)) return;
        return;   // swallow all touches while the picker is open
    }


    // instrument bottom panels (tabs at y=90) only own the band BELOW the tab
    // row. everything above (info line, screen tabs, OCT/REC row) must keep
    // falling through to the global handlers - otherwise an open panel eats
    // taps on the screen tabs (DoubleSprattt, issue #2).
    constexpr int PANEL_BAND_Y = 90;

    // === KB/WAVE/SLICE/LOAD/REC tab buttons (Sampler instrument) - check first ===
    if (screen_ == Screen::Instrument &&
        project_.instruments[cur_inst_].type == seq::InstrumentType::Sampler) {
        if (inst_tab_touch(x, y)) return;
    }

    // === KB/GEN tabs + GEN panel (DrumKit instrument) ===
    if (screen_ == Screen::Instrument &&
        project_.instruments[cur_inst_].type == seq::InstrumentType::DrumKit) {
        if (kit_tab_touch(x, y)) return;
        if (kit_panel_ == KitPanel::Gen && y >= PANEL_BAND_Y) {
            gen_panel_touch(x, y);
            return;   // GEN panel owns the band below the tabs - no keyboard fall-through
        }
    }

    // === WAV loader panel touch (Instrument/Sampler, toggled on) ===
    // tap a row to select it; tap the already-selected row to open/load it.
    if (screen_ == Screen::Instrument && inst_panel_ == InstPanel::Load && y >= PANEL_BAND_Y &&
        project_.instruments[cur_inst_].type == seq::InstrumentType::Sampler) {
        constexpr int PY = 116, LIST_Y = PY + 16, ROW_H = 11, VISIBLE = 9;
        if (wav_count_ > 0 && y >= LIST_Y && y < LIST_Y + VISIBLE * ROW_H) {
            int top = wav_sel_ - VISIBLE / 2;
            if (top > wav_count_ - VISIBLE) top = wav_count_ - VISIBLE;
            if (top < 0) top = 0;
            int row = (y - LIST_Y) / ROW_H;
            int idx = top + row;
            if (idx >= 0 && idx < wav_count_) {
                if (idx == wav_sel_) {
                    // second tap on same row = open/load: synthesize an A press
                    InputState fake{};
                    fake.a = true;
                    load_panel_input(fake, project_.instruments[cur_inst_].sampler.sample_slot);
                } else {
                    wav_sel_ = idx;
                }
            }
        }
        return;   // never fall through to the keyboard
    }

    // === slice editor panel touch (Instrument/Sampler, toggled on) ===
    if (screen_ == Screen::Instrument && inst_panel_ == InstPanel::Slice && y >= PANEL_BAND_Y &&
        project_.instruments[cur_inst_].type == seq::InstrumentType::Sampler) {
        slice_panel_touch(x, y, project_.instruments[cur_inst_].sampler.sample_slot, false);
        smp_touch_active_ = true;   // mark so touch_move drags the chop
        return;
    }
    // === WAVE panel touch (Instrument/Sampler): markers + op buttons ===
    if (screen_ == Screen::Instrument && inst_panel_ == InstPanel::Wave && y >= PANEL_BAND_Y &&
        project_.instruments[cur_inst_].type == seq::InstrumentType::Sampler) {
        wave_panel_touch(x, y, project_.instruments[cur_inst_].sampler.sample_slot, false);
        smp_touch_active_ = true;   // so touch_move keeps dragging markers
        return;
    }
    // === REC panel touch: big arm/stop button (mirrors ZR resample) ===
    if (screen_ == Screen::Instrument && inst_panel_ == InstPanel::Rec && y >= PANEL_BAND_Y &&
        project_.instruments[cur_inst_].type == seq::InstrumentType::Sampler) {
        constexpr int BX = 64, BY = 144, BW = 192, BH = 56;
        if (x >= BX && x < BX + BW && y >= BY && y < BY + BH) {
            constexpr std::size_t MAX_FRAMES = 32000 * 15;
            if (!mixer_.is_resampling()) {
                mixer_.start_resample(MAX_FRAMES);
                if (!player_.playing()) player_.play_song(0);
            } else {
                mixer_.stop_resample();
                std::size_t frames = mixer_.resample_frames();
                if (frames > 0) {
                    auto& s = synth::SampleBank::instance()
                                  .slot(project_.instruments[cur_inst_].sampler.sample_slot);
                    const fx::q15* src = mixer_.resample_data();
                    s.data.assign(src, src + frames * 2);
                    s.channels   = 2;
                    s.root_note  = 60;
                    s.loop_start = 0;
                    s.loop_end   = 0;
                    s.reversed   = false;
                    for (int i = 0; i < synth::Sample::MAX_CHOPS; ++i) s.chops[i] = 0xFFFFFFFFu;
                    mark_dirty();
                }
            }
        }
        return;
    }
    // === KEY: the readout is "<root> <scale>" at x 244..310 y<16.
    // tap the root part (left, x<264) = cycle root note, tap the scale part = cycle
    // scale type. ZL+tap still forces root anywhere (back-compat with the guide).
    // was: root only via ZL+tap -> "it's stuck at C" (discord). discoverability fix.
    if (y < 16 && x >= 244 && x < 310) {
        auto& song = project_.song;
        if (mod_zl_ || x < 264) song.scale_root = (song.scale_root + 1) % 12;
        else                    song.scale_type = (song.scale_type + 1) % seq::SCALE_COUNT;
        mark_dirty();
        return;
    }

    // === theme picker: tap the DESCRY wordmark (top-left, y<16 x<116) ===
    if (y < 16 && x < 116) {
        theme_menu_ = !theme_menu_;
        theme_menu_frame_ = frame_;
        mark_dirty();
        return;
    }

    // === tap tempo: tap the BPM readout (top info line, x 120..240 y<16) ===
    if (y < 16 && x >= 120 && x < 240) {
        uint32_t delta = frame_ - tap_last_frame_;
        tap_last_frame_ = frame_;
        // 60fps: frames-per-beat = 3600/bpm. accept 40..255 bpm (14..90 frames).
        if (delta >= 14 && delta <= 90) {
            int bpm = (int)(3600 / delta);
            if (bpm < 40) bpm = 40; if (bpm > 255) bpm = 255;
            project_.song.bpm = (uint8_t)bpm;
            mark_dirty();
        }
        return;
    }

    // === screen tabs (y28..43): tap to jump to a view ===
    if (y >= 28 && y < 43) {
        constexpr int NT = (int)Screen::NUM;
        int idx = (x - 2) * NT / 316;
        if (idx < 0) idx = 0;
        if (idx >= NT) idx = NT - 1;
        if ((Screen)idx != screen_) {
            nav_dir_ = (idx > (int)screen_) ? +1 : -1;
            prev_screen_ = (uint8_t)screen_;
            screen_ = (Screen)idx;
            screen_change_frame_ = frame_;
        }
        return;
    }

    // === hint strip (y44..62): tap = open the in-app manual ("?" badge) ===
    if (y >= 44 && y < 63) {
        open_help();
        return;
    }

    // === OCT -/+ / KEYS-PADS / REC (y 64-86) ===
    if (y >= 64 && y < 86) {
        if (x >= 4 && x < 44) {
            if (octave_ > 0) octave_--;
            return;
        }
        if (x >= 100 && x < 140) {
            if (octave_ < 7) octave_++;
            return;
        }
        if (x >= 146 && x < 202) {
            // KEYS -> PADS -> KAOSS cycle
            kb_mode_ = (KbMode)(((int)kb_mode_ + 1) % 3);
            return;
        }
        if (x >= 208 && x < 258) {
            // REC toggle. REC records notes you PLAY - so if the drumkit GEN
            // panel is open (no playable surface), flip back to the pads first
            // (gearmo: "tapping REC in GEN mode has no effect").
            rec_mode_ = !rec_mode_;
            if (rec_mode_ && screen_ == Screen::Instrument &&
                project_.instruments[cur_inst_].type == seq::InstrumentType::DrumKit &&
                kit_panel_ == KitPanel::Gen) {
                kit_panel_ = KitPanel::Kb;
            }
            return;
        }
    }

    // === Mixer view: touch faders own the band below the OCT row ===
    if (screen_ == Screen::Mixer && kb_mode_ != KbMode::Kaoss && y >= 96) {
        mixer_fader_touch(x, y, false);
        mixer_touch_active_ = true;
        return;
    }

    // === KAOSS pad: the KB band is an XY performance field ===
    // checked BEFORE the song-view mute pads: kaoss is an explicit mode choice.
    if (kb_mode_ == KbMode::Kaoss && y >= KB_Y) {
        kaoss_touch(x, y, false);
        return;
    }

    // === in song view - live track pads: tap = SOLO toggle ===
    // (mute lives in the mixer faders now; solo is the stage tool)
    if (screen_ == Screen::Song && !rec_mode_ && y >= 140) {
        constexpr int PAD_COLS = 4;
        constexpr int PAD_ROWS = 2;
        constexpr int PAD_W = 320 / PAD_COLS;
        constexpr int PAD_H = (240 - 140) / PAD_ROWS;
        int col = x / PAD_W;
        int row = (y - 140) / PAD_H;
        if (col < 0 || col >= PAD_COLS || row < 0 || row >= PAD_ROWS) return;
        int track = row * PAD_COLS + col;
        if (solo_track_ == track) {
            // unsolo: everything back on
            solo_track_ = -1;
            for (int t = 0; t < seq::NUM_TRACKS; ++t)
                mixer_.track(t).muted = false;
        } else {
            // solo this track: mute all others
            solo_track_ = track;
            for (int t = 0; t < seq::NUM_TRACKS; ++t) {
                bool m = (t != track);
                mixer_.track(t).muted = m;
                if (m) mixer_.note_off_all(t);
            }
        }
        mark_dirty();
        return;
    }

    // (old Sample-view touch UI removed - sample editing lives in the
    //  Instrument view panels now: WAVE / SLICE / LOAD / REC)

    // === keyboard / pads ===
    int note = touch_note_at(x, y);
    if (note < 0 || note > 127) return;
    last_kb_note_ = note;
    touch_held_note_ = note;
    touch_vel_ = touch_velocity(x, y);   // velocity from Y within the key/pad
    pad_flash_frame_ = frame_;           // press-flash animation seed

    // === LIVE REC - write to the playing step instead of the cursor ===
    if (rec_mode_ && player_.playing()) {
        // find any track that's playing cur_phrase_
        int rec_track = -1;
        for (int t = 0; t < seq::NUM_TRACKS; ++t) {
            if (player_.track_state(t).playing &&
                player_.track_state(t).phrase_id == cur_phrase_) {
                rec_track = t;
                break;
            }
        }
        if (rec_track < 0) {
            // cur_phrase isn't playing - fallback: write to track 0's phrase
            if (player_.track_state(0).playing) {
                rec_track = 0;
                cur_phrase_ = player_.track_state(0).phrase_id;
            }
        }
        if (rec_track >= 0) {
            // write to the step that's PLAYING NOW (ts.step already advanced to the next one, take -1)
            int rec_step = (player_.track_state(rec_track).step - 1 + seq::PHRASE_STEPS) % seq::PHRASE_STEPS;
            auto& step = project_.phrases[cur_phrase_].steps[rec_step];
            step.note = (uint8_t)note;
            step.instrument = cur_inst_;
            step.velocity = (uint8_t)touch_vel_;
            last_note_entered_ = note;   // sticky entry follows live rec too
            // preview the note on the selected track (overrides the current note)
            seq::Player::apply_inst_fx_defaults(project_.instruments[cur_inst_], mixer_.track(rec_track));
            auto* v = project_.make_voice(cur_inst_);
            mixer_.replace_voice(rec_track, v);
            if (v) v->note_on(note, touch_vel_);
            // follow the playhead with the cursor so you see where you're writing
            cursor_row_ = rec_step;
            return;
        }
        // nothing is playing - just preview
    } else if (screen_ == Screen::Phrase) {
        // normal write mode - write the note at the current cursor position
        auto& step = project_.phrases[cur_phrase_].steps[cursor_row_];
        step.note = (uint8_t)note;
        step.instrument = cur_inst_;
        step.velocity = (uint8_t)touch_vel_;
        last_note_entered_ = note;   // sticky entry follows the touch keyboard
        cursor_row_ = (cursor_row_ + 1) % seq::PHRASE_STEPS;
    }

    // preview the instrument (in non-rec mode or in any view except phrase).
    // apply the instrument's FX defaults to track 0 first - so the preview goes
    // through the same filter/sends/crush as a sequenced note (WYSIWYG).
    seq::Player::apply_inst_fx_defaults(project_.instruments[cur_inst_], mixer_.track(0));
    auto* v = project_.make_voice(cur_inst_);
    mixer_.replace_voice(0, v);
    if (v) v->note_on(note, touch_vel_);
}

// resolve a touch in the keyboard band to a midi note honoring kb_mode_.
// pads: chromatic 16 from octave_ (or DrumKit base_note so pads == kit pads).
// with a scale set (song.scale_type>0): keys snap to the nearest in-scale note,
// pads become SCALE DEGREES (16 consecutive in-scale notes - no dead pads).
int App::touch_note_at(int x, int y) const {
    if (kb_mode_ == KbMode::Kaoss) return -1;   // kaoss field plays no notes
    const uint8_t st = project_.song.scale_type;
    const uint8_t sr = project_.song.scale_root;
    if (kb_mode_ == KbMode::Keys) {
        int n = find_keyboard_note(x, y, octave_);
        if (n < 0) return n;
        return seq::scale_snap(st, sr, n);
    }
    int p = find_pad_index(x, y);
    if (p < 0) return -1;
    const auto& inst = project_.instruments[cur_inst_];
    if (inst.type == seq::InstrumentType::DrumKit) {
        int n = inst.drumkit.base_note + p;    // kit pads stay chromatic (pad = slot)
        return (n <= 127) ? n : -1;
    }
    if (st != 0) {
        // pad p = p-th in-scale note starting at the octave root
        int n = seq::scale_snap(st, sr, octave_ * 12 + (sr % 12));
        for (int i = 0; i < p && n <= 127; ++i)
            n = seq::scale_step(st, sr, n, +1);
        return (n <= 127) ? n : -1;
    }
    int n = octave_ * 12 + p;
    return (n <= 127) ? n : -1;
}

// touch velocity from the Y position inside the key/pad: bottom = loud (127),
// top = soft. keys use the whole KB band; pads use the position within the cell
// (MPC-style, taller PADS band). floor at 24 so a soft tap is still audible.
int App::touch_velocity(int x, int y) const {
    int v;
    if (kb_mode_ == KbMode::Pads) {
        if (y < PADS_Y) return 110;
        int local = (y - PADS_Y) % pad_cell_h();        // 0..cell_h-1 inside the pad
        v = 24 + local * (127 - 24) / (pad_cell_h() - 1);
    } else {
        if (y < KB_Y) return 110;
        v = 24 + (y - KB_Y) * (127 - 24) / (KB_H - 1);
    }
    if (v < 1) v = 1; if (v > 127) v = 127;
    return v;
}

} // namespace trackr::ui
