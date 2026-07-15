#include "app.h"
#include "ui_internal.h"
#include "../sequencer/scale.h"
#include "../audio/fixed.h"
#include "../synth/drum_gen.h"
#include "../synth/fm_presets.h"
#include "../synth/wave_presets.h"
#include "../synth/wav_loader.h"
#include "../synth/sample_utils.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>

namespace trackr::ui {

// === update ===

void App::update(const InputState& in) {

    // mirror held modifiers for the bottom-screen hint bar
    mod_l_ = in.held_l; mod_r_ = in.held_r; mod_zl_ = in.held_zl;

    // === in-app HELP overlay owns all input while open ===
    if (help_on_) { update_help(in); return; }

    // === LIVE STICK MODULATION (synced with the KAOSS pad) ===
    // LEFT stick = the kaoss pad on a stick: X/Y drive the same two assigned
    // destinations (kaoss_dest_x_/y_) and honor the TRK/ALL target toggle.
    // RIGHT stick keeps a fixed complementary pair: X -> delay+reverb send,
    // Y (pull down) -> bitcrush+downsample. four performance axes total.
    // baseline contract (same as kaoss): snapshot the grabbed tracks when a
    // stick first deflects, ramp back on release - nothing stays stuck.
    {
        int trk = song_col_;
        if (trk < 0) trk = 0;
        if (trk >= 8) trk = 7;
        const bool stick_on = (in.lstick_active && stick_sync_) || in.cstick_active;

        if (stick_on && !stick_was_on_) {
            // gesture start: grab the target track(s) + snapshot baselines
            stick_track_ = trk;
            stick_mask_  = kaoss_all_ ? 0xFF : (uint8_t)(1u << trk);
            for (int t = 0; t < 8; ++t) {
                if (!(stick_mask_ & (1u << t))) continue;
                auto& ts = mixer_.track(t);
                stick_bases_[t] = { ts.cutoff, ts.resonance, ts.send_del, ts.send_rev,
                                    ts.mg_rate, ts.mix_vol, ts.pan,
                                    ts.mg_to_cutoff, ts.mg_to_vca, ts.bits, ts.downsample };
            }
            stick_release_ = 0;
        }
        if (!stick_on && stick_was_on_) {
            // release: ramp the grabbed tracks back to baseline (in tick())
            stick_release_ = KAOSS_REL_FRAMES;
        }
        stick_was_on_ = stick_on;

        // left stick -> assigned kaoss destinations (0..1000 field coords).
        // stick_sync_ off = left stick unplugged (kaoss toggle on the pad)
        if (in.lstick_active && stick_sync_) {
            int vx = (in.lstick_x + 1000) / 2;   // -1000..1000 -> 0..1000
            int vy = (in.lstick_y + 1000) / 2;
            if (vx < 0) vx = 0; if (vx > 1000) vx = 1000;
            if (vy < 0) vy = 0; if (vy > 1000) vy = 1000;
            for (int t = 0; t < 8; ++t) {
                if (!(stick_mask_ & (1u << t))) continue;
                apply_kaoss_dest(t, (uint8_t)kaoss_dest_x_, vx);
                apply_kaoss_dest(t, (uint8_t)kaoss_dest_y_, vy);
            }
            // mirror onto the kaoss crosshair - pad and stick show one truth
            kaoss_x_ = vx;
            kaoss_y_ = vy;
            dirty = true;
        }
        // right stick: X -> delay+reverb send (0..ONE, perceptual x(2-x) curve)
        if (in.cstick_active) {
            int v = (in.cstick_x + 1000) * fx::Q15_ONE / 2000;
            if (v < 0) v = 0; if (v > fx::Q15_ONE) v = fx::Q15_ONE;
            v = (int)(((int64_t)v * (2 * fx::Q15_ONE - v)) / fx::Q15_ONE);
            if (v > fx::Q15_ONE) v = fx::Q15_ONE;
            // right Y -> bitcrush. center/up = clean, down = grittier.
            // bit-mask crush is only audible below ~8 bits, so spend most travel there.
            int down = in.cstick_y < 0 ? -in.cstick_y : 0;   // 0..1000 pull-down amount
            int bits;
            if (down < 300) {
                bits = 16 - down * 8 / 300;         // 16 -> 8 over first 30%
            } else {
                bits = 8 - (down - 300) * 6 / 700;  // 8 -> 2 over the rest
            }
            if (bits < 2) bits = 2; if (bits > 16) bits = 16;
            uint8_t dsm = (down > 500) ? (uint8_t)(1 + (down - 500) * 4 / 500) : 1;
            for (int t = 0; t < 8; ++t) {
                if (!(stick_mask_ & (1u << t))) continue;
                auto& ts = mixer_.track(t);
                ts.perf_hold |= audio::TrackState::HOLD_DEL |
                                audio::TrackState::HOLD_REV |
                                audio::TrackState::HOLD_BIT;
                ts.send_del = (fx::q15)v;
                ts.send_rev = (fx::q15)v;
                ts.bits = (uint8_t)bits;
                ts.downsample = dsm;
            }
            dirty = true;
        }

        // mirror state for the on-screen indicator (read in draw_top)
        auto& ts = mixer_.track(stick_on ? stick_track_ : trk);
        perf_lstick_on_ = in.lstick_active && stick_sync_;
        perf_cstick_on_ = in.cstick_active;
        perf_cutoff_ = (int)ts.cutoff * 100 / fx::Q15_ONE;
        perf_reso_   = (int)ts.resonance * 100 / fx::Q15_ONE;
        perf_send_   = (int)ts.send_del * 100 / fx::Q15_ONE;
        perf_bits_   = ts.bits;
    }
    // any a/b/x/y press or BPM/groove change = dirty
    if (in.a || in.b || in.x || in.y) dirty = true;

    // === UNDO / REDO (global, any screen) ===
    // ZL is the "history/clipboard" modifier: ZL+X/Y = copy/paste (per-screen),
    // ZL+B = undo, ZL+A = redo. Caught here before per-screen editing so B/A
    // don't fall through to value edits.
    if (in.held_zl && in.b) { do_undo(); return; }
    if (in.held_zl && in.a) { do_redo(); return; }

    if (in.held_l && (in.up || in.down || in.left || in.right)) dirty = true;
    if (in.held_r && (in.up || in.down)) dirty = true;

    // in instrument view L+A - clone the current instrument into the first
    // free slot and jump there. answers the "i edited one instrument and
    // another track changed" trap: tweak a copy, the original stays intact.
    if (in.held_l && in.a && screen_ == Screen::Instrument) {
        for (int i = 0; i < seq::MAX_INSTRUMENTS; ++i) {
            if (project_.instruments[i].type == seq::InstrumentType::None) {
                project_.instruments[i] = project_.instruments[cur_inst_];
                cur_inst_ = (uint8_t)i;
                edit_flash_frame_ = frame_;
                mark_dirty();
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
            int v = (int)cur_table_ + (in.right ? 1 : -1);
            if (v < 0) v = 0;
            if (v >= seq::MAX_TABLES) v = seq::MAX_TABLES - 1;
            cur_table_ = (uint8_t)v;
            return;
        }
        // in instrument view L+left/right - switch cur_inst_
        if (screen_ == Screen::Instrument && (in.left || in.right)) {
            int v = (int)cur_inst_ + (in.right ? 1 : -1);
            if (v < 0) v = 0;
            if (v >= seq::MAX_INSTRUMENTS) v = seq::MAX_INSTRUMENTS - 1;
            cur_inst_ = (uint8_t)v;
            return;
        }
        // in phrase view L+left/right - switch cur_phrase_
        if (screen_ == Screen::Phrase && (in.left || in.right)) {
            int v = (int)cur_phrase_ + (in.right ? 1 : -1);
            if (v < 0) v = 0;
            if (v >= seq::MAX_PHRASES) v = seq::MAX_PHRASES - 1;
            cur_phrase_ = (uint8_t)v;
            return;
        }
        // in chain view L+left/right - switch cur_chain_
        if (screen_ == Screen::Chain && (in.left || in.right)) {
            int v = (int)cur_chain_ + (in.right ? 1 : -1);
            if (v < 0) v = 0;
            if (v >= seq::MAX_CHAINS) v = seq::MAX_CHAINS - 1;
            cur_chain_ = (uint8_t)v;
            return;
        }
        int bpm = project_.song.bpm;
        if (in.up)    bpm += 1;
        if (in.down)  bpm -= 1;
        if (in.right) bpm += 10;
        if (in.left)  bpm -= 10;
        if (bpm < 30)  bpm = 30;
        if (bpm > 255) bpm = 255;
        project_.song.bpm = (uint8_t)bpm;
        return;
    }
    if (in.held_r && (in.up || in.down) && screen_ != Screen::Project) {
        int g = project_.song.groove + (in.up ? 1 : -1);
        if (g < 1) g = 1;
        if (g > 24) g = 24;
        project_.song.groove = (uint8_t)g;
        return;
    }
    // held R + left/right - edit swing (shuffle/groove)
    if (in.held_r && (in.left || in.right) && screen_ != Screen::Project) {
        dirty = true;
        int sw = project_.song.swing + (in.right ? 2 : -2);
        if (sw < 0) sw = 0;
        if (sw > 50) sw = 50;
        project_.song.swing = (uint8_t)sw;
        return;
    }

    // switch screen via L/R (without hold). 6 main tabs (Sample was removed).
    if (in.l && !in.held_r) {
        int s = (int)screen_ - 1;
        if (s < 0) s = (int)Screen::NUM - 1;
        prev_screen_ = (uint8_t)screen_;
        screen_ = (Screen)s;
        screen_change_frame_ = frame_;
        nav_dir_ = -1;
        return;
    }
    if (in.r && !in.held_l) {
        int s = (int)screen_ + 1;
        if (s >= (int)Screen::NUM) s = 0;
        prev_screen_ = (uint8_t)screen_;
        screen_ = (Screen)s;
        screen_change_frame_ = frame_;
        nav_dir_ = +1;
        return;
    }

    // play/stop via start (contextual: Phrase = single phrase, Chain = loop the
    // chain on track 0 (m8 chain preview), otherwise - song)
    if (in.start) {
        if (player_.playing()) {
            player_.stop();
        } else if (screen_ == Screen::Phrase) {
            // play the current phrase on track 0
            player_.play_phrase(0, cur_phrase_);
        } else if (screen_ == Screen::Chain) {
            player_.play_chain(0, cur_chain_);
        } else {
            player_.play_song(0);
        }
    }

    // select = preview note under cursor (trigger this instrument).
    // on FX-cmd columns SELECT opens the FX help picker instead (update_phrase).
    // HOLD-TO-SUSTAIN: the note gates off when SELECT is released, so you can
    // hold a note one-handed while tweaking params (DoubleSprattt request).
    if (in.select_ && screen_ == Screen::Phrase &&
        cursor_col_ != 3 && cursor_col_ != 5 && cursor_col_ != 7) {
        const auto& step = project_.phrases[cur_phrase_].steps[cursor_row_];
        if (step.note != seq::EMPTY && step.instrument != seq::EMPTY) {
            auto* v = project_.make_voice(step.instrument);
            mixer_.replace_voice(0, v);
            if (v) v->note_on(step.note, step.velocity);
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

// === draw ===

// fullscreen oscilloscope (performance visualizer) - the entire top screen 400x240
// master wave in the top band + 8 per-track mini scopes in a 4x2 grid below.
void App::draw_scope_fullscreen(Draw& d) {
    constexpr int W = 400, H = 240;
    constexpr int MH = 150;              // master wave band height
    constexpr int MID = MH / 2 + 10;     // master center axis (offset below the title)

    // dark background
    d.rect(0, 0, W, H, pal::BG);

    // grid (subtle) - horizontals at 1/4, 3/4 of the master band
    d.rect(0, 10 + MH / 4,     W, 1, pal::BG_HI);
    d.rect(0, 10 + 3 * MH / 4, W, 1, pal::BG_HI);
    // verticals every 50px
    for (int gx = 50; gx < W; gx += 50) d.rect(gx, 10, 1, MH, pal::BG_HI);
    // center axis
    d.rect(0, MID, W, 1, pal::GRID);

    // master wave: walk all 512 samples, map onto 400 columns
    // draw as a filled line from the center (envelope) - thicker than dots
    const auto& mxr = mixer_;
    std::size_t pos = mxr.scope_write_pos;
    constexpr int N = (int)audio::Mixer::SCOPE_SIZE;  // 512
    constexpr int AMP = MH / 2 - 6;
    int prev_y = MID;
    for (int x = 0; x < W; ++x) {
        std::size_t idx = (pos + N - (std::size_t)N + (std::size_t)(x * N / W)) % N;
        int32_t v = mxr.scope[idx];
        int y_off = (v * AMP) / 32768;
        int y = MID - y_off;
        if (y < 12) y = 12;
        if (y >= 10 + MH) y = 10 + MH - 1;

        // vertical line from center to y (envelope fill, glow)
        int top = y < MID ? y : MID;
        int hgt = y < MID ? (MID - y) : (y - MID);
        if (hgt < 1) hgt = 1;
        d.rect(x, top, 1, hgt, pal::PLAY_BG);            // deep green glow column

        // bright line along the top (connect to the previous point)
        int ly0 = prev_y < y ? prev_y : y;
        int ly1 = prev_y < y ? y : prev_y;
        d.rect(x, ly0, 1, (ly1 - ly0) + 1, pal::PLAY);  // bright green
        prev_y = y;
    }

    // === 8 per-track mini scopes (4x2 grid below the master band) ===
    {
        constexpr int GY = 10 + MH + 4;              // grid top (164)
        constexpr int CW = W / 4;                    // 100 per cell
        constexpr int CH = (H - GY - 12) / 2;        // ~32 per cell
        constexpr int TN = (int)audio::TrackState::TSCOPE_SIZE;   // 256
        // track accent colors: reuse the 4-color track palette, cycled
        const Color tcol[4] = { pal::TRACK0, pal::TRACK1, pal::TRACK2, pal::TRACK3 };

        for (int t = 0; t < audio::NUM_TRACKS; ++t) {
            int cx = (t % 4) * CW;
            int cy = GY + (t / 4) * CH;
            int mid = cy + CH / 2;
            const auto& tr = mixer_.track(t);
            const bool live = !tr.muted && (tr.meter > 0);
            Color wave = live ? tcol[t % 4] : pal::FG_DIM;

            // cell frame + center line
            d.rect(cx + 2, cy, CW - 4, 1, pal::BG_HI);
            d.rect(cx + 2, cy + CH - 1, CW - 4, 1, pal::BG_HI);
            d.rect(cx + 2, mid, CW - 4, 1, pal::GRID);

            // wave: TSCOPE_SIZE samples onto CW-8 columns
            constexpr int WW = CW - 8;
            int amp = CH / 2 - 2;
            std::size_t tp = tr.tscope_pos;
            int py = mid;
            for (int x = 0; x < WW; ++x) {
                std::size_t idx = (tp + (std::size_t)(x * TN / WW)) % (std::size_t)TN;
                int y_off = ((int32_t)tr.tscope[idx] * amp) / 32768;
                int y = mid - y_off;
                if (y < cy + 1) y = cy + 1;
                if (y > cy + CH - 2) y = cy + CH - 2;
                int ly0 = py < y ? py : y;
                int ly1 = py < y ? y : py;
                d.rect(cx + 4 + x, ly0, 1, (ly1 - ly0) + 1, wave);
                py = y;
            }

            // track label (dim when silent, MUTE flag in red)
            char tb[4];
            std::snprintf(tb, sizeof(tb), "T%d", t);
            d.text(cx + 4, cy + 2, tb, live ? wave : pal::FG_DIM);
            if (tr.muted) d.text(cx + 18, cy + 2, "M", pal::RECORD);
        }
    }

    // frame
    d.rect(0, 0, W, 1, pal::GRID);
    d.rect(0, H - 1, W, 1, pal::GRID);

    // title + BPM + hint
    char buf[40];
    d.text(6, 2, "SCOPE", pal::PLAY, 1);
    std::snprintf(buf, sizeof(buf), "BPM%03d", project_.song.bpm);
    d.text(340, 2, buf, pal::FG_DIM, 1);
    d.text(6, H - 10, "L+SELECT — exit scope", pal::FG_DIM, 1);
    // underrun diagnosis: shows real starvation events on-device. 0 = clean;
    // a growing number while playing = the synth can't keep the dsp fed.
    if (debug_xruns > 0) {
        char xb[20];
        std::snprintf(xb, sizeof(xb), "XRUN %lu", (unsigned long)debug_xruns);
        d.text(W - 6 * 12, H - 10, xb, pal::RECORD, 1);
    }
}

static const char* screen_name(Screen s) {
    switch (s) {
        case Screen::Song: return "SONG";
        case Screen::Chain: return "CHAIN";
        case Screen::Phrase: return "PHRASE";
        case Screen::Instrument: return "INST";
        case Screen::Table: return "TABLE";
        case Screen::Mixer: return "MIXER";
        case Screen::Project: return "PROJECT";
        default: return "?";
    }
}

// tiny 8x8 pictogram for each screen, drawn from rects. x,y = top-left.
// design rules: fill the full 8x8 box, symmetric where possible, 2px features
// so they survive 1x on the 3ds screen without looking like noise.
static void draw_screen_icon(Draw& d, Screen s, int x, int y, Color c) {
    auto box = [&](int cx, int cy, int w, int h) { d.rect(x + cx, y + cy, w, h, c); };
    switch (s) {
        case Screen::Song:        // 3 solid rows = arrangement list
            box(0, 0, 8, 2); box(0, 3, 8, 2); box(0, 6, 8, 2);
            break;
        case Screen::Chain:       // two links, offset + connected
            // link 1 (top-left) and link 2 (bottom-right), 2px joint between
            box(0, 0, 5, 2); box(0, 0, 2, 5);          // L-shape link 1
            box(3, 3, 2, 2);                            // joint
            box(6, 3, 2, 5); box(3, 6, 5, 2);          // L-shape link 2
            break;
        case Screen::Phrase:      // 2x2 fat step cells = the phrase grid
            box(0, 0, 3, 3); box(5, 0, 3, 3);
            box(0, 5, 3, 3); box(5, 5, 3, 3);
            break;
        case Screen::Instrument:  // sine wave, 2px stroke, full width
            box(0, 4, 1, 3); box(1, 2, 1, 3); box(2, 0, 2, 3);
            box(4, 2, 1, 3); box(5, 4, 2, 3); box(7, 2, 1, 3);
            break;
        case Screen::Table:       // 3 rows of cmd+val pairs (mini fx list)
            box(0, 0, 2, 2); box(3, 0, 5, 2);
            box(0, 3, 2, 2); box(3, 3, 5, 2);
            box(0, 6, 2, 2); box(3, 6, 5, 2);
            break;
        case Screen::Mixer:       // 3 faders, caps at different heights
            box(1, 0, 1, 8); box(0, 1, 3, 2);
            box(4, 0, 1, 8); box(3, 4, 3, 2);
            box(7, 0, 1, 8); box(6, 2, 2, 2);   // right fader clipped to 8px
            break;
        case Screen::Project:     // floppy: outline body, solid label bar
            box(0, 0, 7, 1); box(7, 1, 1, 1);   // top edge + notched corner px
            box(0, 0, 1, 8); box(7, 1, 1, 7);   // sides
            box(0, 7, 8, 1);                     // bottom
            box(3, 1, 2, 2);                     // shutter slot
            box(2, 5, 4, 2);                     // label
            break;
        default: break;
    }
}

// breathing pulse 0..255 from the global frame counter (triangle wave, ~1.4s period).
// used to make the active nav icon gently pulse.
static uint8_t breathe(uint32_t frame) {
    uint32_t p = frame % 84;            // ~1.4s @ 60fps
    uint32_t t = (p < 42) ? p : (84 - p);  // 0..42..0
    return (uint8_t)(t * 255 / 42);
}

// nav strip: a row of the 6 screen icons across the header, M8-style tab map.
// the active screen gets a pink capsule + bright icon + breathing accent.
// the capsule SLIDES from the previous icon on switch (ease-out, ~8 frames).
// returns the x just past the strip (so callers can place the next widget).
static int draw_nav_strip(Draw& d, Screen active, Screen prev, uint32_t frame,
                          uint32_t change_frame) {
    constexpr int X0 = 3;
    constexpr int CELL = 13;              // 8px icon + 5px gap
    constexpr int IY = 3;                 // icon top
    constexpr uint32_t SLIDE = 8;         // frames
    uint8_t b = breathe(frame);

    // capsule x: lerp from prev icon to active icon over SLIDE frames
    int ax = X0 + (int)active * CELL;
    uint32_t sd = frame - change_frame;
    if (change_frame != 0 && sd < SLIDE && prev != active) {
        int px = X0 + (int)prev * CELL;
        // ease-out: move fast first, settle at the end
        int t = (int)sd;
        ax = px + ((ax - px) * (t * (2 * (int)SLIDE - t))) / ((int)SLIDE * (int)SLIDE);
    }
    {
        uint8_t a = (uint8_t)(0x40 + b / 4);   // 0x40..0x80
        d.rect(ax - 2, IY - 2, 12, 12, with_alpha(pal::CURSOR, a));
        d.rect(ax - 2, IY - 2, 12, 1, pal::CURSOR);
        d.rect(ax - 2, IY + 9, 12, 1, pal::CURSOR);
    }
    int x = X0;
    for (int i = 0; i < (int)Screen::NUM; ++i) {
        Screen s = (Screen)i;
        Color ic = (s == active) ? pal::FG : pal::FG_DIM;
        draw_screen_icon(d, s, x, IY, ic);
        x += CELL;
    }
    return x;   // ~82 (6 icons) / ~95 (with sample active)
}

void App::draw_top(Draw& d) {
    // background
    d.rect(0, 0, 400, 240, pal::BG);

    // === fullscreen scope (performance visualizer) ===
    if (scope_full) {
        draw_scope_fullscreen(d);
        return;
    }

    // header bar
    d.rect(0, 0, 400, 14, pal::BG_HI);
    char buf[32];

    // === nav strip: row of all 7 screen icons, active one highlighted + breathing ===
    int nav_end = draw_nav_strip(d, screen_, (Screen)prev_screen_, frame_,
                                 screen_change_frame_);

    // === breadcrumb: active screen name + context id, slides in on screen change ===
    {
        // slide offset: name enters from the side it came (nav_dir_), eases to 0
        uint32_t sd = frame_ - screen_change_frame_;
        int slide = 0;
        if (screen_change_frame_ != 0 && sd < NAV_SLIDE_FRAMES) {
            int rem = (int)(NAV_SLIDE_FRAMES - sd);        // NAV_SLIDE_FRAMES..1
            slide = nav_dir_ * rem * rem / 4;              // ease-out, dir-aware
        }
        int bx = nav_end + 6 + slide;
        // brightness fades in over the slide
        uint8_t na = (screen_change_frame_ != 0 && sd < NAV_SLIDE_FRAMES)
                     ? (uint8_t)(0x60 + sd * (0xFF - 0x60) / NAV_SLIDE_FRAMES) : 0xFF;
        Color name_c = with_alpha(pal::FG, na);
        d.text(bx, 3, screen_name(screen_), name_c, 1);
        int cx = bx + (int)std::strlen(screen_name(screen_)) * 6 + 6;

        // context id after the name (e.g. 0A for phrase, plus inst)
        switch (screen_) {
            case Screen::Phrase: {
                std::snprintf(buf, sizeof(buf), "%02X", cur_phrase_);
                d.text(cx, 3, buf, with_alpha(pal::CURSOR, na), 1);
                std::snprintf(buf, sizeof(buf), "I%02X", cur_inst_);
                d.text(cx + 16, 3, buf, pal::FG_DIM, 1);
                break;
            }
            case Screen::Chain:
                std::snprintf(buf, sizeof(buf), "%02X", cur_chain_);
                d.text(cx, 3, buf, with_alpha(pal::CURSOR, na), 1);
                break;
            case Screen::Instrument:
                std::snprintf(buf, sizeof(buf), "%02X", cur_inst_);
                d.text(cx, 3, buf, with_alpha(pal::CURSOR, na), 1);
                break;
            case Screen::Table:
                std::snprintf(buf, sizeof(buf), "%02X", cur_table_);
                d.text(cx, 3, buf, with_alpha(pal::CURSOR, na), 1);
                break;
            default: break;
        }
    }

    // play / rec banners (left side of the header, never under the battery/clock).
    // both pulse via frame_ so the state reads at a glance like a transport light.
    if (rec_mode_ == RecMode::Live) {
        // blinking red banner — LIVE REC takes priority over PLAY
        d.rect(0, 0, 400, 14, pal::RECORD);
        d.text(150, 3, "● REC LIVE ●", pal::FG);
        d.text(4, 3, screen_name(screen_), pal::FG, 1);
    } else if (player_.playing()) {
        // PLAY dot beats WITH the music: flashes bright on every step change and
        // decays - a real transport light instead of a dumb timer blink.
        uint32_t pd = frame_ - step_change_frame_;
        Color dot = (pd < 8) ? lerp_color(pal::PLAY, pal::BG_HI, (uint8_t)(pd * 28))
                             : pal::BG_HI;
        d.rect(206, 4, 8, 8, dot);
    }

    // bpm - highlighted if not default
    std::snprintf(buf, sizeof(buf), "BPM%03d", project_.song.bpm);
    Color bpm_col = (project_.song.bpm == 120) ? pal::FG_DIM : pal::CURSOR;
    d.text(220, 3, buf, bpm_col, 1);

    // groove (ticks per step)
    std::snprintf(buf, sizeof(buf), "GRV%02d", project_.song.groove);
    Color grv_col = (project_.song.groove == 6) ? pal::FG_DIM : pal::CURSOR;  // 6 = default
    d.text(270, 3, buf, grv_col, 1);

    // REC target slot - always visible so you know where it's writing
    int rec_t = rec_target_slot();
    std::snprintf(buf, sizeof(buf), "REC>%02d", rec_t);
    d.text(304, 3, buf, pal::RECORD, 1);

    // === clock + battery (right corner) ===
    // clock HH:MM
    if (clock_hour >= 0) {
        std::snprintf(buf, sizeof(buf), "%02d:%02d", clock_hour, clock_min);
        d.text(352, 3, buf, pal::FG_DIM, 1);
    }
    // battery - mini icon right at the edge (x in 384..398)
    {
        int bx = 384, by = 3, bw = 12, bh = 7;
        // battery body (frame)
        Color frame = pal::FG_DIM;
        d.rect(bx, by, bw, bh, frame);
        d.rect(bx + 1, by + 1, bw - 2, bh - 2, pal::BG);
        d.rect(bx + bw, by + 2, 1, bh - 4, frame);  // positive terminal nub
        // fill by level (0..5 -> 0..10px)
        int lvl = battery_level < 0 ? 5 : battery_level;
        if (lvl > 5) lvl = 5;
        int fill = (bw - 2) * lvl / 5;
        Color bat_col = battery_charging ? pal::PLAY
                      : (lvl <= 1 ? pal::RECORD : pal::TRACK1);
        if (fill > 0) d.rect(bx + 1, by + 1, fill, bh - 2, bat_col);
    }

    // === live DSP cluster (replaces the old full-width text overlay) ===
    // four tiny vertical bars in the header row: CUT RES SND BIT of the
    // "performance" track (kaoss/stick target = song_col_). always visible as a
    // dashboard; lights up bright while a gesture is actually modulating.
    {
        const bool kaoss_live = (kb_mode_ == KbMode::Kaoss) && (kaoss_active_ || kaoss_release_ > 0);
        const bool live = perf_lstick_on_ || perf_cstick_on_ || kaoss_live;
        int trk = kaoss_live ? kaoss_track_
                : (song_col_ >= 0 && song_col_ < audio::NUM_TRACKS ? song_col_ : 0);
        const auto& ts = mixer_.track(trk);

        constexpr int CX = 340, CY = 13, BW = 5, BH = 8, GAP = 8;
        // track tag (highlighted while a gesture is live)
        char tb[4];
        std::snprintf(tb, sizeof(tb), "T%d", trk);
        d.text(CX - 14, CY + 1, tb, live ? pal::CURSOR : pal::FG_DIM, 1);

        struct Bar { int v255; Color c; };   // value 0..255 + accent
        int cut = (int)ts.cutoff * 255 / fx::Q15_ONE;
        int res = (int)ts.resonance * 255 / fx::Q15_ONE;
        int snd = (int)(ts.send_del > ts.send_rev ? ts.send_del : ts.send_rev) * 255 / fx::Q15_ONE;
        int bit = (16 - ts.bits) * 255 / 14;             // 16bit=0, 2bit=full
        const Bar bars[4] = {
            { cut, pal::HEADER }, { res, pal::CURSOR },
            { snd, pal::PLAY   }, { bit, pal::RECORD },
        };
        for (int i = 0; i < 4; ++i) {
            int x = CX + i * GAP;
            d.rect(x, CY, BW, BH, pal::BG_HI);                     // well
            int h = bars[i].v255 * BH / 255;
            if (h > BH) h = BH;
            Color c = live ? bars[i].c : lerp_color(pal::BG_HI, bars[i].c, 140);
            if (h > 0) d.rect(x, CY + BH - h, BW, h, c);
        }
    }

    // main content
    switch (screen_) {
        case Screen::Phrase:     draw_phrase(d); break;
        case Screen::Chain:      draw_chain(d); break;
        case Screen::Song:       draw_song(d); break;
        case Screen::Instrument: draw_instrument(d); draw_env_overlay(d); break;
        case Screen::Table:      draw_table(d); break;
        case Screen::Mixer:      draw_mixer(d); break;
        case Screen::Project:    draw_project(d); break;
        default: break;
    }

    // === clone toast ("CLONE 1F" / "BANK FULL") - top-right, fades out ~1.5s ===
    if (clone_msg_frame_ != 0 && clone_msg_[0]) {
        uint32_t cd = frame_ - clone_msg_frame_;
        constexpr uint32_t HOLD = 60, FADE = 30;
        if (cd < HOLD + FADE) {
            uint8_t a = cd < HOLD ? 0xFF : (uint8_t)(0xFF - (cd - HOLD) * 0xFF / FADE);
            int w = (int)std::strlen(clone_msg_) * 6 + 10;
            d.rect(395 - w, 18, w, 12, with_alpha(pal::PANEL, a));
            d.rect(395 - w, 18, 2, 12, with_alpha(pal::CURSOR, a));
            d.text(400 - w, 20, clone_msg_, with_alpha(pal::CURSOR, a), 1);
        } else {
            clone_msg_frame_ = 0;
        }
    }
}

void App::draw_bottom(Draw& d) {
    // bottom 320x240 = top block (info+buttons) + keyboard below
    d.rect(0, 0, 320, 240, pal::BG);
    // DESCRY wordmark doubles as the THEME button (tap opens the picker)
    d.text(8, 6, "DESCRY", pal::FG, 1);
    d.text(52, 6, pal::theme_name(theme_idx), pal::FG_DIM, 1);

    // === theme picker overlay: replaces the whole bottom UI while open ===
    if (theme_menu_) {
        draw_theme_menu(d);
        return;
    }

    // === in-app HELP overlay: full-screen manual, replaces the bottom UI ===
    if (help_on_) {
        draw_help(d);
        return;
    }

    // BPM / GRV / SWING - live values to the right of the title
    {
        char hb[40];
        std::snprintf(hb, sizeof(hb), "BPM%03d GRV%02d SWG%02d",
                      project_.song.bpm, project_.song.groove, project_.song.swing);
        d.text(120, 6, hb, pal::FG_DIM, 1);
        // KEY readout: tap root part = cycle root, tap scale part = cycle scale.
        char kb2[12];
        std::snprintf(kb2, sizeof(kb2), "%s %s",
                      seq::root_name(project_.song.scale_root),
                      seq::scale_name(project_.song.scale_type));
        d.text(244, 6, kb2, project_.song.scale_type ? pal::CURSOR : pal::FG_DIM, 1);
    }

    // === contextual hints removed (user: clean UI) ===
    // quick sample bank status
    {
        auto& s = synth::SampleBank::instance().slot(cur_sample_);
        char sb[40];
        if (s.data.empty()) {
            std::snprintf(sb, sizeof(sb), "S%02d:EMPTY", cur_sample_);
            d.text(8, 18, sb, pal::RECORD);
        } else {
            float sec = s.data.size() / 32000.0f;
            std::snprintf(sb, sizeof(sb), "S%02d:%.1fs", cur_sample_, sec);
            d.text(8, 18, sb, pal::PLAY);
        }
    }

    // === tappable screen tabs (y28..43) - icon + label, tap to jump ===
    {
        constexpr int TAB_Y = 28, TAB_H = 15;
        constexpr int NT = (int)Screen::NUM;   // 7
        static const char* tnames[NT] = {"SONG","CHN","PHR","INST","TBL","MIX","PRJ"};
        for (int i = 0; i < NT; ++i) {
            int x0 = 2 + i * 316 / NT;
            int x1 = 2 + (i + 1) * 316 / NT;
            int w = x1 - x0 - 2;
            bool on = ((int)screen_ == i);
            // tactile tab: gradient face (icons drawn on top)
            ui_button(d, x0, TAB_Y, w, TAB_H,
                      on ? pal::HEADER : pal::BG_HI,
                      on ? pal::HEADER : pal::BG_HI, nullptr, 0, on);
            if (on) {
                // active: icon + label (underline drawn separately - it slides)
                int len = 0; while (tnames[i][len]) ++len;
                int cw = 8 + 3 + len * 6;
                int ix = x0 + (w - cw) / 2;
                draw_screen_icon(d, (Screen)i, ix, TAB_Y + 3, pal::FG);
                d.text(ix + 11, TAB_Y + 4, tnames[i], pal::FG);
            } else {
                // inactive: icon only, dim - clean strip, no text noise
                draw_screen_icon(d, (Screen)i, x0 + (w - 8) / 2, TAB_Y + 3, pal::FG_DIM);
            }
        }
        // sliding underline: mirrors the top nav capsule (ease-out, ~8 frames)
        {
            constexpr uint32_t SLIDE = 8;
            auto tx0 = [&](int i) { return 2 + i * 316 / NT; };
            auto tw  = [&](int i) { return 2 + (i + 1) * 316 / NT - tx0(i) - 2; };
            int ci = (int)screen_;
            int ux = tx0(ci), uw = tw(ci);
            uint32_t sd = frame_ - screen_change_frame_;
            if (screen_change_frame_ != 0 && sd < SLIDE && prev_screen_ != (uint8_t)ci) {
                int px = tx0(prev_screen_), pw = tw(prev_screen_);
                int t = (int)sd;
                int k  = t * (2 * (int)SLIDE - t);      // ease-out
                int kd = (int)SLIDE * (int)SLIDE;
                ux = px + (ux - px) * k / kd;
                uw = pw + (uw - pw) * k / kd;
            }
            d.rect(ux, TAB_Y + TAB_H - 2, uw, 2, pal::CURSOR);
        }
    }

    // === contextual hotkey hints (react live to held modifiers) ===
    // no modifier: line1 = this screen's keys, line2 = global keys.
    // ZL/L/R held: both lines replaced by that modifier's combo map.
    {
        struct H { const char* k; const char* v; };
        // per-screen base hints
        static const H h_song[]  = {{"Y","queue"},{"X","stop q"},{"pad","solo"},{"ZL+SEL","clone"}};
        static const H h_chain[] = {{"A/B","edit"},{"SEL","open phr"},{"ZL+SEL","clone"}};
        static const H h_phr[]   = {{"A/B/X/Y","edit"},{"SEL","prev/fx"},{"START","phrase"}};
        static const H h_inst[]  = {{"A/B/X/Y","edit"},{"ZL+SEL","fx row"}};
        static const H h_tbl[]   = {{"A/B/X/Y","edit"},{"SEL","hear"}};
        static const H h_mix[]   = {{"A/B/X/Y","vol"},{"SEL","mute"}};
        static const H h_prj[]   = {{"A","load"},{"Y","save"},{"X","new"},{"B","del 2x"}};
        static const H h_glob[]  = {{"L/R","view"},{"START","play"},{"ZL/L/R","hold=combos"}};
        // modifier maps
        static const H h_zl[] = {{"X","copy"},{"Y","paste"},{"B","undo"},{"A","redo"},{"SEL","sel/clone"},{"UD","len"}};
        static const H h_l[]      = {{"DPAD","bpm"},{"L/R","prev/next"},{"SEL","scope"}};
        static const H h_l_inst[] = {{"UD","bpm"},{"<>","inst slot"},{"A","clone inst"},{"SEL","scope"}};
        static const H h_r[]     = {{"A","clr cell"},{"B","clr step"},{"UD","groove"},{"LR","swing"}};
        static const H h_r_phr[] = {{"A","clr cell"},{"B","clr step"},{"Y","clr phr"},{"UD","grv"},{"LR","swg"}};
        static const H h_r_tbl[] = {{"A/B","tbl speed"},{"UD","groove"},{"LR","swing"}};

        auto draw_hints = [&](int y, const H* h, int n, ui::Color kc, ui::Color vc) {
            int x = 8;
            for (int i = 0; i < n; ++i) {
                d.text(x, y, h[i].k, kc);
                x += (int)std::strlen(h[i].k) * 6 + 3;
                d.text(x, y, h[i].v, vc);
                x += (int)std::strlen(h[i].v) * 6 + 10;
            }
        };

        if (mod_zl_ || mod_l_ || mod_r_) {
            // modifier held: show its combo map, bright (this is the live cheat)
            const char* tag = mod_zl_ ? "ZL" : (mod_l_ ? "L" : "R");
            // R map is contextual: table screen repurposes R+A/B for table speed,
            // phrase screen adds R+Y = clear whole phrase
            const H* hr = (screen_ == Screen::Table)  ? h_r_tbl
                        : (screen_ == Screen::Phrase) ? h_r_phr : h_r;
            int   hrn  = (screen_ == Screen::Table)  ? 3
                        : (screen_ == Screen::Phrase) ? 5 : 4;
            const H* hl = (screen_ == Screen::Instrument) ? h_l_inst : h_l;
            int   hln  = (screen_ == Screen::Instrument) ? 4 : 3;
            const H* mh = mod_zl_ ? h_zl : (mod_l_ ? hl : hr);
            int  mn = mod_zl_ ? 6 : (mod_l_ ? hln : hrn);
            d.rect(4, 45, 16, 17, pal::CURSOR);
            d.text(7, 50, tag, 0xFF313432);
            int x = 26;
            for (int i = 0; i < mn; ++i) {
                d.text(x, 46, mh[i].k, pal::CURSOR);
                x += (int)std::strlen(mh[i].k) * 6 + 3;
                d.text(x, 46, mh[i].v, pal::FG);
                x += (int)std::strlen(mh[i].v) * 6 + 10;
            }
            d.text(26, 55, "release to cancel", pal::FG_DIM);
        } else {
            const H* sh; int sn;
            switch (screen_) {
                case Screen::Song:       sh = h_song;  sn = 4; break;
                case Screen::Chain:      sh = h_chain; sn = 3; break;
                case Screen::Phrase:     sh = h_phr;   sn = 3; break;
                case Screen::Instrument: sh = h_inst;  sn = 2; break;
                case Screen::Table:      sh = h_tbl;   sn = 2; break;
                case Screen::Mixer:      sh = h_mix;   sn = 2; break;
                default:                 sh = h_prj;   sn = 4; break;
            }
            draw_hints(46, sh, sn, pal::HEADER, pal::FG_DIM);
            draw_hints(55, h_glob, 3, pal::FG_DIM, pal::FG_DIM);
        }
        // "?" badge: tap the hint strip to open the in-app manual
        ui_button(d, 302, 45, 14, 16, pal::BG_HI, pal::HEADER, "?", pal::HEADER);
    }

    // button helper (tactile: gradient + bevel via ui_button)
    auto draw_btn = [&](int x, int y, int w, int h, const char* lbl,
                        ui::Color bg, ui::Color border, bool active = false) {
        ui_button(d, x, y, w, h, bg, border, lbl, pal::FG, active);
    };

    // === OCT -/+ and REC (was y=92, now y=64 - freed up by removing PLAY/SAMP/KIT/RENDER) ===
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "OCT %d", octave_);
        draw_btn(  4, 64,  40, 22, "-", pal::BG_HI, pal::HEADER);
        d.text( 56, 72, buf, pal::FG, 1);
        draw_btn(100, 64,  40, 22, "+", pal::BG_HI, pal::HEADER);

        // KB mode cycle button: KEYS -> PADS -> KAOSS
        {
            const char* kb_lbl = kb_mode_ == KbMode::Keys ? "KEYS"
                               : kb_mode_ == KbMode::Pads ? "PADS" : "KAOS";
            bool kb_alt = kb_mode_ != KbMode::Keys;
            draw_btn(146, 64, 56, 22, kb_lbl,
                     kb_alt ? pal::HEADER : pal::BG_HI,
                     kb_alt ? pal::FG : pal::FG_DIM, kb_alt);
        }

        // REC mode cycle button (JAM / WRT / LIVE) - the label IS the mode
        // indicator now, freeing x264+ for the CLR button (issue #5).
        {
            const char* rec_lbl = rec_mode_ == RecMode::Jam  ? "JAM"
                                : rec_mode_ == RecMode::Write ? "WRT" : "LIVE";
            ui::Color rec_bg     = rec_mode_ == RecMode::Live ? pal::RECORD
                                 : rec_mode_ == RecMode::Write ? pal::HEADER : pal::BG_HI;
            ui::Color rec_border = rec_mode_ == RecMode::Jam ? pal::FG_DIM : pal::FG;
            draw_btn(208, 64, 50, 22, rec_lbl, rec_bg, rec_border, rec_mode_ != RecMode::Jam);
        }

        // CLR: erase the step under the cursor (WRITE) / the playing step (LIVE).
        // dim in JAM mode - nothing to erase when keys don't write.
        {
            bool can_clr = rec_mode_ != RecMode::Jam &&
                           (screen_ == Screen::Phrase || player_.playing());
            draw_btn(264, 64, 52, 22, "CLR",
                     can_clr ? pal::BG_HI : pal::PANEL,
                     can_clr ? pal::RECORD : pal::GRID);
        }
    }

    // on a Sampler instrument the bottom screen hosts tabs + slice/load panels,
    // so hide the scope strip there (same as in the Sample view). same for the
    // DrumKit KB/GEN tab row which occupies the same y-band.
    const bool sampler_inst = (screen_ == Screen::Instrument &&
        (project_.instruments[cur_inst_].type == seq::InstrumentType::Sampler ||
         project_.instruments[cur_inst_].type == seq::InstrumentType::DrumKit));
    // mixer view: the touch faders own the whole band below the transport -
    // the scope strip (y 92..112) would poke out from under them (MF_Y=96)
    const bool mixer_faders = (screen_ == Screen::Mixer && kb_mode_ != KbMode::Kaoss);

    // === SCOPE ===  (strip above the keyboard - filled envelope + beat pulse)
    // hidden when instrument tabs / taller pads occupy the same strip.
    // PADS mode: the scope fits above the pads (y92..112, pads start at 116)
    // so we keep it visible - only mixer faders and sampler tabs still suppress it.
    if (!sampler_inst && !mixer_faders) {
        constexpr int SCO_X = 4;
        constexpr int SCO_Y = 92;
        constexpr int SCO_W = 312;
        constexpr int SCO_H = 20;
        constexpr int MIDY = SCO_Y + SCO_H / 2;
        d.rect(SCO_X, SCO_Y, SCO_W, SCO_H, pal::PANEL);
        // beat pulse: border flashes with the playhead step change
        {
            uint32_t pd = frame_ - step_change_frame_;
            ui::Color bc = (player_.playing() && pd < 6)
                         ? lerp_color(pal::PLAY, pal::GRID, (uint8_t)(pd * 42))
                         : pal::GRID;
            d.rect(SCO_X, SCO_Y - 1, SCO_W, 1, bc);
            d.rect(SCO_X, SCO_Y + SCO_H, SCO_W, 1, bc);
        }
        d.rect(SCO_X, MIDY, SCO_W, 1, lerp_color(pal::PANEL, pal::FG_DIM, 120));
        const auto& mxr = mixer_;
        std::size_t pos = mxr.scope_write_pos;
        int prev_y = MIDY;
        for (int x = 0; x < SCO_W; ++x) {
            std::size_t idx = (pos + audio::Mixer::SCOPE_SIZE - (std::size_t)SCO_W + (std::size_t)x)
                            % audio::Mixer::SCOPE_SIZE;
            int32_t v = mxr.scope[idx];
            int y_off = (v * (SCO_H / 2 - 1)) / 32768;
            int y = MIDY - y_off;
            if (y < SCO_Y) y = SCO_Y;
            if (y >= SCO_Y + SCO_H) y = SCO_Y + SCO_H - 1;
            // filled envelope column (dim glow) + bright connected line on top
            int top = y < MIDY ? y : MIDY;
            int hgt = y < MIDY ? (MIDY - y) : (y - MIDY);
            if (hgt > 0) d.rect(SCO_X + x, top, 1, hgt, pal::PLAY_BG);
            int ly0 = prev_y < y ? prev_y : y;
            int ly1 = prev_y < y ? y : prev_y;
            d.rect(SCO_X + x, ly0, 1, (ly1 - ly0) + 1, pal::PLAY);
            prev_y = y;
        }
    }

    // separator above the keyboard (skip in mixer faders + sampler tabs)
    if (!sampler_inst && !mixer_faders) {
        d.rect(0, KB_Y - 4, 320, 1, pal::HEADER);
    }

    if (screen_ == Screen::Song && rec_mode_ != RecMode::Live && kb_mode_ != KbMode::Kaoss) {
        // === song view: LIVE TRACK PADS v2 ===
        // mute moved to the mixer faders; these pads are now the stage view:
        // tap = SOLO toggle (solo the track, everything else muted).
        // each cell = live dashboard: chain, activity meter, solo/mute state.
        constexpr int PAD_COLS = 4;
        constexpr int PAD_ROWS = 2;
        constexpr int PAD_W = 320 / PAD_COLS;
        constexpr int PAD_H = (240 - KB_Y) / PAD_ROWS;
        for (int t = 0; t < seq::NUM_TRACKS; ++t) {
            int col = t % PAD_COLS;
            int row = t / PAD_COLS;
            int x = col * PAD_W;
            int y = KB_Y + row * PAD_H;
            auto& tr = mixer_.track(t);
            const auto& ts = player_.track_state(t);
            bool playing = ts.playing;
            bool muted = tr.muted;
            bool solo  = (solo_track_ == t);

            ui::Color bg, border;
            if (solo) {
                // soloed: bright header capsule, breathing
                uint8_t br = breathe_pulse(frame_, 48);
                bg = lerp_color(pal::BG_HI, pal::HEADER, (uint8_t)(60 + br / 3));
                border = pal::CURSOR;
            } else if (muted) {
                bg = pal::PANEL; border = pal::GRID;
            } else if (playing && mixer_.primary_voice(t)) {
                bg = pal::BG_HI; border = pal::PLAY;
            } else {
                bg = pal::BG_HI; border = pal::HEADER;
            }
            ui_button(d, x + 2, y + 2, PAD_W - 4, PAD_H - 4, bg, border);

            // track tag + state word
            char tlbl[8];
            std::snprintf(tlbl, sizeof(tlbl), "T%d", t);
            d.text(x + 8, y + 8, tlbl, muted && !solo ? pal::FG_DIM : pal::FG, 2);
            if (solo)       d.text(x + 36, y + 12, "SOLO", pal::CURSOR);
            else if (muted) d.text(x + 36, y + 12, "MUTE", pal::RECORD);
            else if (playing) {
                // chain id being played - real info instead of a static arrow
                char cb[8];
                if (ts.chain_id != seq::EMPTY)
                    std::snprintf(cb, sizeof(cb), "C%02X", ts.chain_id);
                else
                    std::snprintf(cb, sizeof(cb), "PHR");
                d.text(x + 36, y + 12, cb, pal::PLAY);
            }

            // live activity meter (vertical, right edge) - replaces nothing, adds life
            {
                int mh = (int)tr.meter * (PAD_H - 10) / fx::Q15_ONE;
                if (mh > PAD_H - 10) mh = PAD_H - 10;
                if (mh > 0) {
                    ui::Color mc = (tr.meter > fx::Q15_ONE * 9 / 10) ? pal::RECORD : pal::PLAY;
                    d.rect(x + PAD_W - 8, y + PAD_H - 5 - mh, 3, mh, mc);
                }
            }

            // mini cutoff bar (kept - shows kaoss/stick modulation at a glance)
            int cw = (tr.cutoff * (PAD_W - 16)) / fx::Q15_ONE;
            d.rect(x + 6, y + PAD_H - 10, PAD_W - 16, 2, pal::GRID);
            d.rect(x + 6, y + PAD_H - 10, cw, 2, pal::HEADER);
        }
        // hint line in the 2px gap? no room - hints live in the top hint bar
        return;
    }

    // (the old Sample-screen bottom UI was removed - sample editing now lives in
    //  the Instrument view panels: WAVE / SLICE / LOAD / REC)


    // === FX help picker (Phrase view, fx column + SELECT) - replaces the keyboard ===
    if (screen_ == Screen::Phrase && fx_help_) {
        draw_fx_help(d);
        return;
    }

    // === Sampler instrument: KB/WAVE/SLICE/LOAD/REC tab buttons (always shown above the panel) ===
    if (screen_ == Screen::Instrument &&
        project_.instruments[cur_inst_].type == seq::InstrumentType::Sampler) {
        draw_inst_tabs(d);
    }

    // === DrumKit instrument: KB/GEN tab buttons + GEN panel ===
    if (screen_ == Screen::Instrument &&
        project_.instruments[cur_inst_].type == seq::InstrumentType::DrumKit) {
        draw_kit_tabs(d);
        if (kit_panel_ == KitPanel::Gen) {
            draw_gen_panel(d);
            return;
        }
    }

    // === bottom panels (Instrument view, Sampler) - selected via inst_panel_ ===
    if (screen_ == Screen::Instrument &&
        project_.instruments[cur_inst_].type == seq::InstrumentType::Sampler &&
        inst_panel_ != InstPanel::Kb) {
        int slot = project_.instruments[cur_inst_].sampler.sample_slot;
        switch (inst_panel_) {
            case InstPanel::Wave:  draw_wave_panel(d, slot);  break;
            case InstPanel::Slice: draw_slice_panel(d, slot); break;
            case InstPanel::Load:  draw_load_panel(d, slot);  break;
            case InstPanel::Rec:   draw_rec_panel(d, slot);   break;
            default: break;
        }
        return;
    }
    // === Mixer view: bottom screen = touch faders (9 strips) ===
    if (screen_ == Screen::Mixer && kb_mode_ != KbMode::Kaoss) {
        draw_mixer_faders(d);
        return;
    }

    // === KAOSS pad - XY performance field instead of the keyboard ===
    if (kb_mode_ == KbMode::Kaoss) {
        draw_kaoss(d);
        return;
    }

    // === KEYBOARD / PADS - cycled via kb_mode_ (KEYS/PADS/KAOS button) ===
    // collect notes currently sounding on playing tracks - to light them up live
    auto note_is_playing = [&](int n) -> bool {
        if (!player_.playing()) return false;
        for (int t = 0; t < seq::NUM_TRACKS; ++t) {
            const auto& tst = player_.track_state(t);
            if (tst.playing && (int)tst.last_note == n) return true;
        }
        return false;
    };

    if (kb_mode_ == KbMode::Pads) {
        // === 4x4 performance pads (MPC-style, tall band from y=PADS_Y) ===
        // chromatic 16 notes from octave_; on a DrumKit instrument each pad shows
        // its sample slot and lights up when filled.
        const auto& inst = project_.instruments[cur_inst_];
        const bool kit = (inst.type == seq::InstrumentType::DrumKit);
        // on a kit, pads follow base_note so pad 0 == kit pad 0
        const int base = kit ? inst.drumkit.base_note : octave_ * 12;
        // scale mode (non-kit): pads are consecutive SCALE DEGREES from the root.
        // precompute the 16 notes with the same walk touch_note_at uses.
        const uint8_t sc_t = project_.song.scale_type, sc_r = project_.song.scale_root;
        int deg_note[16];
        if (!kit && sc_t != 0) {
            int n = seq::scale_snap(sc_t, sc_r, octave_ * 12 + (sc_r % 12));
            for (int i = 0; i < 16; ++i) {
                deg_note[i] = (n <= 127) ? n : -1;
                n = seq::scale_step(sc_t, sc_r, n, +1);
            }
        }

        d.rect(KB_X, PADS_Y, KB_W, PADS_H, pal::PANEL);
        for (int p = 0; p < 16; ++p) {
            int col = p % PADS_COLS;
            int row = PADS_ROWS - 1 - (p / PADS_COLS);   // pad 0 = bottom-left
            int x = KB_X + col * pad_cell_w();
            int y = PADS_Y + row * pad_cell_h();
            int w = pad_cell_w() - 3, h = pad_cell_h() - 3;
            int note = (!kit && sc_t != 0) ? deg_note[p] : base + p;

            bool held = (note == touch_held_note_) || note_is_playing(note);
            // press flash: bright pop on hit that decays over ~8 frames
            uint8_t flash = 0;
            if (note == last_kb_note_ && pad_flash_frame_ > 0) {
                uint32_t fd = frame_ - pad_flash_frame_;
                if (fd < 8) flash = (uint8_t)(255 - fd * 32);
            }
            bool filled = true;      // synth pads are always "live"
            char lbl[8];
            if (kit) {
                uint8_t slot = inst.drumkit.slots[p];
                filled = (slot != 0xFF && slot < synth::SAMPLE_BANK_SIZE &&
                          !synth::SampleBank::instance().slot(slot).empty());
                if (slot == 0xFF) std::snprintf(lbl, sizeof(lbl), "--");
                else              std::snprintf(lbl, sizeof(lbl), "%02d", slot);
            } else if (note < 0) {
                filled = false;
                std::snprintf(lbl, sizeof(lbl), "--");
            } else {
                Draw::note_str((uint8_t)note, lbl);
            }

            // pad body: rubbery gradient, warm tint when held, flash overlay on hit
            ui::Color top = held ? 0xFFE8A8C8 : (filled ? 0xFF4A4E4C : 0xFF303432);
            ui::Color bot = held ? 0xFFC07090 : (filled ? 0xFF262A28 : 0xFF1A1E1C);
            if (flash && !held) {
                top = lerp_color(top, 0xFFE8A8C8, flash);
                bot = lerp_color(bot, 0xFFC07090, flash);
            }
            constexpr int STEP = 4;
            for (int yy = 0; yy < h; yy += STEP) {
                int bandh = (yy + STEP <= h) ? STEP : (h - yy);
                uint8_t t = (uint8_t)(yy * 255 / (h - 1));
                d.rect(x + 1, y + 1 + yy, w - 2, bandh, lerp_color(top, bot, t));
            }
            // edge: top catch + bottom lip
            d.rect(x + 1, y + 1, w - 2, 1, held ? 0xFFFFB0D8 : 0xFF60646A);
            d.rect(x + 1, y + h - 2, w - 2, 2, 0xFF000004);
            // label bottom-left; note number for kits top-right
            d.text(x + 5, y + h - 12, lbl, held ? 0xFF3A1028 : (filled ? pal::FG : pal::FG_DIM));
            if (kit) {
                char nn[4];
                std::snprintf(nn, sizeof(nn), "%X", p);
                d.text(x + w - 10, y + 4, nn, held ? 0xFF3A1028 : pal::FG_DIM);
            }
            // velocity hint: thin bar on the left edge showing where Y maps loud/soft
            if (held) {
                int vh = touch_vel_ * (h - 4) / 127;
                d.rect(x + 2, y + h - 2 - vh, 2, vh, 0xFFFFB0D8);
            }
        }
        return;
    }

    // === KEYBOARD - white keys (hardware look: top highlight, body gradient, base lip) ===
    // base under the whole keyboard - the "corpus" the keys sit in
    d.rect(KB_X, KB_Y, KB_W, KB_H, pal::PANEL);

    for (int i = 0; i < WHITE_KEYS; ++i) {
        int x = white_key_x(i);
        int w = white_key_w(i);
        int oct = i / 7;
        int note = (octave_ + oct) * 12 + white_to_semi[i % 7];
        bool held = (note == touch_held_note_) || note_is_playing(note);
        bool last = (note == last_kb_note_);

        int kx = x + 1, kw = w - 2;
        int ky = KB_Y, kh = KB_H - 1;

        // body vertical gradient: ivory at top -> slightly grey at the bottom
        ui::Color top = 0xFFF4F4EC;
        ui::Color bot = 0xFFB8B8C0;
        if (held) { top = 0xFFE8A8C8; bot = 0xFFC07090; }   // pressed: warm tint, darker
        else if (last) { top = 0xFFF0D8E4; bot = 0xFFC8B0BC; }
        // stepped gradient (bands of 4px) - cheap, looks smooth enough at this size
        constexpr int STEP = 4;
        for (int yy = 0; yy < kh; yy += STEP) {
            int bandh = (yy + STEP <= kh) ? STEP : (kh - yy);
            uint8_t t = (uint8_t)(yy * 255 / (kh - 1));
            d.rect(kx, ky + yy, kw, bandh, lerp_color(top, bot, t));
        }
        // top highlight (light catches the top edge)
        d.rect(kx, ky, kw, 1, held ? 0xFFC890A8 : 0xFFFFFFFF);
        // left bevel (lighter) + right shadow (the seam to the next key)
        d.rect(kx, ky, 1, kh, held ? 0xFFD098B0 : 0xFFFFFFFF);
        d.rect(kx + kw - 1, ky, 1, kh, 0xFF60606A);
        // base lip - a darker band where the key meets the corpus
        d.rect(kx, ky + kh - 3, kw, 3, held ? 0xFF905068 : 0xFF888892);
        d.rect(kx, ky + kh - 1, kw, 1, 0xFF404048);
        // pressed keys sink: a shadow across the very top instead of highlight
        if (held) d.rect(kx, ky, kw, 2, 0xFF7A4058);

        // label the C key
        if (i % 7 == 0) {
            char nlbl[4];
            std::snprintf(nlbl, sizeof(nlbl), "C%d", octave_ + oct);
            d.text(x + 4, KB_Y + KB_H - 12, nlbl, 0xFF55555E);
        }
        // scale marker: dot above the base lip on in-scale keys (root = accent ring)
        if (project_.song.scale_type) {
            const uint8_t st = project_.song.scale_type, sr = project_.song.scale_root;
            if (seq::scale_has(st, sr, note)) {
                bool is_root = (note % 12) == (sr % 12);
                ui::Color mc = is_root ? pal::CURSOR : 0xFF9090A0;
                d.rect(kx + kw / 2 - 1, ky + kh - 8, 3, 3, mc);
                if (is_root) d.rect(kx + kw / 2 - 2, ky + kh - 9, 5, 1, mc);
            }
        }
    }
    // === black keys on top (glossy: gradient + left specular + rounded base shadow) ===
    for (int oct = 0; oct < 2; ++oct) {
        for (int b = 0; b < 5; ++b) {
            int wh = oct * 7 + black_after_white[b];
            int cx = white_key_x(wh + 1);          // seam between two white keys
            int bw = white_key_w(wh) * 6 / 10;
            int bh = KB_H * 6 / 10;
            int note = (octave_ + oct) * 12 + black_to_semi[b];
            bool held = (note == touch_held_note_) || note_is_playing(note);
            bool last = (note == last_kb_note_);
            int bx = cx - bw / 2;

            // gradient body: charcoal -> near-black
            ui::Color top = 0xFF3A3A44;
            ui::Color bot = 0xFF121218;
            if (held) { top = 0xFFD86098; bot = 0xFF803058; }
            else if (last) { top = 0xFF5A4452; bot = 0xFF201822; }
            constexpr int BSTEP = 3;
            for (int yy = 0; yy < bh; yy += BSTEP) {
                int bandh = (yy + BSTEP <= bh) ? BSTEP : (bh - yy);
                uint8_t t = (uint8_t)(yy * 255 / (bh - 1));
                d.rect(bx, KB_Y + yy, bw, bandh, lerp_color(top, bot, t));
            }
            // left specular highlight (a thin glossy reflection)
            d.rect(bx, KB_Y, 1, bh - 2, held ? 0xFFF0A0C8 : 0xFF606070);
            // top edge catch
            d.rect(bx, KB_Y, bw, 1, held ? 0xFFFFB0D8 : 0xFF505060);
            // rounded base: a brighter front lip then a dark drop shadow under it
            d.rect(bx, KB_Y + bh - 3, bw, 1, held ? 0xFFB04878 : 0xFF2A2A34);
            d.rect(bx, KB_Y + bh - 2, bw, 2, 0xFF000004);
            // scale marker on in-scale black keys
            if (project_.song.scale_type &&
                seq::scale_has(project_.song.scale_type, project_.song.scale_root, note)) {
                bool is_root = (note % 12) == (project_.song.scale_root % 12);
                d.rect(bx + bw / 2 - 1, KB_Y + bh - 8, 3, 3,
                       is_root ? pal::CURSOR : 0xFF888898);
            }
        }
    }
}

// === theme picker overlay ===
// a row per theme: name + live swatch strip (bg / play / cursor / fg / header).
// tap a row = apply instantly (main persists it), tap outside = close.
namespace {
    constexpr int THM_X = 32, THM_W = 256;
    constexpr int THM_ROW_H = 26, THM_HDR = 18;
}

void App::draw_theme_menu(Draw& d) {
    const int n = pal::theme_count();
    const int ph = THM_HDR + n * THM_ROW_H + 6;
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
}

bool App::theme_menu_touch(int x, int y) {
    const int n = pal::theme_count();
    const int ph = THM_HDR + n * THM_ROW_H + 6;
    const int py = (240 - ph) / 2;
    if (x < THM_X || x >= THM_X + THM_W || y < py || y >= py + ph) {
        theme_menu_ = false;    // tap outside = close
        return true;
    }
    int row = (y - py - THM_HDR) / THM_ROW_H;
    if (row >= 0 && row < n) {
        set_theme(row);         // apply instantly; main.cpp persists the change
        theme_menu_ = false;
    }
    return true;
}

void App::tick() {
    // animation layer: global frame counter for trail/flash/pulses
    ++frame_;

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

    // stick modulation release: ramp the grabbed tracks back to baseline
    // (mirror of the kaoss release; sticks can grab ALL tracks too)
    if (!stick_was_on_ && stick_release_ > 0) {
        const int n = stick_release_;
        auto step_q15 = [n](fx::q15& v, fx::q15 target) {
            v = (fx::q15)((int)v + ((int)target - (int)v) / n);
        };
        for (int t = 0; t < 8; ++t) {
            if (!(stick_mask_ & (1u << t))) continue;
            auto& ts = mixer_.track(t);
            const auto& base = stick_bases_[t];
            step_q15(ts.cutoff,    base.cutoff);
            step_q15(ts.resonance, base.resonance);
            step_q15(ts.send_del,  base.send_del);
            step_q15(ts.send_rev,  base.send_rev);
            step_q15(ts.mg_rate,   base.mg_rate);
            step_q15(ts.mix_vol,   base.mix_vol);
            step_q15(ts.pan,       base.pan);
            ts.mg_to_cutoff = (int16_t)(ts.mg_to_cutoff
                             + ((int)base.mg_to_cutoff - (int)ts.mg_to_cutoff) / n);
            ts.mg_to_vca    = (int16_t)(ts.mg_to_vca
                             + ((int)base.mg_to_vca - (int)ts.mg_to_vca) / n);
            ts.bits       = (uint8_t)((int)ts.bits + ((int)base.bits - (int)ts.bits) / n);
            ts.downsample = (uint8_t)((int)ts.downsample
                             + ((int)base.downsample - (int)ts.downsample) / n);
        }
        --stick_release_;
        if (stick_release_ == 0) {
            // ramp finished: hand the params back to the sequencer (mirror of kaoss)
            for (int t = 0; t < 8; ++t)
                if (stick_mask_ & (1u << t)) mixer_.track(t).perf_hold = 0;
        }
    }

    // detect playhead step change - for the pulse on each new step.
    // any playing track counts (the header PLAY dot beats on every screen);
    // track 0 preferred so the pulse follows the main groove.
    int ps = -1;
    if (player_.playing()) {
        for (int t = 0; t < seq::NUM_TRACKS; ++t) {
            if (player_.track_state(t).playing) { ps = player_.track_state(t).step; break; }
        }
    }
    if (ps != last_playing_step_) {
        last_playing_step_ = ps;
        if (ps >= 0) step_change_frame_ = frame_;
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
