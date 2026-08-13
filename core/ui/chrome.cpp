// App: top chrome, bottom status/hints, performance controls and keyboard.
#include "app.h"
#include "ui_internal.h"
#include "../sequencer/scale.h"
#include "../synth/sampler.h"
#include <cstdio>
#include <cstring>

namespace trackr::ui {

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

    // === unsaved-changes dot ===
    // the dirty flag used to be visible ONLY on the project screen, i.e. you had
    // to leave what you were doing to learn whether it was saved. one dot in the
    // always-on header answers it from every view. breathes so it reads as a state,
    // not as decoration.
    if (project_dirty()) {
        uint8_t br = breathe_pulse(frame_, 72);
        Color dc = lerp_color(with_alpha(pal::CURSOR, 120), pal::CURSOR, br);
        d.rect(346, 5, 4, 4, dc);
    }

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
    // Passive chrome must sit behind the controls, not glow like an active state.
    // Derive it from the live theme so all colorways keep the same hierarchy.
    const Color quiet_frame = lerp_color(pal::BG_HI, pal::HEADER, 72);
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
        auto& bank = synth::SampleBank::instance();
        auto& s = bank.slot(cur_sample_);
        char sb[64];
        const unsigned ram_mb10 = (unsigned)(bank.bytes_used() * 10 / (1024 * 1024));
        if (s.data.empty()) {
            std::snprintf(sb, sizeof(sb), "S%02d:EMPTY  RAM %u.%u/32M", cur_sample_, ram_mb10 / 10, ram_mb10 % 10);
            d.text(8, 18, sb, pal::RECORD);
        } else {
            float sec = s.num_frames() / (float)synth::SAMPLER_SR;
            if (s.name[0]) std::snprintf(sb, sizeof(sb), "S%02d:%.18s  %u.%uM", cur_sample_, s.name, ram_mb10 / 10, ram_mb10 % 10);
            else           std::snprintf(sb, sizeof(sb), "S%02d:%.1fs  RAM %u.%u/32M", cur_sample_, sec, ram_mb10 / 10, ram_mb10 % 10);
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
        static const H h_prj[]   = {{"A","load"},{"Y","save"},{"X","new"},{"B","del 2x"},{"SEL","wav"}};
        static const H h_glob[]  = {{"L/R","view"},{"START","play"},{"ZL/L/R","hold=combos"}};
        // modifier maps
        static const H h_zl[] = {{"X","copy"},{"Y","paste"},{"B","undo"},{"A","redo"},{"SEL","sel/clone"},{"UD","len"}};
        static const H h_l[]      = {{"DPAD","bpm"},{"L/R","prev/next"},{"SEL","scope"}};
        static const H h_l_inst[] = {{"UD","bpm"},{"<>","inst slot"},{"A","clone inst"},{"SEL","scope"}};
        static const H h_r[]     = {{"A","clr cell"},{"B","clr step"},{"UD","groove"},{"LR","swing"}};
        static const H h_r_phr[] = { {"A","cell"}, {"B","step"}, {"X","tools"}, {"Y","phrase"} };
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
            // phrase adds R+X tools and R+Y phrase clear; global groove/swing
            // still work, but the four destructive/edit actions get the scarce hint row.
            const H* hr = (screen_ == Screen::Table)  ? h_r_tbl
                        : (screen_ == Screen::Phrase) ? h_r_phr : h_r;
            int   hrn  = (screen_ == Screen::Table)  ? 3
                        : (screen_ == Screen::Phrase) ? 4 : 4;
            const H* hl = (screen_ == Screen::Instrument) ? h_l_inst : h_l;
            int   hln  = (screen_ == Screen::Instrument) ? 4 : 3;
            const H* mh = mod_zl_ ? h_zl : (mod_l_ ? hl : hr);
            int  mn = mod_zl_ ? 6 : (mod_l_ ? hln : hrn);
            d.rect(4, 45, 16, 17, pal::CURSOR);
            d.text(7, 50, tag, pal::BG);   // knockout on the cursor chip (was a fixed cretaceous grey)
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
                default:                 sh = h_prj;   sn = 5; break;
            }
            draw_hints(46, sh, sn, pal::HEADER, pal::FG_DIM);
            draw_hints(55, h_glob, 3, pal::FG_DIM, pal::FG_DIM);
        }
        // "?" badge: tap the hint strip to open the in-app manual
        ui_button(d, 302, 45, 14, 16, pal::BG_HI, quiet_frame, "?", pal::HEADER);
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
        draw_btn(  4, 64,  40, 22, "-", pal::BG_HI, quiet_frame);
        d.text( 56, 72, buf, pal::FG, 1);
        draw_btn(100, 64,  40, 22, "+", pal::BG_HI, quiet_frame);

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

    // === SCOPE ===  (compact strip above the keyboard)
    // Keep this one deliberately boring: the fullscreen scope is the performance
    // visualizer. This strip is only a quick signal check and must not pulse/glow.
    // PADS start at y=116, so all keyboard modes share the same 20px geometry.
    if (!sampler_inst && !mixer_faders) {
        constexpr int SCO_X = 4;
        constexpr int SCO_Y = 92;
        constexpr int SCO_W = 312;
        constexpr int SCO_H = 20;
        constexpr int MIDY = SCO_Y + SCO_H / 2;
        d.rect(SCO_X, SCO_Y, SCO_W, SCO_H, pal::PANEL);
        d.rect(SCO_X, SCO_Y - 1, SCO_W, 1, pal::GRID);
        d.rect(SCO_X, SCO_Y + SCO_H, SCO_W, 1, pal::GRID);
        if (scope_style_ != 2)
            d.rect(SCO_X, MIDY, SCO_W, 1, lerp_color(pal::PANEL, pal::FG_DIM, 120));
        draw_master_scope(d, SCO_X, SCO_Y + 1, SCO_W, SCO_H - 2);
    }

    // separator above the keyboard (skip in mixer faders + sampler tabs)
    if (!sampler_inst && !mixer_faders) {
        d.rect(0, KB_Y - 4, 320, 1, pal::GRID);
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
                bg = pal::BG_HI; border = quiet_frame;
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


    // === Phrase tools / FX help pickers replace the keyboard ===
    if (screen_ == Screen::Phrase && phrase_tools_on_) {
        draw_phrase_tools(d);
        return;
    }
    if (screen_ == Screen::Phrase && fx_help_) {
        draw_fx_help(d);
        return;
    }

    // === Sampler instrument: KB/WAVE/SLICE/LOAD/REC tabs ===
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

    // === bottom panels (Instrument view, Sampler) ===
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
    // === themed key/pad colors ===
    // these used to be hardcoded pink-and-ivory hexes, so the keyboard and the
    // pads stayed cretaceous-rose on EVERY theme (vapor/ember/frost included).
    // derive them from the live palette instead - computed per frame, never cached
    // in statics (see the rule in draw.h).
    const Color KEY_TOP   = lerp_color(pal::FG, 0xFFFFFFFF, 150);   // ivory face, theme-tinted
    const Color KEY_BOT   = lerp_color(pal::FG, 0xFF000000, 90);    // shaded lower body
    const Color KEY_EDGE  = lerp_color(pal::FG, 0xFFFFFFFF, 210);   // top/left light catch
    const Color KEY_SEAM  = lerp_color(pal::BG, 0xFF000000, 60);    // seam to the next key
    const Color KEY_LIP   = lerp_color(pal::FG, 0xFF000000, 130);   // base lip
    const Color KEY_LIP2  = lerp_color(pal::BG, 0xFF000000, 90);
    const Color KEY_LBL   = lerp_color(pal::BG, pal::FG_DIM, 90);   // "C4" on a white key
    const Color BKEY_TOP  = lerp_color(pal::PANEL, pal::FG, 60);    // black key body
    const Color BKEY_BOT  = lerp_color(pal::PANEL, 0xFF000000, 40);
    const Color BKEY_SPEC = lerp_color(pal::PANEL, pal::FG, 110);   // glossy left edge
    // pressed state: the theme's cursor accent, light on top / saturated below
    const Color HIT_TOP   = lerp_color(pal::CURSOR, 0xFFFFFFFF, 90);
    const Color HIT_BOT   = lerp_color(pal::CURSOR, 0xFF000000, 70);
    const Color HIT_EDGE  = lerp_color(pal::CURSOR, 0xFFFFFFFF, 170);
    const Color HIT_SINK  = lerp_color(pal::CURSOR, 0xFF000000, 120);
    const Color HIT_LBL   = lerp_color(pal::CURSOR, 0xFF000000, 190);
    // "last note" ghost: halfway between the face and the hit tint
    const Color LAST_TOP  = lerp_color(KEY_TOP, HIT_TOP, 70);
    const Color LAST_BOT  = lerp_color(KEY_BOT, HIT_BOT, 70);
    const Color SCALE_DOT = lerp_color(pal::BG, pal::FG, 150);

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

            // pad body: rubbery gradient, accent tint when held, flash overlay on hit
            const Color PAD_TOP  = lerp_color(pal::BG, pal::FG, 55);   // filled pad face
            const Color PAD_BOT  = lerp_color(pal::BG, 0xFF000000, 45);
            const Color PADE_TOP = lerp_color(pal::BG, pal::GRID, 120); // empty pad face
            const Color PADE_BOT = lerp_color(pal::PANEL, pal::BG, 120);
            ui::Color top = held ? HIT_TOP : (filled ? PAD_TOP : PADE_TOP);
            ui::Color bot = held ? HIT_BOT : (filled ? PAD_BOT : PADE_BOT);
            if (flash && !held) {
                top = lerp_color(top, HIT_TOP, flash);
                bot = lerp_color(bot, HIT_BOT, flash);
            }
            constexpr int STEP = 4;
            for (int yy = 0; yy < h; yy += STEP) {
                int bandh = (yy + STEP <= h) ? STEP : (h - yy);
                uint8_t t = (uint8_t)(yy * 255 / (h - 1));
                d.rect(x + 1, y + 1 + yy, w - 2, bandh, lerp_color(top, bot, t));
            }
            // edge: top catch + bottom lip
            d.rect(x + 1, y + 1, w - 2, 1, held ? HIT_EDGE : lerp_color(pal::GRID, pal::FG, 60));
            d.rect(x + 1, y + h - 2, w - 2, 2, lerp_color(pal::PANEL, 0xFF000000, 120));
            // label bottom-left; note number for kits top-right
            d.text(x + 5, y + h - 12, lbl, held ? HIT_LBL : (filled ? pal::FG : pal::FG_DIM));
            if (kit) {
                char nn[4];
                std::snprintf(nn, sizeof(nn), "%X", p);
                d.text(x + w - 10, y + 4, nn, held ? HIT_LBL : pal::FG_DIM);
            }
            // velocity hint: thin bar on the left edge showing where Y maps loud/soft
            if (held) {
                int vh = touch_vel_ * (h - 4) / 127;
                d.rect(x + 2, y + h - 2 - vh, 2, vh, HIT_EDGE);
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
        ui::Color top = KEY_TOP;
        ui::Color bot = KEY_BOT;
        if (held) { top = HIT_TOP; bot = HIT_BOT; }          // pressed: accent tint, darker
        else if (last) { top = LAST_TOP; bot = LAST_BOT; }
        // stepped gradient (bands of 4px) - cheap, looks smooth enough at this size
        constexpr int STEP = 4;
        for (int yy = 0; yy < kh; yy += STEP) {
            int bandh = (yy + STEP <= kh) ? STEP : (kh - yy);
            uint8_t t = (uint8_t)(yy * 255 / (kh - 1));
            d.rect(kx, ky + yy, kw, bandh, lerp_color(top, bot, t));
        }
        // top highlight (light catches the top edge)
        d.rect(kx, ky, kw, 1, held ? HIT_EDGE : KEY_EDGE);
        // left bevel (lighter) + right shadow (the seam to the next key)
        d.rect(kx, ky, 1, kh, held ? HIT_EDGE : KEY_EDGE);
        d.rect(kx + kw - 1, ky, 1, kh, KEY_SEAM);
        // base lip - a darker band where the key meets the corpus
        d.rect(kx, ky + kh - 3, kw, 3, held ? HIT_SINK : KEY_LIP);
        d.rect(kx, ky + kh - 1, kw, 1, KEY_LIP2);
        // pressed keys sink: a shadow across the very top instead of highlight
        if (held) d.rect(kx, ky, kw, 2, HIT_SINK);

        // label the C key
        if (i % 7 == 0) {
            char nlbl[4];
            std::snprintf(nlbl, sizeof(nlbl), "C%d", octave_ + oct);
            d.text(x + 4, KB_Y + KB_H - 12, nlbl, KEY_LBL);
        }
        // scale marker: dot above the base lip on in-scale keys (root = accent ring)
        if (project_.song.scale_type) {
            const uint8_t st = project_.song.scale_type, sr = project_.song.scale_root;
            if (seq::scale_has(st, sr, note)) {
                bool is_root = (note % 12) == (sr % 12);
                ui::Color mc = is_root ? pal::CURSOR : lerp_color(KEY_BOT, pal::BG, 120);
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

            // gradient body: charcoal -> near-black (theme-tinted)
            ui::Color top = BKEY_TOP;
            ui::Color bot = BKEY_BOT;
            if (held) { top = lerp_color(pal::CURSOR, 0xFF000000, 40); bot = HIT_SINK; }
            else if (last) { top = lerp_color(BKEY_TOP, pal::CURSOR, 80); bot = BKEY_BOT; }
            constexpr int BSTEP = 3;
            for (int yy = 0; yy < bh; yy += BSTEP) {
                int bandh = (yy + BSTEP <= bh) ? BSTEP : (bh - yy);
                uint8_t t = (uint8_t)(yy * 255 / (bh - 1));
                d.rect(bx, KB_Y + yy, bw, bandh, lerp_color(top, bot, t));
            }
            // left specular highlight (a thin glossy reflection)
            d.rect(bx, KB_Y, 1, bh - 2, held ? HIT_EDGE : BKEY_SPEC);
            // top edge catch
            d.rect(bx, KB_Y, bw, 1, held ? HIT_EDGE : BKEY_SPEC);
            // rounded base: a brighter front lip then a dark drop shadow under it
            d.rect(bx, KB_Y + bh - 3, bw, 1, held ? HIT_SINK : lerp_color(BKEY_TOP, pal::PANEL, 120));
            d.rect(bx, KB_Y + bh - 2, bw, 2, lerp_color(pal::PANEL, 0xFF000000, 120));
            // scale marker on in-scale black keys
            if (project_.song.scale_type &&
                seq::scale_has(project_.song.scale_type, project_.song.scale_root, note)) {
                bool is_root = (note % 12) == (project_.song.scale_root % 12);
                d.rect(bx + bw / 2 - 1, KB_Y + bh - 8, 3, 3,
                       is_root ? pal::CURSOR : SCALE_DOT);
            }
        }
    }
}

} // namespace trackr::ui
