// App: mixer view (m8-style, adapted for 8 tracks + master/fx strip).
// top screen: 8 channel strips (fader + live peak meter + mute) and a master
// column (master vol, delay time/fb/wet, reverb wet).
// song.track_vol / master settings persist in the project; synced into the
// audio mixer here every update.
#include "../app.h"
#include "../ui_internal.h"
#include "../../audio/fixed.h"
#include <cstdio>

namespace trackr::ui {

namespace {
    constexpr int MX_TRACKS = seq::NUM_TRACKS;       // 8 channel strips
    constexpr int MX_COLS   = MX_TRACKS + 2;         // + master strip + groove zone
    constexpr int MX_MASTER = MX_TRACKS;             // col 8
    constexpr int MX_GROOVE = MX_TRACKS + 1;         // col 9
    // master strip rows
    enum { MR_MASTER = 0, MR_DLY_TIME, MR_DLY_FB, MR_DLY_WET, MR_REV_WET,
           MR_REV_SIZE, MR_REV_DAMP, MR_DUCK_SRC, MR_DUCK_REL, MR_ROWS };
    static const char* const kMasterRows[MR_ROWS] = {
        "MST", "DTIM", "DFB", "DWET", "RWET", "RSIZ", "RDMP", "DUCK", "DREL"
    };
}

// push song mixer settings into the audio mixer (called from update_mixer and
// on project load via main's sync path - cheap, just assignments).
static void sync_mixer(seq::Project& p, audio::Mixer& m) {
    for (int t = 0; t < MX_TRACKS; ++t) {
        m.track(t).mix_vol = (fx::q15)((int)p.song.track_vol[t] * fx::Q15_ONE / 255);
        m.track(t).duck_amt = (fx::q15)((int)p.song.track_duck[t] * fx::Q15_ONE / 255);
    }
    m.master = (fx::q15)((int)p.song.master_vol * fx::Q15_ONE / 255);
    // delay time: 0..255 -> 32..8160 frames (cap at DELAY_BUF-1)
    std::size_t dt = (std::size_t)p.song.dly_time * 32;
    if (dt < 32) dt = 32;
    if (dt >= audio::Mixer::DELAY_BUF) dt = audio::Mixer::DELAY_BUF - 1;
    m.delay_time     = dt;
    m.delay_feedback = (fx::q15)((int)p.song.dly_fb  * fx::Q15_ONE / 255);
    m.delay_wet      = (fx::q15)((int)p.song.dly_wet * fx::Q15_ONE / 255);
    m.reverb_wet     = (fx::q15)((int)p.song.rev_wet * fx::Q15_ONE / 255);
    // reverb character (Project tail, v12). 0 = legacy file -> defaults matching
    // the old hardcoded comb values (fb 0.65, damp 30%).
    {
        int es = p.rev_size ? p.rev_size : 92;   // 0.50..0.92 comb feedback
        int ed = p.rev_damp ? p.rev_damp : 85;   // 0..0.9 damping
        m.reverb.set_room_size((fx::q15)(16384 + es * 54));
        m.reverb.set_damping((fx::q15)(ed * 115));
    }
    // duck release: 0..255 -> ~2000..34000 frames (60ms..1.06s @32k)
    m.duck_rel_frames = 2000u + (uint32_t)p.song.duck_rel * 125u;
}

void App::update_mixer(const InputState& in) {
    // navigation: left/right across strips; up/down inside the master strip / groove zone
    if (in.left)  mixer_col_ = (mixer_col_ - 1 + MX_COLS) % MX_COLS;
    if (in.right) mixer_col_ = (mixer_col_ + 1) % MX_COLS;
    if (mixer_col_ == MX_MASTER) {
        if (in.up)   mixer_row_ = (mixer_row_ - 1 + MR_ROWS) % MR_ROWS;
        if (in.down) mixer_row_ = (mixer_row_ + 1) % MR_ROWS;
        if (mixer_row_ >= MR_ROWS) mixer_row_ = 0;
    } else if (mixer_col_ == MX_GROOVE) {
        if (in.up)   mixer_row_ = (mixer_row_ - 1 + seq::PHRASE_STEPS) % seq::PHRASE_STEPS;
        if (in.down) mixer_row_ = (mixer_row_ + 1) % seq::PHRASE_STEPS;
    } else {
        // track strip: row 0 = fader, row 1 = duck depth
        if (in.up || in.down) mixer_row_ = mixer_row_ ? 0 : 1;
        if (mixer_row_ > 1) mixer_row_ = 0;
    }

    int delta = 0;
    if (in.a) delta = +8;
    if (in.b) delta = -8;
    if (in.x) delta = +32;
    if (in.y) delta = -32;

    auto& song = project_.song;

    // groove zone: A/B = +/-1 tick, X = straight 6, Y = clear slot
    if (mixer_col_ == MX_GROOVE) {
        uint8_t& g = song.groove_steps[mixer_row_];
        if (in.a) { int v = g + 1; if (v > 12) v = 12; g = (uint8_t)v; dirty = true; }
        if (in.b) { int v = g - 1; if (v < 0) v = 0; g = (uint8_t)v; dirty = true; }
        if (in.x) { g = 6; dirty = true; }
        if (in.y) { g = 0; dirty = true; }
        sync_mixer(project_, mixer_);
        return;
    }

    if (delta) {
        auto bump = [&](uint8_t& v) {
            int nv = (int)v + delta;
            if (nv < 0) nv = 0;
            if (nv > 255) nv = 255;
            v = (uint8_t)nv;
        };
        if (mixer_col_ < MX_TRACKS) {
            // track strip: row 0 = channel fader, row 1 = sidechain duck depth
            if (mixer_row_ == 1) bump(song.track_duck[mixer_col_]);
            else                 bump(song.track_vol[mixer_col_]);
        } else if (mixer_col_ == MX_MASTER) switch (mixer_row_) {
            case MR_MASTER:   bump(song.master_vol); break;
            case MR_DLY_TIME: bump(song.dly_time);   break;
            case MR_DLY_FB:   bump(song.dly_fb);     break;
            case MR_DLY_WET:  bump(song.dly_wet);    break;
            case MR_REV_WET:  bump(song.rev_wet);    break;
            case MR_REV_SIZE:
                if (!project_.rev_size) project_.rev_size = 92;   // legacy 0 -> default first
                bump(project_.rev_size);
                if (!project_.rev_size) project_.rev_size = 1;    // keep out of legacy 0
                break;
            case MR_REV_DAMP:
                if (!project_.rev_damp) project_.rev_damp = 85;
                bump(project_.rev_damp);
                if (!project_.rev_damp) project_.rev_damp = 1;
                break;
            case MR_DUCK_SRC: {
                // src cycles OFF, T0..T7 (delta sign only - it's an enum not a level)
                int v = (song.duck_src == 0xFF) ? -1 : song.duck_src;
                v += (delta > 0) ? 1 : -1;
                if (v < -1) v = seq::NUM_TRACKS - 1;
                if (v >= seq::NUM_TRACKS) v = -1;
                song.duck_src = (v < 0) ? 0xFF : (uint8_t)v;
                break;
            }
            case MR_DUCK_REL: bump(song.duck_rel);   break;
        }
        dirty = true;
    }

    // SELECT on a track strip = mute toggle (matches song-view mute pads)
    if (in.select_ && mixer_col_ < MX_TRACKS) {
        auto& tr = mixer_.track(mixer_col_);
        tr.muted = !tr.muted;
        if (tr.muted) mixer_.note_off_all(mixer_col_);
    }

    // keep the audio mixer in sync every frame (cheap)
    sync_mixer(project_, mixer_);
}

// === bottom-screen TOUCH FADERS (Mixer view) ===
// 9 strips: 8 tracks + MST. drag inside a strip = set volume (finger Y maps
// to the fader), tap the header cell = mute toggle (tracks only).
// same data as the top-screen mixer: song.track_vol / master_vol, synced by
// sync_mixer() every update.
namespace {
    constexpr int MF_Y  = 96;              // strip area top (below the OCT row)
    constexpr int MF_H  = 240 - MF_Y - 4;  // 140
    constexpr int MF_N  = 9;               // 8 tracks + master
    constexpr int MF_W  = 320 / MF_N;      // 35
    constexpr int MF_HEAD = 16;            // mute header height
    inline int mf_x(int i) { return 2 + i * MF_W; }
}

void App::draw_mixer_faders(Draw& d) {
    auto& song = project_.song;
    for (int i = 0; i < MF_N; ++i) {
        const bool master = (i == 8);
        int x = mf_x(i);
        int w = MF_W - 4;
        uint8_t vol = master ? song.master_vol : song.track_vol[i];
        bool muted  = !master && mixer_.track(i).muted;
        bool sel    = (mixer_col_ == i);

        // header cell: track tag / MUTE state (tactile)
        Color hbg = muted ? pal::RECORD : (sel ? pal::HEADER : pal::BG_HI);
        char tb[4];
        if (master) std::snprintf(tb, sizeof(tb), "MS");
        else        std::snprintf(tb, sizeof(tb), "T%d", i);
        ui_button(d, x, MF_Y, w, MF_HEAD - 2, hbg, hbg, tb,
                  muted ? pal::FG : (sel ? pal::FG : pal::FG_DIM), sel || muted);

        // fader well
        int fy = MF_Y + MF_HEAD;
        int fh = MF_H - MF_HEAD;
        d.rect(x, fy, w, fh, pal::PANEL);
        // fill from the bottom
        int fill = (int)vol * (fh - 2) / 255;
        Color fc = master ? pal::HEADER : (muted ? pal::FG_DIM : pal::PLAY);
        if (fill > 0) d.rect(x + 2, fy + fh - 1 - fill, w - 4, fill,
                             lerp_color(pal::PANEL, fc, 180));
        // cap line
        d.rect(x, fy + fh - 1 - fill, w, 2, fc);
        // live peak meter: thin line on the right edge (tracks only)
        if (!master) {
            int mh = (int)mixer_.track(i).meter * (fh - 2) / fx::Q15_ONE;
            if (mh > fh - 2) mh = fh - 2;
            if (mh > 0) {
                Color mc = (mixer_.track(i).meter > fx::Q15_ONE * 9 / 10)
                         ? pal::RECORD : pal::PLAY;
                d.rect(x + w - 3, fy + fh - 1 - mh, 2, mh, mc);
            }
            // peak-hold cap
            int ph = (int)peak_hold_[i] * (fh - 2) / fx::Q15_ONE;
            if (ph > fh - 2) ph = fh - 2;
            if (ph > 2) d.rect(x + w - 3, fy + fh - 1 - ph, 2, 1, pal::FG);
        }
        // numeric value at the bottom of the well (bright when this strip is selected)
        char vb[5];
        std::snprintf(vb, sizeof(vb), "%3d", vol);
        d.text(x + (w - 18) / 2, fy + fh - 10, vb, sel ? pal::FG : pal::FG_DIM);
    }
}

void App::mixer_fader_touch(int x, int y, bool is_move) {
    if (y < MF_Y) return;
    int i = (x - 2) / MF_W;
    if (i < 0) i = 0;
    if (i >= MF_N) i = MF_N - 1;
    const bool master = (i == 8);
    auto& song = project_.song;

    // header tap = mute toggle (tap only - drags passing through don't flip it)
    if (!is_move && y < MF_Y + MF_HEAD) {
        if (!master) {
            auto& tr = mixer_.track(i);
            tr.muted = !tr.muted;
            if (tr.muted) mixer_.note_off_all(i);
        }
        mixer_col_ = i;
        mark_dirty();
        return;
    }

    // fader zone: finger Y -> volume (bottom = 0, top = 255)
    int fy = MF_Y + MF_HEAD;
    int fh = MF_H - MF_HEAD;
    int v = (fy + fh - 1 - y) * 255 / (fh - 2);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    if (master) song.master_vol = (uint8_t)v;
    else        song.track_vol[i] = (uint8_t)v;
    mixer_col_ = i;          // follow the finger with the cursor
    mark_dirty();
    // push into the audio mixer right away (don't wait for update_mixer)
    sync_mixer(project_, mixer_);
}

void App::draw_mixer(Draw& d) {
    constexpr int Y0 = 24;
    constexpr int STRIP_W = 34;                    // 8*34=272 for tracks
    constexpr int X0 = 6;
    constexpr int FADER_H = 130;
    constexpr int FADER_Y = Y0 + 26;
    constexpr int MX_X = X0 + MX_TRACKS * STRIP_W + 8;    // master strip x (286)
    constexpr int GR_X = 352;                              // groove zone x

    d.text(X0, Y0, "MIXER", pal::HEADER, 1);
    d.text(X0 + 60, Y0, "A/B X/Y:VOL SELECT:MUTE v:DUCK", pal::FG_DIM);

    // === 8 track strips ===
    for (int t = 0; t < MX_TRACKS; ++t) {
        int x = X0 + t * STRIP_W;
        bool sel = (mixer_col_ == t);
        auto& tr = mixer_.track(t);
        uint8_t vol = project_.song.track_vol[t];

        // header: track number (T0..T7 - same numbering as song view / touch pads)
        char hb[6];
        std::snprintf(hb, sizeof(hb), "T%d", t);
        d.text(x + 8, FADER_Y - 12, hb, sel ? pal::CURSOR : pal::HEADER);

        // fader track (background) + fill up to vol
        constexpr int FW = 10;
        int fx_ = x + 6;
        d.rect(fx_, FADER_Y, FW, FADER_H, pal::BG_HI);
        int fh = (int)vol * FADER_H / 255;
        d.rect(fx_, FADER_Y + FADER_H - fh, FW, fh, sel ? pal::CURSOR : pal::HEADER);
        // fader cap line
        d.rect(fx_ - 2, FADER_Y + FADER_H - fh - 1, FW + 4, 2, sel ? pal::FG : pal::FG_DIM);

        // live peak meter to the right of the fader
        int mh = (int)tr.meter * FADER_H / fx::Q15_ONE;
        if (mh > FADER_H) mh = FADER_H;
        d.rect(fx_ + FW + 4, FADER_Y, 4, FADER_H, pal::BG_HI);
        if (mh > 0) {
            // green body + red top segment when close to clip
            ui::Color mc = (tr.meter > fx::Q15_ONE * 9 / 10) ? pal::RECORD : pal::PLAY;
            d.rect(fx_ + FW + 4, FADER_Y + FADER_H - mh, 4, mh, mc);
        }
        // peak-hold cap: recent maximum, falling slowly (updated in tick())
        {
            int ph = (int)peak_hold_[t] * FADER_H / fx::Q15_ONE;
            if (ph > FADER_H) ph = FADER_H;
            if (ph > 2) {
                ui::Color pc = (peak_hold_[t] > fx::Q15_ONE * 9 / 10) ? pal::RECORD : pal::FG;
                d.rect(fx_ + FW + 4, FADER_Y + FADER_H - ph, 4, 1, pc);
            }
        }

        // value + mute state under the fader
        char vb[8];
        std::snprintf(vb, sizeof(vb), "%3d", vol);
        d.text(x + 4, FADER_Y + FADER_H + 6, vb, sel && mixer_row_ == 0 ? pal::FG : pal::FG_DIM);
        // duck depth mini-row below the value. accent when this row is selected;
        // duck source track gets a "SRC" tag instead (it pumps, doesn't dip).
        {
            bool drow = sel && (mixer_row_ == 1);
            if ((int)project_.song.duck_src == t) {
                d.text(x + 4, FADER_Y + FADER_H + 16, "vSRC", drow ? pal::CURSOR : pal::PLAY);
            } else {
                uint8_t dk = project_.song.track_duck[t];
                char db[8];
                std::snprintf(db, sizeof(db), "v%3d", dk);
                d.text(x + 4, FADER_Y + FADER_H + 16, db,
                       drow ? pal::CURSOR : (dk ? pal::FG_HEX : pal::FG_DIM));
            }
        }
        if (tr.muted) {
            d.rect(x + 2, FADER_Y + FADER_H / 2 - 5, STRIP_W - 8, 11, with_alpha(pal::RECORD, 200));
            d.text(x + 8, FADER_Y + FADER_H / 2 - 3, "MUTE", pal::FG);
        }

        // selection brackets around the whole strip
        if (sel) {
            uint8_t br = breathe_pulse(frame_, 64);
            ui::Color cur = lerp_color(with_alpha(pal::CURSOR, 130), pal::CURSOR, br);
            d.corner_brackets(x, FADER_Y - 14, STRIP_W - 6, FADER_H + 32, cur, 5, 1);
        }
    }

    // === master / fx strip (rows) ===
    {
        bool strip_sel = (mixer_col_ == MX_MASTER);
        d.text(MX_X, FADER_Y - 12, "MASTER", strip_sel ? pal::CURSOR : pal::HEADER);
        const uint8_t vals[MR_ROWS] = {
            project_.song.master_vol, project_.song.dly_time,
            project_.song.dly_fb, project_.song.dly_wet, project_.song.rev_wet,
            (uint8_t)(project_.rev_size ? project_.rev_size : 92),
            (uint8_t)(project_.rev_damp ? project_.rev_damp : 85),
            project_.song.duck_src, project_.song.duck_rel
        };
        for (int r = 0; r < MR_ROWS; ++r) {
            int y = FADER_Y + 4 + r * 19;   // 9 rows now - tighter pitch than the old 24
            bool sel = strip_sel && (mixer_row_ == r);
            d.text(MX_X, y, kMasterRows[r], sel ? pal::CURSOR : pal::HEADER);
            constexpr int BW = 30;
            if (r == MR_DUCK_SRC) {
                // duck source: OFF / T0..T7 as text
                char sb[6];
                if (project_.song.duck_src == 0xFF) std::snprintf(sb, sizeof(sb), "OFF");
                else std::snprintf(sb, sizeof(sb), "T%d", project_.song.duck_src);
                d.text(MX_X + BW + 3, y + 8, sb,
                       project_.song.duck_src != 0xFF ? pal::PLAY : (sel ? pal::FG : pal::FG_DIM));
                // pump indicator: live duck envelope as a draining bar
                int denv = (int)mixer_.duck_env() * BW / fx::Q15_ONE;
                d.rect(MX_X, y + 10, BW, 5, pal::BG_HI);
                if (denv > 0) d.rect(MX_X, y + 10, denv, 5, pal::PLAY);
            } else {
                d.rect(MX_X, y + 10, BW, 5, pal::BG_HI);
                d.rect(MX_X, y + 10, (int)vals[r] * BW / 255, 5, sel ? pal::CURSOR : pal::HEADER);
                char vb[6];
                std::snprintf(vb, sizeof(vb), "%3d", vals[r]);
                d.text(MX_X + BW + 3, y + 8, vb, sel ? pal::FG : pal::FG_DIM);
            }
            if (sel) {
                uint8_t br = breathe_pulse(frame_, 64);
                d.corner_brackets(MX_X - 3, y - 2, 58, 18,
                                  lerp_color(with_alpha(pal::CURSOR, 130), pal::CURSOR, br), 3, 1);
            }
        }
    }

    // === groove pattern zone (m8-style ticks-per-step ladder) ===
    {
        bool zone_sel = (mixer_col_ == MX_GROOVE);
        d.text(GR_X, FADER_Y - 12, "GRV", zone_sel ? pal::CURSOR : pal::HEADER);
        const auto& gs = project_.song.groove_steps;
        // pattern length = leading non-zero run
        int len = 0;
        while (len < seq::PHRASE_STEPS && gs[len] != 0) ++len;
        // no pattern -> the global groove value drives everything; show it in the header
        if (len == 0) {
            char gb[8];
            std::snprintf(gb, sizeof(gb), "=%d", project_.song.groove);
            d.text(GR_X + 22, FADER_Y - 12, gb, pal::FG_HEX);
        }
        constexpr int GROW_H = 11;
        for (int i = 0; i < seq::PHRASE_STEPS; ++i) {
            int y = FADER_Y + i * GROW_H;
            bool sel = zone_sel && (mixer_row_ == i);
            bool in_pat = (i < len);
            char rb[8];
            if (gs[i] == 0) std::snprintf(rb, sizeof(rb), "%X --", i);
            else            std::snprintf(rb, sizeof(rb), "%X %2d", i, gs[i]);
            d.text(GR_X, y, rb, sel ? pal::FG : (in_pat ? pal::FG_HEX : pal::FG_DIM));
            // mini tick bar
            if (gs[i] > 0) {
                int bw = gs[i] * 2;
                d.rect(GR_X + 26, y + 2, bw, 4, in_pat ? pal::PLAY : pal::FG_DIM);
            }
            if (sel) {
                uint8_t br = breathe_pulse(frame_, 64);
                d.corner_brackets(GR_X - 2, y - 1, 46, GROW_H, 
                                  lerp_color(with_alpha(pal::CURSOR, 130), pal::CURSOR, br), 3, 1);
            }
        }
        // hint under the ladder
        if (zone_sel)
            d.text(GR_X - 46, FADER_Y + 16 * GROW_H + 4, "A/B:TICKS X:6 Y:CLR", pal::FG_DIM);
    }
}

} // namespace trackr::ui
