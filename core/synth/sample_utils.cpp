#include "sample_utils.h"
#include <algorithm>
#include <vector>

namespace trackr::synth {

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

// helper: extract the q15 value of the left channel for frame i
// for mono = data[i], for stereo = data[i*2]
static inline int16_t left_at(const Sample& s, uint32_t frame) {
    if (s.channels == 2) {
        return s.data[frame * 2];
    }
    return s.data[frame];
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
