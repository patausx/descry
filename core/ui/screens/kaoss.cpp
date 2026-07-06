// App: KAOSS pad — XY touch performance controller (dsn-12 heritage, m8 has nothing like it).
// The keyboard band of the bottom screen becomes an XY field; dragging writes into the
// target track's mixer DSP through the SAME path as the live stick modulation, so the
// audio engine needs zero changes. On release the grabbed params ramp back to the
// baseline captured at gesture start (no stuck closed filters).
//
// Layout (inside the KB band, works in every context where the keyboard would show):
//   [left column 38px: X-assign btn / Y-assign btn] [XY field 280x100]
// Tapping the X or Y button opens a popup grid over the field with all available
// destinations (CUT/RES/DEL/REV/BIT/DSM/RAT/M>C/M>V/VOL/PAN); tap one to assign.
#include "../app.h"
#include "../ui_internal.h"
#include "../../audio/fixed.h"
#include <cstdio>

namespace trackr::ui {

namespace {
    // left column: X / Y assign buttons + TRK/ALL target toggle + STK sync toggle
    constexpr int AX_W = 38;
    constexpr int AX_H = (KB_H - 36) / 2;             // 32px per assign button
    constexpr int TGT_H = 18;                         // TRK/ALL strip
    constexpr int STK_H = 18;                         // stick sync strip
    // XY field
    constexpr int KF_X = AX_W + 2;                    // 40
    constexpr int KF_Y = KB_Y;
    constexpr int KF_W = KB_W - KF_X;                 // 280
    constexpr int KF_H = KB_H;                        // 100


    inline Color field_grid() { return lerp_color(pal::PANEL, pal::GRID, 80); }

    // popup menu grid geometry (drawn over the field)
    constexpr int MENU_COLS = 4;
    constexpr int MENU_CW   = 66;
    constexpr int MENU_CH   = 26;
    constexpr int MENU_X    = KF_X + 8;
    constexpr int MENU_Y    = KF_Y + 14;
}

const char* App::kaoss_dest_name(uint8_t dst) {
    static const char* n[(int)KaossDest::COUNT] = {
        "CUT", "RES", "DEL", "REV", "BIT", "DSM", "RAT", "M>C", "M>V", "VOL", "PAN",
    };
    return (dst < (int)KaossDest::COUNT) ? n[dst] : "???";
}

// write ONE destination on ONE track from a normalized 0..1000 value.
// shared by both axes so X and Y can carry any destination each.
void App::apply_kaoss_dest(int trk, uint8_t dest, int v) {
    auto& ts = mixer_.track(trk);
    switch ((KaossDest)dest) {
        case KaossDest::Cut:
            ts.cutoff = (fx::q15)(v * fx::Q15_ONE / 1000);
            break;
        case KaossDest::Res:
            // cap at 0.9*ONE like the stick path - full Q self-oscillates too eagerly
            ts.resonance = (fx::q15)(v * (fx::Q15_ONE * 90 / 100) / 1000);
            break;
        case KaossDest::Del:
            ts.send_del = (fx::q15)(v * fx::Q15_ONE / 1000);
            break;
        case KaossDest::Rev:
            ts.send_rev = (fx::q15)(v * fx::Q15_ONE / 1000);
            break;
        case KaossDest::Bit: {
            // low end = clean, high = crushed. bit-mask crush is only audible below
            // ~8 bits, so spend most travel there (same curve as the C-stick).
            int bits = (v < 300) ? 16 - v * 8 / 300
                                 : 8 - (v - 300) * 6 / 700;
            if (bits < 2) bits = 2; if (bits > 16) bits = 16;
            ts.bits = (uint8_t)bits;
            break;
        }
        case KaossDest::Dsm:
            ts.downsample = (uint8_t)(1 + v * 5 / 1000);   // 1..6
            break;
        case KaossDest::Rat:
            ts.mg_rate = (fx::q15)(v * fx::Q15_ONE / 1000);
            break;
        case KaossDest::Mgc:
            // signed depth: center = 0, up = positive wobble, down = inverted
            ts.mg_to_cutoff = (int16_t)((v - 500) * 32767 / 500);
            break;
        case KaossDest::Mgv:
            ts.mg_to_vca = (int16_t)((v - 500) * 32767 / 500);
            break;
        case KaossDest::Vol:
            ts.mix_vol = (fx::q15)(v * fx::Q15_ONE / 1000);
            break;
        case KaossDest::Pan:
            // signed: center = C, left/right = hard pan
            ts.pan = (fx::q15)((v - 500) * fx::Q15_ONE / 500);
            break;
        default: break;
    }
}

// XY (0..1000) -> both assigned destinations, on every grabbed track.
void App::apply_kaoss() {
    for (int t = 0; t < audio::NUM_TRACKS; ++t) {
        if (!(kaoss_grab_mask_ & (1u << t))) continue;
        apply_kaoss_dest(t, (uint8_t)kaoss_dest_x_, kaoss_x_);
        apply_kaoss_dest(t, (uint8_t)kaoss_dest_y_, kaoss_y_);
    }
    dirty = true;
}

// capture one track's baseline into kaoss_base_[trk]
void App::kaoss_snapshot(int trk) {
    auto& ts = mixer_.track(trk);
    kaoss_base_[trk] = { ts.cutoff, ts.resonance, ts.send_del, ts.send_rev,
                         ts.mg_rate, ts.mix_vol, ts.pan,
                         ts.mg_to_cutoff, ts.mg_to_vca, ts.bits, ts.downsample };
}

// touch entry (press + move). handles assign buttons, the popup menu and the field.
void App::kaoss_touch(int x, int y, bool is_move) {
    // === popup menu open: it owns all touches (press only) ===
    if (kaoss_menu_ != 0) {
        if (is_move) return;
        const int n = (int)KaossDest::COUNT;
        for (int i = 0; i < n; ++i) {
            int col = i % MENU_COLS, row = i / MENU_COLS;
            int bx = MENU_X + col * MENU_CW;
            int by = MENU_Y + row * MENU_CH;
            if (x >= bx && x < bx + MENU_CW - 4 && y >= by && y < by + MENU_CH - 4) {
                if (kaoss_menu_ == 1) kaoss_dest_x_ = (KaossDest)i;
                else                  kaoss_dest_y_ = (KaossDest)i;
                kaoss_menu_ = 0;
                mark_dirty();
                return;
            }
        }
        kaoss_menu_ = 0;   // tap outside the grid = close without assigning
        return;
    }

    // === X / Y assign buttons + toggles (press only) ===
    if (!is_move && x < AX_W && y >= KB_Y) {
        if (y >= KB_Y + 2 * AX_H + TGT_H) {
            // STK toggle: plug/unplug the left stick from the kaoss dests
            stick_sync_ = !stick_sync_;
            mark_dirty();
            return;
        }
        if (y >= KB_Y + 2 * AX_H) {
            // TRK/ALL toggle. mid-gesture switch would orphan baselines - ignore then.
            if (!kaoss_active_) { kaoss_all_ = !kaoss_all_; mark_dirty(); }
            return;
        }
        kaoss_menu_ = (y < KB_Y + AX_H) ? 1 : 2;   // top = X, bottom = Y
        kaoss_menu_frame_ = frame_;
        mark_dirty();
        return;
    }
    if (x < KF_X || y < KF_Y) return;

    // normalize field position: X right = 1000, Y up = 1000
    int nx = (x - KF_X) * 1000 / (KF_W - 1);
    int ny = (KF_Y + KF_H - 1 - y) * 1000 / (KF_H - 1);
    if (nx < 0) nx = 0; if (nx > 1000) nx = 1000;
    if (ny < 0) ny = 0; if (ny > 1000) ny = 1000;

    if (!kaoss_active_) {
        // gesture start: grab the target track(s) + snapshot baselines
        int trk = song_col_;
        if (trk < 0) trk = 0;
        if (trk >= audio::NUM_TRACKS) trk = audio::NUM_TRACKS - 1;
        kaoss_track_ = trk;
        kaoss_grab_mask_ = kaoss_all_ ? 0xFF : (uint8_t)(1u << trk);
        for (int t = 0; t < audio::NUM_TRACKS; ++t)
            if (kaoss_grab_mask_ & (1u << t)) kaoss_snapshot(t);
        kaoss_release_ = 0;     // cancel a pending release ramp
        kaoss_active_  = true;
        // tap ripple: expanding ring from the touch point
        kaoss_rip_x_ = (int16_t)x;
        kaoss_rip_y_ = (int16_t)y;
        kaoss_rip_frame_ = frame_;
    }
    kaoss_x_ = nx;
    kaoss_y_ = ny;

    // push the finger trail (screen px)
    kaoss_trail_x_[kaoss_trail_pos_] = (int16_t)x;
    kaoss_trail_y_[kaoss_trail_pos_] = (int16_t)y;
    kaoss_trail_pos_ = (uint8_t)((kaoss_trail_pos_ + 1) % KAOSS_TRAIL);
    if (kaoss_trail_len_ < KAOSS_TRAIL) ++kaoss_trail_len_;

    apply_kaoss();
}

// per-frame housekeeping: release ramp back to baseline + trail dissipation.
// called from App::tick().
void App::kaoss_tick() {
    if (kaoss_active_) return;

    // trail dissolves once the finger is gone (one point every other frame)
    if (kaoss_trail_len_ > 0 && (frame_ & 1)) --kaoss_trail_len_;

    if (kaoss_release_ <= 0) return;
    const int n = kaoss_release_;
    // linear convergence: each frame close 1/n of the remaining gap -> exact at n=1
    auto step_q15 = [n](fx::q15& v, fx::q15 target) {
        v = (fx::q15)((int)v + ((int)target - (int)v) / n);
    };
    for (int t = 0; t < audio::NUM_TRACKS; ++t) {
        if (!(kaoss_grab_mask_ & (1u << t))) continue;
        auto& ts = mixer_.track(t);
        const auto& base = kaoss_base_[t];
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
    --kaoss_release_;
}

void App::draw_kaoss(Draw& d) {
    // === X / Y assign buttons + TRK/ALL toggle (left column) ===
    {
        struct { const char* tag; KaossDest dst; int menu; } btns[2] = {
            { "X", kaoss_dest_x_, 1 }, { "Y", kaoss_dest_y_, 2 },
        };
        for (int i = 0; i < 2; ++i) {
            int y = KB_Y + i * AX_H;
            bool open = (kaoss_menu_ == btns[i].menu);
            Color bg     = open ? pal::HEADER : pal::BG_HI;
            Color border = open ? pal::FG     : pal::FG_DIM;
            ui_button(d, 0, y + 1, AX_W - 2, AX_H - 2, bg, border, nullptr, 0, open);
            // axis tag on top, current destination below
            d.text(4, y + 5, btns[i].tag, open ? pal::FG : pal::HEADER);
            d.text(4, y + AX_H - 14, kaoss_dest_name((uint8_t)btns[i].dst),
                   open ? pal::FG : pal::CURSOR);
        }
        // target toggle: TRK = track under cursor, ALL = every track at once
        {
            int y = KB_Y + 2 * AX_H;
            Color bg = kaoss_all_ ? pal::PLAY_BG : pal::BG_HI;
            ui_button(d, 0, y + 1, AX_W - 2, TGT_H - 2, bg, pal::FG_DIM,
                      kaoss_all_ ? "ALL" : "TRK",
                      kaoss_all_ ? pal::PLAY : pal::FG_HEX, kaoss_all_);
        }
        // stick sync toggle: left stick plugged into the kaoss dests or inert
        {
            int y = KB_Y + 2 * AX_H + TGT_H;
            Color bg = stick_sync_ ? pal::PLAY_BG : pal::BG_HI;
            ui_button(d, 0, y + 1, AX_W - 2, STK_H - 2, bg, pal::FG_DIM,
                      "STK", stick_sync_ ? pal::PLAY : pal::FG_DIM, stick_sync_);
        }
    }

    // === XY field ===
    d.rect(KF_X, KF_Y, KF_W, KF_H, pal::PANEL);
    // quarter grid
    for (int i = 1; i < 4; ++i) {
        d.rect(KF_X + KF_W * i / 4, KF_Y, 1, KF_H, field_grid());
        d.rect(KF_X, KF_Y + KF_H * i / 4, KF_W, 1, field_grid());
    }

    // axis labels: X bottom-right, Y top-left
    d.text(KF_X + KF_W - 22, KF_Y + KF_H - 10, kaoss_dest_name((uint8_t)kaoss_dest_x_), pal::FG_DIM);
    d.text(KF_X + 3,         KF_Y + 2,          kaoss_dest_name((uint8_t)kaoss_dest_y_), pal::FG_DIM);

    // finger trail: oldest = dimmest (fades into the field bg)
    for (int i = 0; i < kaoss_trail_len_; ++i) {
        int idx = (kaoss_trail_pos_ + KAOSS_TRAIL - kaoss_trail_len_ + i) % KAOSS_TRAIL;
        int tx = kaoss_trail_x_[idx], ty = kaoss_trail_y_[idx];
        if (tx < KF_X || ty < KF_Y) continue;
        uint8_t t = (uint8_t)((i + 1) * 200 / (kaoss_trail_len_ ? kaoss_trail_len_ : 1));
        Color c = lerp_color(pal::PANEL, pal::PLAY, t);
        d.rect(tx - 1, ty - 1, 2, 2, c);
    }

    // tap ripple: square ring expanding from the touch point, fading out.
    // clamped to the field so it never bleeds over the buttons.
    if (kaoss_rip_x_ >= 0) {
        uint32_t age = frame_ - kaoss_rip_frame_;
        constexpr uint32_t RIP_LIFE = 14;
        if (age < RIP_LIFE) {
            int r = 3 + (int)age * 2;
            uint8_t a = (uint8_t)(200 - age * 200 / RIP_LIFE);
            Color rc = with_alpha(pal::CURSOR, a);
            int x0 = kaoss_rip_x_ - r, x1 = kaoss_rip_x_ + r;
            int y0 = kaoss_rip_y_ - r, y1 = kaoss_rip_y_ + r;
            if (x0 < KF_X) x0 = KF_X;
            if (x1 > KF_X + KF_W - 1) x1 = KF_X + KF_W - 1;
            if (y0 < KF_Y) y0 = KF_Y;
            if (y1 > KF_Y + KF_H - 1) y1 = KF_Y + KF_H - 1;
            if (x1 > x0 && y1 > y0) {
                if (kaoss_rip_y_ - r >= KF_Y)          d.rect(x0, y0, x1 - x0, 1, rc);
                if (kaoss_rip_y_ + r <  KF_Y + KF_H)   d.rect(x0, y1, x1 - x0, 1, rc);
                if (kaoss_rip_x_ - r >= KF_X)          d.rect(x0, y0, 1, y1 - y0, rc);
                if (kaoss_rip_x_ + r <  KF_X + KF_W)   d.rect(x1, y0, 1, y1 - y0 + 1, rc);
            }
        } else {
            kaoss_rip_x_ = -1;   // expired
        }
    }

    // crosshair at the current position
    int cx = KF_X + kaoss_x_ * (KF_W - 1) / 1000;
    int cy = KF_Y + (KF_H - 1) - kaoss_y_ * (KF_H - 1) / 1000;
    Color line = kaoss_active_ ? pal::HEADER : pal::FG_DIM;
    d.rect(cx, KF_Y, 1, KF_H, line);
    d.rect(KF_X, cy, KF_W, 1, line);
    d.rect(cx - 2, cy - 2, 5, 5, kaoss_active_ ? pal::CURSOR : pal::FG_DIM);

    // active glow: breathing corner brackets around the field
    if (kaoss_active_) {
        uint8_t pulse = breathe_pulse(frame_, 48);
        d.corner_brackets(KF_X, KF_Y, KF_W, KF_H,
                          lerp_color(pal::HEADER, pal::CURSOR, pulse), 6, 1);
    } else if (kaoss_release_ > 0) {
        // release ramp in progress - dim brackets fading out
        uint8_t t = (uint8_t)(kaoss_release_ * 255 / KAOSS_REL_FRAMES);
        d.corner_brackets(KF_X, KF_Y, KF_W, KF_H,
                          lerp_color(pal::PANEL, pal::FG_DIM, t), 6, 1);
    }

    // header line inside the field: target + normalized live values
    {
        char buf[44];
        int trk = kaoss_active_ ? kaoss_track_
                : (song_col_ >= 0 && song_col_ < audio::NUM_TRACKS ? song_col_ : 0);
        char tgt[6];
        if (kaoss_all_) std::snprintf(tgt, sizeof(tgt), "ALL");
        else            std::snprintf(tgt, sizeof(tgt), "T%d", trk);
        std::snprintf(buf, sizeof(buf), "%s %s%03d %s%03d", tgt,
                      kaoss_dest_name((uint8_t)kaoss_dest_x_), kaoss_x_ / 10,
                      kaoss_dest_name((uint8_t)kaoss_dest_y_), kaoss_y_ / 10);
        d.text(KF_X + KF_W - 6 * 16, KF_Y + 2, buf,
               kaoss_active_ ? pal::CURSOR : pal::FG_DIM);
    }

    // === popup destination menu (drawn last - over everything) ===
    if (kaoss_menu_ != 0) {
        // unfold: veil fades in + rows appear with a stagger (2 frames per row)
        uint32_t age = frame_ - kaoss_menu_frame_;
        constexpr uint32_t VEIL_IN = 5;
        uint8_t va = (age < VEIL_IN) ? (uint8_t)(160 * (age + 1) / VEIL_IN) : 160;
        d.rect(KF_X, KF_Y, KF_W, KF_H, with_alpha(0xFF000000, va));
        const char* title = (kaoss_menu_ == 1) ? "ASSIGN X" : "ASSIGN Y";
        d.text(MENU_X, KF_Y + 3, title, pal::HEADER);
        const KaossDest cur = (kaoss_menu_ == 1) ? kaoss_dest_x_ : kaoss_dest_y_;
        const int n = (int)KaossDest::COUNT;
        for (int i = 0; i < n; ++i) {
            int col = i % MENU_COLS, row = i / MENU_COLS;
            if (age < 2 + (uint32_t)row * 2) continue;   // staggered reveal
            int bx = MENU_X + col * MENU_CW;
            int by = MENU_Y + row * MENU_CH;
            bool sel = ((KaossDest)i == cur);
            Color bg     = sel ? pal::HEADER : pal::BG_HI;
            Color border = sel ? pal::FG     : pal::FG_DIM;
            ui_button(d, bx, by, MENU_CW - 4, MENU_CH - 4, bg, border,
                      kaoss_dest_name((uint8_t)i), sel ? pal::FG : pal::FG_HEX, sel);
        }
    }
}

} // namespace trackr::ui
