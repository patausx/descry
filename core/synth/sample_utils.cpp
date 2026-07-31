#include "sample_utils.h"
#include <algorithm>
#include <vector>

namespace trackr::synth {

// helper: extract the q15 value of the left channel for frame i
// for mono = data[i], for stereo = data[i*2]
static inline int16_t left_at(const Sample& s, uint32_t frame) {
    if (s.channels == 2) {
        return s.data[frame * 2];
    }
    return s.data[frame];
}

// === destructive edit ops (moved out of ui/sample_editor.cpp so the
// instrument editor's WAVE panel shares one implementation) ===

void sample_trim_norm(Sample& s, fx::q15 start_norm, fx::q15 length_norm) {
    if (s.data.empty()) return;
    uint32_t total = (uint32_t)s.data.size();
    uint32_t a = ((uint32_t)start_norm * total) >> 15;
    uint32_t len = ((uint32_t)length_norm * total) >> 15;
    if (a >= total) return;
    if (a + len > total) len = total - a;
    if (len == 0) return;
    std::vector<fx::q15> trimmed(s.data.begin() + a, s.data.begin() + a + len);
    s.data = std::move(trimmed);
    // reset chops/loops - otherwise they'd point to the wrong place
    s.loop_start = 0;
    s.loop_end   = 0;
    for (int i = 0; i < Sample::MAX_CHOPS; ++i) s.chops[i] = 0xFFFFFFFFu;
}

void sample_normalize(Sample& s) {
    if (s.data.empty()) return;
    int32_t peak = 1;
    for (auto v : s.data) { int32_t a = v < 0 ? -v : v; if (a > peak) peak = a; }
    if (peak >= 30000) return;
    int64_t scale = (int64_t)30000 * 32768 / peak;
    for (auto& v : s.data) {
        int64_t r = ((int64_t)v * scale) >> 15;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        v = (fx::q15)r;
    }
}

void sample_reverse(Sample& s) {
    if (s.data.empty()) return;
    std::reverse(s.data.begin(), s.data.end());
    s.reversed = !s.reversed;
}

// gain in dB (approximate), +/-1..3 db at a time
void sample_gain_db(Sample& s, int delta_db) {
    if (s.data.empty()) return;
    static const int32_t scale_pos[] = { 32768, 36764, 41250, 46341 }; // 0,+1,+2,+3 db
    static const int32_t scale_neg[] = { 32768, 29205, 26031, 23197 }; // 0,-1,-2,-3 db
    int idx = delta_db < 0 ? -delta_db : delta_db;
    if (idx > 3) idx = 3;
    int32_t scale = (delta_db >= 0) ? scale_pos[idx] : scale_neg[idx];
    for (auto& v : s.data) {
        int64_t r = ((int64_t)v * scale) >> 15;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        v = (fx::q15)r;
    }
}

void sample_fade_in(Sample& s, uint32_t a, uint32_t b) {
    if (s.data.empty() || b <= a) return;
    if (b > s.data.size()) b = (uint32_t)s.data.size();
    uint32_t len = b - a;
    for (uint32_t i = a; i < b; ++i) {
        int32_t g = (int32_t)((uint64_t)(i - a) * 32768 / len);
        s.data[i] = (fx::q15)(((int64_t)s.data[i] * g) >> 15);
    }
}

void sample_fade_out(Sample& s, uint32_t a, uint32_t b) {
    if (s.data.empty() || b <= a) return;
    if (b > s.data.size()) b = (uint32_t)s.data.size();
    uint32_t len = b - a;
    for (uint32_t i = a; i < b; ++i) {
        int32_t g = (int32_t)((uint64_t)(b - 1 - i) * 32768 / len);
        s.data[i] = (fx::q15)(((int64_t)s.data[i] * g) >> 15);
    }
}

void sample_auto_slice(Sample& s, int n_chops) {
    if (s.data.empty()) return;
    if (n_chops < 1) n_chops = 1;
    if (n_chops > Sample::MAX_CHOPS) n_chops = Sample::MAX_CHOPS;
    uint32_t total = (uint32_t)s.data.size();
    for (int i = 0; i < n_chops; ++i) {
        s.chops[i] = (uint32_t)((uint64_t)i * total / n_chops);
    }
    for (int i = n_chops; i < Sample::MAX_CHOPS; ++i) {
        s.chops[i] = 0xFFFFFFFFu;
    }
}

int sample_chop_count(const Sample& s) {
    int n = 0;
    for (int i = 0; i < Sample::MAX_CHOPS; ++i)
        if (s.chops[i] != 0xFFFFFFFFu) ++n;
    return n;
}

int sample_chops_sorted(const Sample& s, uint32_t* out) {
    int n = 0;
    for (int i = 0; i < Sample::MAX_CHOPS; ++i)
        if (s.chops[i] != 0xFFFFFFFFu) out[n++] = s.chops[i];
    // insertion sort (n <= 32)
    for (int i = 1; i < n; ++i) {
        uint32_t v = out[i];
        int j = i - 1;
        while (j >= 0 && out[j] > v) { out[j + 1] = out[j]; --j; }
        out[j + 1] = v;
    }
    return n;
}

// === transient auto-slice ===
// classic energy-onset detector, integer only (no float on the ARM11 path):
//   1. mean |x| per 4ms window (128 frames @32k) -> envelope
//   2. onset when env rises above trailing average by a sensitivity-scaled
//      ratio AND clears a small absolute floor (kills hiss-triggering)
//   3. refractory ~40ms so one snare doesn't fire 3 markers
//   4. marker backtracked to the local minimum before the rise (hit START,
//      not its peak), then snapped to a zero crossing
// sensitivity 0..255 maps to onset ratio ~4.0 (low) .. ~1.15 (high).
int sample_auto_slice_transients(Sample& s, int sensitivity) {
    const uint32_t total = s.num_frames();
    if (total < 256) return 0;

    constexpr uint32_t WIN = 128;            // 4ms @ 32k
    constexpr uint32_t REFRACT_WIN = 10;     // 10 windows = 40ms refractory
    const uint32_t n_win = total / WIN;
    if (n_win < 4) return 0;

    if (sensitivity < 0) sensitivity = 0;
    if (sensitivity > 255) sensitivity = 255;
    // ratio in q8: 255->294 (x1.15), 0->1024 (x4.0). linear in between.
    const int32_t ratio_q8 = 1024 - ((1024 - 294) * sensitivity) / 255;

    // envelope: mean |left| per window. heap vector - ~n_win*4 bytes, fine.
    std::vector<int32_t> env(n_win);
    int32_t peak = 1;
    for (uint32_t w = 0; w < n_win; ++w) {
        int64_t acc = 0;
        const uint32_t base = w * WIN;
        for (uint32_t i = 0; i < WIN; ++i) {
            int32_t v = left_at(s, base + i);
            acc += v < 0 ? -v : v;
        }
        env[w] = (int32_t)(acc / WIN);
        if (env[w] > peak) peak = env[w];
    }
    // absolute floor: 2% of peak envelope - below that it's noise/tail
    const int32_t floor_abs = peak / 50;

    // wipe old chops, place new ones. chop[0] is always frame 0: a trimmed
    // break starts ON a hit, but an onset at frame 0 has no rising edge to
    // detect (no trailing context). matches equal-slice behaviour too.
    for (auto& c : s.chops) c = 0xFFFFFFFFu;
    s.chops[0] = 0;
    int placed = 1;

    // trailing average over the previous 8 windows (32ms context)
    constexpr uint32_t CTX = 8;
    uint32_t last_onset_w = 0; bool have_onset = false;

    for (uint32_t w = 1; w < n_win && placed < Sample::MAX_CHOPS; ++w) {
        int64_t trail = 0; uint32_t cnt = 0;
        for (uint32_t k = (w > CTX ? w - CTX : 0); k < w; ++k) { trail += env[k]; ++cnt; }
        int32_t avg = cnt ? (int32_t)(trail / cnt) : 0;
        if (avg < 1) avg = 1;

        const bool rising = env[w] > ((int64_t)avg * ratio_q8 >> 8) && env[w] > floor_abs;
        const bool free_of_refract = !have_onset || (w - last_onset_w) >= REFRACT_WIN;
        if (!rising || !free_of_refract) continue;

        // backtrack to the local envelope minimum (start of the hit)
        uint32_t wm = w;
        while (wm > 0 && env[wm - 1] < env[wm]) --wm;
        uint32_t frame = wm * WIN;
        frame = find_zero_crossing_near(s, frame, WIN);
        // dedupe: skip if a chop is already within half a window
        bool dup = false;
        for (int i = 0; i < placed; ++i) {
            uint32_t c = s.chops[i];
            uint32_t d = c > frame ? c - frame : frame - c;
            if (d < WIN / 2) { dup = true; break; }
        }
        if (!dup) s.chops[placed++] = frame;
        last_onset_w = w; have_onset = true;
    }
    return placed;
}

uint32_t find_zero_crossing_near(const Sample& s,
                                  uint32_t frame_pos,
                                  uint32_t radius) {
    const uint32_t total = s.num_frames();
    if (total < 2) return frame_pos;

    // clamp the input into the valid range
    if (frame_pos >= total) frame_pos = total - 1;

    // if already at zero - return as is
    if (left_at(s, frame_pos) == 0) return frame_pos;

    // search symmetrically: on each iteration i we check frame_pos-i and frame_pos+i.
    // the first zero-crossing found = the nearest (since we go from 0 outward).
    int32_t prev_left  = (frame_pos > 0)         ? left_at(s, frame_pos - 1) : 0;
    int32_t prev_right = (frame_pos + 1 < total) ? left_at(s, frame_pos + 1) : 0;
    int32_t curr       = left_at(s, frame_pos);

    // i=1: check the immediate neighbors right away
    for (uint32_t i = 1; i <= radius; ++i) {
        // left: look for a transition sign(prev_left) != sign(curr_left)
        if (frame_pos >= i) {
            uint32_t l = frame_pos - i;
            int32_t v = left_at(s, l);
            // sign changed between l and l+1 (or one of them is 0)
            int32_t v_next = (i == 1) ? curr : left_at(s, l + 1);
            // zero crossing = different signs or one = 0
            if ((v == 0) ||
                (v > 0 && v_next <= 0) ||
                (v < 0 && v_next >= 0)) {
                return l;
            }
            (void)prev_left;
        }
        // right: same thing
        if (frame_pos + i < total) {
            uint32_t r = frame_pos + i;
            int32_t v = left_at(s, r);
            int32_t v_prev = (i == 1) ? curr : left_at(s, r - 1);
            if ((v == 0) ||
                (v > 0 && v_prev <= 0) ||
                (v < 0 && v_prev >= 0)) {
                return r;
            }
            (void)prev_right;
        }
    }

    // not found within the radius - return the original point (no-op for the caller)
    return frame_pos;
}

} // namespace trackr::synth
