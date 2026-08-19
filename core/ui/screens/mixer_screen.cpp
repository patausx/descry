// App: mixer view (m8-style, adapted for 8 tracks + master/fx strip).
// top screen: 8 channel strips (fader + live peak meter + mute) and a master
// column (master vol, delay time/fb/wet, reverb wet).
// song.track_vol / master settings persist in the project; synced into the
// audio mixer here every update.
#include "../app.h"
#include "../ui_internal.h"
#include "../../audio/fixed.h"
#include <cstdio>
#include <cstring>

namespace trackr::ui {

namespace {
    constexpr int AUDIO_SR = 32000;
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

// push song mixer settings into the audio mixer.
// the actual mapping lives in seq::Player::apply_song_mixer (core) so the
// offline renderer produces the same mix as playback.
// solo is layered on top HERE, not in core: it's a momentary UI state, not part
// of the project. without this the per-frame sync would stomp an active solo
// with the persisted mute mask.
void App::sync_mixer_from_song() {
    seq::Player::apply_song_mixer(project_, mixer_);
    if (solo_track_ >= 0 && solo_track_ < seq::NUM_TRACKS) {
        for (int t = 0; t < seq::NUM_TRACKS; ++t)
            mixer_.track(t).muted = (t != solo_track_);
    }
}

void App::update_mixer(const InputState& in) {
    // navigation: left/right across strips; up/down inside the master strip / groove zone
    if (in.left)  mixer_col_ = wrap_index(mixer_col_, -1, MX_COLS);
    if (in.right) mixer_col_ = wrap_index(mixer_col_, +1, MX_COLS);
    if (mixer_col_ == MX_MASTER) {
        if (in.up)   mixer_row_ = wrap_index(mixer_row_, -1, MR_ROWS);
        if (in.down) mixer_row_ = wrap_index(mixer_row_, +1, MR_ROWS);
        if (mixer_row_ >= MR_ROWS) mixer_row_ = 0;
    } else if (mixer_col_ == MX_GROOVE) {
        if (in.up)   mixer_row_ = wrap_index(mixer_row_, -1, seq::PHRASE_STEPS);
        if (in.down) mixer_row_ = wrap_index(mixer_row_, +1, seq::PHRASE_STEPS);
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
    if (in.encoder_delta) delta = in.encoder_delta;

    auto& song = project_.song;

    // groove zone: A/B = +/-1 tick, X = straight 6, Y = clear slot
    if (mixer_col_ == MX_GROOVE) {
        uint8_t& g = song.groove_steps[mixer_row_];
        if (in.a || in.b || in.encoder_delta) {
            int step = in.encoder_delta ? in.encoder_delta : (in.a ? 1 : -1);
            if (bump_clamped(g, step, 0, 12)) mark_project_dirty();
        }
        if (in.x && g != 6) { g = 6; mark_project_dirty(); }
        if (in.y && g != 0) { g = 0; mark_project_dirty(); }
        sync_mixer_from_song();
        return;
    }

    if (delta) {
        const auto before_song = song;
        const uint8_t before_rev_size = project_.rev_size;
        const uint8_t before_rev_damp = project_.rev_damp;
        if (mixer_col_ < MX_TRACKS) {
            // track strip: row 0 = channel fader, row 1 = sidechain duck depth
            if (mixer_row_ == 1) bump_clamped(song.track_duck[mixer_col_], delta, 0, 255);
            else                 bump_clamped(song.track_vol[mixer_col_], delta, 0, 255);
        } else if (mixer_col_ == MX_MASTER) switch (mixer_row_) {
            case MR_MASTER:   bump_clamped(song.master_vol, delta, 0, 255); break;
            case MR_DLY_TIME: bump_clamped(song.dly_time, delta, 0, 255);   break;
            case MR_DLY_FB:   bump_clamped(song.dly_fb, delta, 0, 255);     break;
            case MR_DLY_WET:  bump_clamped(song.dly_wet, delta, 0, 255);    break;
            case MR_REV_WET:  bump_clamped(song.rev_wet, delta, 0, 255);    break;
            case MR_REV_SIZE:
                if (!project_.rev_size) project_.rev_size = 92;   // legacy 0 -> default first
                bump_clamped(project_.rev_size, delta, 0, 255);
                if (!project_.rev_size) project_.rev_size = 1;    // keep out of legacy 0
                break;
            case MR_REV_DAMP:
                if (!project_.rev_damp) project_.rev_damp = 85;
                bump_clamped(project_.rev_damp, delta, 0, 255);
                if (!project_.rev_damp) project_.rev_damp = 1;
                break;
            case MR_DUCK_SRC: {
                // src cycles OFF, T0..T7 (delta sign only - it's an enum not a level)
                int v = wrap_index((song.duck_src == 0xFF) ? 0 : song.duck_src + 1,
                                   delta > 0 ? 1 : -1, seq::NUM_TRACKS + 1) - 1;
                song.duck_src = (v < 0) ? 0xFF : (uint8_t)v;
                break;
            }
            case MR_DUCK_REL: bump_clamped(song.duck_rel, delta, 0, 255); break;
        }
        if (std::memcmp(&before_song, &song, sizeof(song)) != 0 ||
            before_rev_size != project_.rev_size || before_rev_damp != project_.rev_damp)
            mark_project_dirty();
    }

    // SELECT on a track strip = mute toggle (matches song-view mute pads).
    // writes the PROJECT mask (persisted + survives PLAY), mixer follows via sync.
    if (in.select_ && mixer_col_ < MX_TRACKS) {
        project_.track_mute ^= (uint8_t)(1u << mixer_col_);
        mixer_mute_motion_mask_ |= (uint8_t)(1u << mixer_col_);
        mixer_mute_motion_frame_ = frame_;
        if (project_.track_mute & (1u << mixer_col_)) mixer_.note_off_all(mixer_col_);
        mark_project_dirty();
    }

    // keep the audio mixer in sync every frame (cheap)
    sync_mixer_from_song();
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
        if (fill > 0) {
            // Four cheap bands give the fader body some depth without turning
            // the restrained tracker palette into glossy skeuomorphic sludge.
            constexpr int BANDS = 4;
            const int top = fy + fh - 1 - fill;
            for (int b = 0; b < BANDS; ++b) {
                int y0 = top + fill * b / BANDS;
                int y1 = top + fill * (b + 1) / BANDS;
                uint8_t shade = (uint8_t)(105 + b * 28); // darker top, richer bottom
                if (y1 > y0)
                    d.rect(x + 2, y0, w - 4, y1 - y0,
                           lerp_color(pal::PANEL, fc, shade));
            }
            // one-pixel side sheen keeps the narrow strip legible at low values
            d.rect(x + 2, top, 1, fill, with_alpha(pal::FG, muted ? 22 : 42));
        }
        // cap line
        d.rect(x, fy + fh - 1 - fill, w, mixer_touch_active_ && sel ? 3 : 2, fc);
        // old cap ghosts out after a move, making the physical travel readable.
        uint32_t ga = frame_ - mixer_ghost_frame_[i];
        if (mixer_ghost_frame_[i] && ga < 8) {
            int gf = (int)mixer_ghost_value_[i] * (fh - 2) / 255;
            d.rect(x + 2, fy + fh - 1 - gf, w - 4, 1,
                   with_alpha(pal::FG, (uint8_t)(150 - ga * 18)));
        }
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
        if (!master && (mixer_mute_motion_mask_ & (1u << i)) &&
            frame_ - mixer_mute_motion_frame_ < 6) {
            uint32_t ma = frame_ - mixer_mute_motion_frame_;
            int sh = (int)motion_out(ma, 6) * (fh - 2) / 255;
            d.rect(x + 1, fy, w - 2, sh,
                   with_alpha(muted ? pal::RECORD : pal::PLAY, (uint8_t)(130 - ma * 14)));
        }
        // numeric value at the bottom of the well (bright when this strip is selected)
        char vb[5];
        std::snprintf(vb, sizeof(vb), "%3d", vol);
        d.text(x + (w - 18) / 2, fy + fh - 10, vb, sel ? pal::FG : pal::FG_DIM);
    }
}

void App::mixer_fader_touch(int x, int y, bool is_move) {
    if (y < MF_Y) return;
    int i = clamp_int((x - 2) / MF_W, 0, MF_N - 1);
    const bool master = (i == 8);
    auto& song = project_.song;

    // header tap = mute toggle (tap only - drags passing through don't flip it)
    if (!is_move && y < MF_Y + MF_HEAD) {
        if (!master) {
            project_.track_mute ^= (uint8_t)(1u << i);
            mixer_mute_motion_mask_ |= (uint8_t)(1u << i);
            mixer_mute_motion_frame_ = frame_;
            if (project_.track_mute & (1u << i)) mixer_.note_off_all(i);
        }
        mixer_col_ = i;
        if (!master) mark_project_dirty();
        sync_mixer_from_song();
        return;
    }

    // fader zone: finger Y -> volume (bottom = 0, top = 255)
    int fy = MF_Y + MF_HEAD;
    int fh = MF_H - MF_HEAD;
    int v = clamp_int((fy + fh - 1 - y) * 255 / (fh - 2), 0, 255);
    uint8_t& target = master ? song.master_vol : song.track_vol[i];
    if (target != (uint8_t)v) {
        mixer_ghost_value_[i] = target;
        mixer_ghost_frame_[i] = frame_;
        target = (uint8_t)v;
        mark_project_dirty();
    }
    mixer_col_ = i;          // follow the finger with the cursor
    // push into the audio mixer right away (don't wait for update_mixer)
    sync_mixer_from_song();
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
        if (fh > 0) {
            Color fc = sel ? pal::CURSOR : pal::HEADER;
            constexpr int BANDS = 5;
            const int top = FADER_Y + FADER_H - fh;
            for (int b = 0; b < BANDS; ++b) {
                int y0 = top + fh * b / BANDS;
                int y1 = top + fh * (b + 1) / BANDS;
                uint8_t shade = (uint8_t)(100 + b * 27);
                if (y1 > y0)
                    d.rect(fx_, y0, FW, y1 - y0, lerp_color(pal::BG_HI, fc, shade));
            }
            d.rect(fx_, top, 1, fh, with_alpha(pal::FG, sel ? 70 : 36));
        }
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
                char vb[8];
                if (r == MR_DLY_TIME) {
                    const int ms = ((int)vals[r] * 32 * 1000 + AUDIO_SR / 2) / AUDIO_SR;
                    std::snprintf(vb, sizeof(vb), "%3dms", ms ? ms : 1);
                } else {
                    std::snprintf(vb, sizeof(vb), "%3d", vals[r]);
                }
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
