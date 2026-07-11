#include "sampler.h"
#include <cmath>
#include <algorithm>

namespace trackr::synth {

// === semitone ratio LUT: 2^((n-60)/12) in q16.16 ===
// filled lazily. n=60 (root) -> 1.0, n=72 -> 2.0, n=48 -> 0.5
// range [0..127] centered on 60
static int32_t g_semitone_ratio_q16[128];
static bool    g_semi_lut_built = false;

static void build_semi_lut() {
    if (g_semi_lut_built) return;
    for (int n = 0; n < 128; ++n) {
        double r = std::pow(2.0, (n - 60) / 12.0);
        // clamp so we don't exceed int32 (max ratio at n=127 -> 2^5.58 ~= 47.8)
        double q = r * 65536.0;
        if (q > 2147483000.0) q = 2147483000.0;
        g_semitone_ratio_q16[n] = (int32_t)q;
    }
    g_semi_lut_built = true;
}

// convert a semitone offset to a q16.16 ratio (via the table)
// note here = "how many semitones from root"
static inline int32_t semi_ratio_q16(int semitones_from_root) {
    int idx = 60 + semitones_from_root;
    if (idx < 0)   idx = 0;
    if (idx > 127) idx = 127;
    return g_semitone_ratio_q16[idx];
}

// hermite (catmull-rom) cubic interpolation between 4 q15 points
// frac in q16 in [0, 65536)
static inline fx::q15 hermite4_q15(fx::q15 y0, fx::q15 y1, fx::q15 y2, fx::q15 y3, uint32_t frac) {
    // catmull-rom coefficients:
    // a = -0.5*y0 + 1.5*y1 - 1.5*y2 + 0.5*y3
    // b =      y0 - 2.5*y1 + 2.0*y2 - 0.5*y3
    // c = -0.5*y0          + 0.5*y2
    // d =                y1
    // out = ((a*t + b)*t + c)*t + d
    // compute in int32 (coefficient range up to ~5*32767=163K, fits easily)
    int32_t Y0 = y0, Y1 = y1, Y2 = y2, Y3 = y3;
    int32_t a = (-Y0 + 3*Y1 - 3*Y2 + Y3);            // *0.5 -> defer: multiply by t/2
    int32_t b = (2*Y0 - 5*Y1 + 4*Y2 - Y3);           // *0.5
    int32_t c = (-Y0 + Y2);                          // *0.5
    int32_t d = Y1;

    // t in q15 (frac >> 1 to go 16-bit -> 15-bit)
    int32_t t = (int32_t)(frac >> 1);                // q15 [0, 32768)

    // ((a*t/2 + b/2)*t + c/2)*t + d (+ accumulator correction for /2)
    // safe to do via int64 to avoid overflow
    int64_t acc = (int64_t)a * t;                    // q15 * q15 -> q30, *0.5 -> divide at the end
    acc >>= 15;                                      // -> q15
    acc += b;                                        // q15 (b in q0; we need b in the same scale - well, b is an int)
    // simplify: recompute directly without a*0.5/b*0.5/c*0.5 - multiply everything by 2 for integer math
    // -> out = (((a*t + b*2)/2*t + c*2)/2 *t + d*2)/2 ... messy. better to do it all in double inside. NO.
    // let's redo it cleaner:

    // full formula with explicit /2:
    // out = ((a*t + b)*t + c)*t / 2 + d  where a=-y0+3y1-3y2+y3, b=2y0-5y1+4y2-y3, c=-y0+y2
    // and t here is in [0,1]. in q15 scale:
    int64_t at_b  = ((int64_t)a * t) >> 15;          // q15
    at_b += b;
    int64_t at_b_t_c = (at_b * t) >> 15;             // q15
    at_b_t_c += c;
    int64_t res = (at_b_t_c * t) >> 15;              // q15
    res = (res >> 1) + d;                             // *0.5 + d

    if (res >  32767) res =  32767;
    if (res < -32768) res = -32768;
    return (fx::q15)res;
}

// reads an interleaved frame with bounds protection
// stereo: ch=0 = L, ch=1 = R
// mono: ch is ignored (always the same frame)
static inline fx::q15 read_frame_safe(const Sample& s, int64_t frame, int ch, uint32_t total_frames) {
    if (frame < 0 || (uint64_t)frame >= total_frames) return 0;
    if (s.channels == 2) {
        return s.data[(uint32_t)frame * 2 + ch];
    }
    return s.data[(uint32_t)frame];
}

// exp velocity curve: v^2 / 127^2 -> q15
static inline fx::q15 vel_curve(int v) {
    if (v <= 0)   return 0;
    if (v >= 127) return fx::Q15_ONE;
    int32_t vv = v * v;                              // 0..16129
    return (fx::q15)((vv * fx::Q15_ONE) / (127 * 127));
}

void Sampler::note_on(int note, int velocity) {
    build_semi_lut();
    // bounds check - protect against garbage sample_slot in the union
    if (params.sample_slot < 0 || params.sample_slot >= SAMPLE_BANK_SIZE) {
        active_ = false;
        return;
    }
    auto& s = SampleBank::instance().slot(params.sample_slot);
    if (s.empty()) { active_ = false; return; }

    uint32_t total = s.num_frames();

    // === M8-style play mode: derive reverse/loop from play_mode ===
    const PlayMode pm = params.play_mode;
    const bool want_reverse = (pm == PlayMode::Rev || pm == PlayMode::RevLoop);
    const bool want_loop    = (pm == PlayMode::FwdLoop || pm == PlayMode::RevLoop);
    // keep the legacy bools in sync so anything still reading them stays correct
    params.reverse = want_reverse;
    params.loop    = want_loop;

    // play boundaries from start/length (q15 fractions of total)
    play_start_ = ((uint64_t)params.start  * total) >> 15;
    uint32_t end_frame = ((uint64_t)params.length * total) >> 15;
    if (end_frame > total) end_frame = total;
    if (play_start_ >= total) play_start_ = total > 0 ? total - 1 : 0;
    play_end_ = end_frame;

    // === slice selection ===
    // chromatic_slices: the played note (offset from root) selects the slice.
    // otherwise params.slice is used. slice 0 = whole sample (use start/length).
    int slice_idx = 0;
    if (params.chromatic_slices) {
        int n = note - s.root_note;            // 0 = first slice
        if (n >= 0 && n < Sample::MAX_CHOPS) slice_idx = n + 1;
        else slice_idx = 0;                    // out of range -> whole sample
    } else {
        slice_idx = params.slice;
    }
    if (slice_idx > 0 && slice_idx <= Sample::MAX_CHOPS) {
        uint32_t a = s.chops[slice_idx - 1];
        if (a != 0xFFFFFFFFu && a < total) {
            // slice end = next valid chop after 'a', else play_end_/total
            uint32_t b = total;
            for (int i = 0; i < Sample::MAX_CHOPS; ++i) {
                uint32_t c = s.chops[i];
                if (c != 0xFFFFFFFFu && c > a && c < b) b = c;
            }
            play_start_ = a;
            play_end_   = b;
        }
    }

    // loop boundaries - taken from the Sample itself (not from params)
    loop_active_ = want_loop &&
                   s.loop_end > s.loop_start &&
                   s.loop_end <= total;
    loop_start_ = s.loop_start;
    loop_end_   = s.loop_end;
    // no explicit loop points set -> loop the whole play window (m8 behaviour).
    // FWDLOOP/REVLOOP used to silently do nothing until the user dragged loop
    // markers in the WAVE panel - "the loop feature is just broken" (discord).
    if (want_loop && !loop_active_ && play_end_ > play_start_) {
        loop_start_  = play_start_;
        loop_end_    = play_end_;
        loop_active_ = true;
    }

    // pitch ratio via the table + fine cents
    int semis = note - s.root_note;
    // when slices are picked chromatically, each slice plays at root pitch
    // (the note only chooses which slice, not the transposition)
    if (params.chromatic_slices) semis = 0;
    int32_t ratio_q16 = semi_ratio_q16(semis);
    // fine cents: +/-50 cents = +/-0.5 semitone. cheap via a linear approximation:
    // 2^(c/1200) ~= 1 + c*0.0005776 for small c. in q16: c * 0.0005776 * 65536 ~= c * 37.85
    if (params.fine_cents != 0) {
        int32_t cent_adj = (int32_t)params.fine_cents * 38;     // ~q16 correction
        int64_t r = (int64_t)ratio_q16 + (((int64_t)cent_adj * ratio_q16) >> 16);
        if (r < 1) r = 1;
        ratio_q16 = (int32_t)r;
    }

    // reverse: the sign of pos_inc becomes negative, and we start from the end of the window
    if (want_reverse) {
        pos_inc_ = -ratio_q16;
        pos_hi_ = (int64_t)play_end_ - 1;
        if (pos_hi_ < (int64_t)play_start_) pos_hi_ = play_start_;
        pos_lo_ = 0;
    } else {
        pos_inc_ = ratio_q16;
        pos_hi_ = (int64_t)play_start_;
        pos_lo_ = 0;
    }

    velocity_ = vel_curve(velocity);
    gated_ = true;

    // ADSR start
    stage_ = (params.attack > 0) ? Stage::Attack : Stage::Decay;
    stage_pos_ = 0;
    env_ = (params.attack > 0) ? 0 : ((fx::q31)1 << 30);   // q30: 1.0
    release_start_env_ = 0;
    active_ = true;
}

void Sampler::note_off() {
    if (!gated_) return;
    gated_ = false;
    // remember env at the moment of release so the exp release is smooth from the current level
    release_start_env_ = env_;
    stage_ = Stage::Release;
    stage_pos_ = 0;
}

bool Sampler::render(fx::q15* out, std::size_t frames) {
    if (!active_) {
        for (std::size_t i = 0; i < frames * 2; ++i) out[i] = 0;
        return false;
    }

    auto& s = SampleBank::instance().slot(params.sample_slot);
    if (s.empty()) { active_ = false; return false; }

    uint32_t total = s.num_frames();

    // crossfade length - min(256, 1/4 of the loop length)
    uint32_t xfade_len = 256;
    if (loop_active_) {
        uint32_t loop_len = loop_end_ - loop_start_;
        if (xfade_len > loop_len / 4) xfade_len = loop_len / 4;
    }

    // anti-click fadein at the start - 32 frame ramp (for note_on without dying)
    constexpr uint32_t FADEIN_LEN = 32;
    // anti-click fadeout before the end of the sample (if not loop) - 128 frame
    constexpr uint32_t FADEOUT_LEN = 128;

    constexpr fx::q31 ENV_ONE = (fx::q31)1 << 30;       // 1.0 in q30

    for (std::size_t i = 0; i < frames; ++i) {
        // === ADSR envelope tick ===
        switch (stage_) {
            case Stage::Attack: {
                if (params.attack > 0) {
                    env_ += ENV_ONE / (fx::q31)params.attack;
                }
                if (env_ >= ENV_ONE || stage_pos_ >= params.attack) {
                    env_ = ENV_ONE;
                    stage_ = Stage::Decay;
                    stage_pos_ = 0;
                }
                break;
            }
            case Stage::Decay: {
                if (params.decay == 0) {
                    stage_ = Stage::Sustain;
                    env_ = (fx::q31)params.sustain << 15;   // q15 -> q30
                } else {
                    fx::q31 target = (fx::q31)params.sustain << 15;
                    fx::q31 step = (ENV_ONE - target) / (fx::q31)params.decay;
                    env_ -= step;
                    if (env_ <= target || stage_pos_ >= params.decay) {
                        env_ = target;
                        stage_ = Stage::Sustain;
                    }
                }
                break;
            }
            case Stage::Sustain:
                env_ = (fx::q31)params.sustain << 15;
                break;
            case Stage::Release: {
                // exponential release: env *= coeff per frame
                // coeff chosen so that after params.release frames env falls to ~0
                // exp: env(n) = start * (1 - 1/release)^n -> after release frames -> ~37%
                // we do a linear fade from release_start_env_ -> 0 for predictability (like m8)
                if (params.release == 0) {
                    env_ = 0;
                    stage_ = Stage::Idle;
                    active_ = false;
                } else {
                    // linear (predictable, no "infinite tail")
                    fx::q31 step = release_start_env_ / (fx::q31)params.release;
                    env_ -= step;
                    if (env_ <= 0 || stage_pos_ >= params.release) {
                        env_ = 0;
                        stage_ = Stage::Idle;
                        active_ = false;
                    }
                }
                break;
            }
            case Stage::Idle:
                out[i*2] = out[i*2+1] = 0;
                continue;
        }
        ++stage_pos_;

        // === bounds + loop check ===
        bool went_out = false;
        if (pos_inc_ >= 0) {
            if ((uint64_t)pos_hi_ >= play_end_) went_out = true;
            if (loop_active_ && (uint64_t)pos_hi_ >= loop_end_) {
                // wrap to the loop start
                int64_t over = pos_hi_ - (int64_t)loop_end_;
                pos_hi_ = (int64_t)loop_start_ + over;
                went_out = false;
            }
        } else {
            // reverse
            if (pos_hi_ < (int64_t)play_start_) {
                if (loop_active_) {
                    int64_t over = (int64_t)loop_start_ - pos_hi_;
                    pos_hi_ = (int64_t)loop_end_ - over - 1;
                    went_out = false;
                } else {
                    went_out = true;
                }
            }
        }
        if (went_out) {
            active_ = false;
            out[i*2] = out[i*2+1] = 0;
            continue;
        }

        // === hermite cubic interpolation for L and R ===
        int64_t p0 = pos_hi_ - 1;
        int64_t p1 = pos_hi_;
        int64_t p2 = pos_hi_ + 1;
        int64_t p3 = pos_hi_ + 2;

        fx::q15 L0 = read_frame_safe(s, p0, 0, total);
        fx::q15 L1 = read_frame_safe(s, p1, 0, total);
        fx::q15 L2 = read_frame_safe(s, p2, 0, total);
        fx::q15 L3 = read_frame_safe(s, p3, 0, total);
        fx::q15 sample_l = hermite4_q15(L0, L1, L2, L3, pos_lo_);

        fx::q15 sample_r;
        if (s.channels == 2) {
            fx::q15 R0 = read_frame_safe(s, p0, 1, total);
            fx::q15 R1 = read_frame_safe(s, p1, 1, total);
            fx::q15 R2 = read_frame_safe(s, p2, 1, total);
            fx::q15 R3 = read_frame_safe(s, p3, 1, total);
            sample_r = hermite4_q15(R0, R1, R2, R3, pos_lo_);
        } else {
            sample_r = sample_l;
        }

        // === loop crossfade ===
        // in the window [loop_end - xfade_len, loop_end) we mix in the future loop_start+offset
        if (loop_active_ && pos_inc_ >= 0 && xfade_len > 0) {
            uint32_t xfade_zone_start = loop_end_ - xfade_len;
            if ((uint64_t)pos_hi_ >= xfade_zone_start && (uint64_t)pos_hi_ < loop_end_) {
                uint32_t into_zone = (uint32_t)(pos_hi_ - (int64_t)xfade_zone_start);
                int64_t alt_p = (int64_t)loop_start_ + into_zone;
                fx::q15 AL = read_frame_safe(s, alt_p, 0, total);
                fx::q15 AR = (s.channels == 2) ? read_frame_safe(s, alt_p, 1, total) : AL;
                // linear crossfade: at into_zone=0 -> 100% main / 0% alt, at into_zone=xfade_len -> 0/100
                fx::q15 alt_w = (fx::q15)(((uint32_t)fx::Q15_ONE * into_zone) / xfade_len);
                fx::q15 main_w = fx::Q15_ONE - alt_w;
                sample_l = fx::sat_q15((fx::q31)fx::mul_q15(sample_l, main_w) + fx::mul_q15(AL, alt_w));
                sample_r = fx::sat_q15((fx::q31)fx::mul_q15(sample_r, main_w) + fx::mul_q15(AR, alt_w));
            }
        }

        // === fadein at the start (when pos is close to play_start_) ===
        if (pos_inc_ >= 0 && !loop_active_) {
            uint32_t since_start = (uint32_t)(pos_hi_ - (int64_t)play_start_);
            if (since_start < FADEIN_LEN) {
                int32_t fade = (int32_t)((since_start * fx::Q15_ONE) / FADEIN_LEN);
                sample_l = fx::mul_q15(sample_l, (fx::q15)fade);
                sample_r = fx::mul_q15(sample_r, (fx::q15)fade);
            }
        }

        // === fadeout before the end of the sample (if not loop) ===
        if (!loop_active_ && pos_inc_ >= 0 && play_end_ > FADEOUT_LEN) {
            uint32_t fadeout_zone = play_end_ - FADEOUT_LEN;
            if ((uint64_t)pos_hi_ >= fadeout_zone) {
                uint32_t into_zone = (uint32_t)(pos_hi_ - (int64_t)fadeout_zone);
                if (into_zone < FADEOUT_LEN) {
                    int32_t fade = (int32_t)(((FADEOUT_LEN - into_zone) * fx::Q15_ONE) / FADEOUT_LEN);
                    sample_l = fx::mul_q15(sample_l, (fx::q15)fade);
                    sample_r = fx::mul_q15(sample_r, (fx::q15)fade);
                }
            }
        }

        // === apply env, velocity, gain ===
        fx::q15 env_q15 = (fx::q15)(env_ >> 15);    // q30 -> q15
        fx::q15 amp = fx::mul_q15(env_q15, velocity_);
        amp = fx::mul_q15(amp, params.gain);

        sample_l = fx::mul_q15(sample_l, amp);
        sample_r = fx::mul_q15(sample_r, amp);

        out[i*2 + 0] = sample_l;
        out[i*2 + 1] = sample_r;

        // === advance the position ===
        int64_t new_lo = (int64_t)pos_lo_ + pos_inc_;
        // pos_inc_ signed -> new_lo may be negative
        // hi += new_lo / 65536, lo = new_lo mod 65536 (in a 2's complement-friendly way)
        int64_t carry = new_lo >> 16;
        pos_hi_ += carry;
        pos_lo_ = (fx::uq16)(new_lo & 0xFFFF);
    }

    return active_;
}

} // namespace trackr::synth
