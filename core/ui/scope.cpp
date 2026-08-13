// App: master scope renderer and fullscreen performance visualizer.
#include "app.h"
#include "ui_internal.h"
#include <cstdio>

namespace trackr::ui {

// === draw ===

// fullscreen oscilloscope (performance visualizer) - the entire top screen 400x240
// master wave in the top band + 8 per-track mini scopes in a 4x2 grid below.

// === master scope renderer (shared: bottom strip + fullscreen band) ===
// honors scope_style_. draws INSIDE (x,y,w,h) - frame/grid stay with the caller.
void App::draw_master_scope(Draw& d, int x, int y, int w, int h, bool filled) {
    const auto& mxr = mixer_;
    constexpr int N = (int)audio::Mixer::SCOPE_SIZE;
    const std::size_t pos = mxr.scope_write_pos;
    const int mid = y + h / 2;
    const int amp = h / 2 - 1;
    auto sample_at = [&](const fx::q15* buf, int col) -> int32_t {
        return buf[(pos + (std::size_t)(col * N / w)) % (std::size_t)N];
    };

    switch (scope_style_) {
    case 1: { // BARS - mirrored peak bars every 4px (chunky VU look)
        constexpr int BW = 4;
        for (int bx = 0; bx + BW <= w; bx += BW) {
            int32_t peak = 0;
            for (int i = 0; i < BW; ++i) {
                int32_t v = sample_at(mxr.scope, bx + i);
                if (v < 0) v = -v;
                if (v > peak) peak = v;
            }
            int bh = (int)((peak * amp) / 32768);
            if (bh < 1) bh = 1;
            d.rect(x + bx, mid - bh, BW - 1, bh, pal::PLAY);
            d.rect(x + bx, mid,      BW - 1, bh, pal::PLAY_BG);   // dim mirror below
        }
        break;
    }
    case 2: { // X-Y - goniometer (lissajous), studio-style:
        // rotated 45deg into mid/side space - mono = vertical needle, stereo
        // width blooms the figure sideways (reverb/detune open it like a flower).
        // samples are CONNECTED line segments with phosphor decay - a naive
        // dot-per-sample L-vs-R plot reads as "a thin diagonal line", useless.
        const int cx = x + w / 2;
        const int r  = (h / 2 - 1 < w / 2 - 1) ? h / 2 - 1 : w / 2 - 1;

        // dim vertical axis = the mono needle's home (mid/side space)
        d.rect(cx, y, 1, h, pal::BG_HI);

        // auto-gain with slew: instant peak made the figure JUMP every frame.
        // fast attack / slow release peak follower = a real scope's range knob
        // being turned smoothly instead of teleporting. floor ~ -24dBFS keeps
        // silence from amplifying into noise.
        int32_t peak = 2048;
        for (int i = 0; i < N; ++i) {
            int32_t l = mxr.scope_l[i]; if (l < 0) l = -l; if (l > peak) peak = l;
            int32_t rr = mxr.scope_r[i]; if (rr < 0) rr = -rr; if (rr > peak) peak = rr;
        }
        if (peak > scope_xy_peak_) scope_xy_peak_ = peak;                       // attack: instant
        else scope_xy_peak_ -= (scope_xy_peak_ - peak) / 16;                    // release: ~1/16 per frame
        if (scope_xy_peak_ < 2048) scope_xy_peak_ = 2048;
        const int32_t g = scope_xy_peak_;

        // QUAD BUDGET: every pixel below is one citro2d object from the SHARED
        // per-frame pool (top+bottom screens!). unbounded bresenham on hot
        // transients ate the whole pool and the bottom screen stopped drawing
        // (the "flickering / everything vanishes" bug). hard caps:
        //  - budget scales with the field size (strip gets less than fullscreen)
        //  - overlong segments (transient jumps) get SKIPPED - a real CRT beam
        //    moving that fast wouldn't light the phosphor anyway.
        int budget = w * h / 8;                    // fullscreen band ~7k, strip ~780
        if (budget > 6000) budget = 6000;
        const int MAX_SEG = (r > 40) ? 48 : 16;    // px; longer jumps = beam blanked

        // tiny bresenham - Draw has no line primitive and only this style needs one
        auto seg = [&](int x0, int y0, int x1, int y1, Color c) {
            int dx = x1 - x0; if (dx < 0) dx = -dx;
            int dy = y1 - y0; if (dy < 0) dy = -dy;
            int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
            int err = dx - dy;
            for (;;) {
                d.rect(x0, y0, 1, 1, c);
                if (--budget <= 0) return;
                if (x0 == x1 && y0 == y1) break;
                int e2 = 2 * err;
                if (e2 > -dy) { err -= dy; x0 += sx; }
                if (e2 <  dx) { err += dx; y0 += sy; }
            }
        };

        const Color mid_c = lerp_color(pal::PLAY_BG, pal::PLAY, 128);
        int ppx = 0, ppy = 0;
        bool first = true;
        for (int i = 0; i < N && budget > 0; ++i) {
            std::size_t idx = (pos + (std::size_t)i) % (std::size_t)N;
            int32_t l = mxr.scope_l[idx], rv = mxr.scope_r[idx];
            // 45deg rotation: X = side (L-R), Y = mid (L+R). sums span 2*peak.
            int px = cx  + (int)(((l - rv) * r) / (2 * g));
            int py = mid - (int)(((l + rv) * r) / (2 * g));
            px = clamp_int(px, x, x + w - 1);
            py = clamp_int(py, y, y + h - 1);
            if (!first) {
                int adx = px - ppx; if (adx < 0) adx = -adx;
                int ady = py - ppy; if (ady < 0) ady = -ady;
                if (adx + ady <= MAX_SEG) {
                    // phosphor decay: oldest half dim, mid zone medium, newest bright
                    Color c = (i > N - N / 4) ? pal::PLAY
                            : (i > N / 2)     ? mid_c
                                              : pal::PLAY_BG;
                    seg(ppx, ppy, px, py, c);
                }   // else: beam blanked on the jump - just move the pen
            }
            ppx = px; ppy = py; first = false;
        }
        break;
    }
    default: { // 0: WAVE - filled envelope + connected line (the classic)
        int prev_y = mid;
        for (int col = 0; col < w; ++col) {
            int yy = mid - (int)((sample_at(mxr.scope, col) * amp) / 32768);
            if (yy < y) yy = y;
            if (yy >= y + h) yy = y + h - 1;
            if (filled) {
                int top = yy < mid ? yy : mid;
                int hgt = yy < mid ? (mid - yy) : (yy - mid);
                if (hgt > 0) d.rect(x + col, top, 1, hgt, pal::PLAY_BG);
            }
            int ly0 = prev_y < yy ? prev_y : yy;
            int ly1 = prev_y < yy ? yy : prev_y;
            d.rect(x + col, ly0, 1, (ly1 - ly0) + 1, pal::PLAY);
            prev_y = yy;
        }
        break;
    }
    }
}

void App::draw_scope_fullscreen(Draw& d) {
    constexpr int W = 400, H = 240;
    constexpr int MH = 150;              // master wave band height
    constexpr int MID = MH / 2 + 10;     // master center axis (offset below the title)

    // Fullscreen performance view stays visually stable while playback runs.
    // Motion belongs to the waveform itself; no beat-synced glow/wash is added.

    // dark background
    d.rect(0, 0, W, H, pal::BG);

    // grid (subtle) - horizontals at 1/4, 3/4 of the master band
    // (skip for X-Y: the goniometer draws its own axis)
    if (scope_style_ != 2) {
        d.rect(0, 10 + MH / 4,     W, 1, pal::BG_HI);
        d.rect(0, 10 + 3 * MH / 4, W, 1, pal::BG_HI);
        // verticals every 50px
        for (int gx = 50; gx < W; gx += 50) d.rect(gx, 10, 1, MH, pal::BG_HI);
        // center axis
        d.rect(0, MID, W, 1, pal::GRID);
    }

    // master band: one renderer, four styles (WAVE/BARS/DOTS/X-Y)
    draw_master_scope(d, 0, 12, W, MH - 4, false);

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
    // Footer uses fixed non-overlapping zones: compact exit hint at left,
    // diagnostics after x=104. The old long hint reached x=132 while metrics
    // started at x=124, so both strings physically drew over each other.
    d.text(6, H - 10, "L+SEL EXIT", pal::FG_DIM, 1);
    char perf[44];
    const unsigned dsp10 = (unsigned)(debug_render_us / 100);
    const unsigned max10 = (unsigned)(debug_render_max_us / 100);
    std::snprintf(perf, sizeof(perf), "DSP %u.%ums  MAX %u.%ums",
                  dsp10 / 10, dsp10 % 10, max10 / 10, max10 % 10);
    d.text(104, H - 10, perf, debug_render_max_us > 24000 ? pal::RECORD : pal::FG_DIM, 1);

    // underrun diagnosis: shows real starvation events on-device. 0 = clean;
    // a growing number while playing = the synth can't keep the dsp fed.
    if (debug_xruns > 0) {
        char xb[20];
        std::snprintf(xb, sizeof(xb), "XRUN %lu", (unsigned long)debug_xruns);
        d.text(W - 6 * 12, H - 10, xb, pal::RECORD, 1);
    }
}

} // namespace trackr::ui
