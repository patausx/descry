#include "wavsynth.h"
#include "wavetable.h"
#include <cmath>

namespace trackr::synth {

constexpr int SR = 32000;

// === SIN LUT (1024 points, q15) - much faster than std::sin ===
// 1024 points is enough for a clean sine, lerp between points
static fx::q15 g_sin_lut[1024];
static bool    g_sin_lut_built = false;

static void build_sin_lut() {
    if (g_sin_lut_built) return;
    for (int i = 0; i < 1024; ++i) {
        double a = 2.0 * 3.14159265358979 * i / 1024.0;
        g_sin_lut[i] = (fx::q15)(std::sin(a) * 32767.0);
    }
    g_sin_lut_built = true;
}

// get sin(phase) where phase: 0..65535 = full period
// linear interpolation between LUT entries
static inline fx::q15 fast_sin(fx::uq16 phase) {
    // map 16-bit phase to 10-bit lut + 6-bit fractional
    uint32_t idx_full = (uint32_t)phase << 4;   // 16-bit → 20-bit
    int idx  = (idx_full >> 10) & 0x3FF;
    int frac = idx_full & 0x3FF;                // 0..1023
    fx::q15 a = g_sin_lut[idx];
    fx::q15 b = g_sin_lut[(idx + 1) & 0x3FF];
    int32_t diff = (int32_t)b - a;
    return (fx::q15)(a + ((diff * frac) >> 10));
}

// generate a wave sample from phase uq16 (0..0xFFFF = one period)
// PolyBLEP anti-aliasing for saw/square: smooths the discontinuities in a correct window around the wrap.
// dt = phase_inc in normalized form (0..1, where 1 = full cycle per sample). our phase_inc is in q16,
// so dt = phase_inc / 65536. PolyBLEP poly: t in [0,1], scale = dt.
static inline int32_t polyblep_q15(uint32_t phase, uint32_t phase_inc) {
    if (phase_inc == 0) return 0;
    // t in [0, 1) (normalized phase within the period)
    double t  = (double)(phase & 0xFFFF) / 65536.0;
    double dt = (double)phase_inc       / 65536.0;
    if (dt > 0.5) dt = 0.5;  // at very high frequencies the blep is meaningless
    double v = 0.0;
    if (t < dt) {
        double x = t / dt;
        v = x + x - x * x - 1.0;
    } else if (t > 1.0 - dt) {
        double x = (t - 1.0) / dt;
        v = x * x + x + x + 1.0;
    }
    return (int32_t)(v * 32767.0);
}

static fx::q15 sample_wave(WaveShape shape, fx::uq16 phase, fx::q15 /*size*/, uint32_t* noise_state, uint32_t phase_inc, uint8_t user_slot) {
    switch (shape) {
        case WaveShape::Sine:
            return fast_sin(phase);
        case WaveShape::Saw: {
            // linear saw -1 -> +1, minus polyBLEP at the wrap (anti-aliasing)
            int32_t s = (int32_t)phase - 32768;
            s -= polyblep_q15(phase, phase_inc);
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            return (fx::q15)s;
        }
        case WaveShape::Square: {
            // square = saw_up - saw_down (shifted by half a cycle)
            // BLEP on both edges of the discontinuity
            int32_t s = (phase < 32768) ? 32767 : -32768;
            s -= polyblep_q15(phase, phase_inc);
            uint32_t phase_h = (phase + 32768) & 0xFFFF;  // shifted phase for the down-edge
            s += polyblep_q15(phase_h, phase_inc);
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            return (fx::q15)s;
        }
        case WaveShape::Triangle: {
            int32_t p = static_cast<int32_t>(phase);
            if (p < 32768) return static_cast<fx::q15>(p * 2 - 32768);
            return static_cast<fx::q15>((65535 - p) * 2 - 32768);
        }
        case WaveShape::Noise: {
            // xorshift - per-instance state (otherwise all noise voices sync in mono)
            uint32_t s = *noise_state;
            if (s == 0) s = 0xCAFEBABE;
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            *noise_state = s;
            return static_cast<fx::q15>((int32_t)(s & 0xFFFF) - 32768);
        }
        case WaveShape::User:
            // user wavetable from the SD bank. 1024-pt lerp; single-cycle files
            // are usually smooth enough that aliasing stays acceptable at 32k.
            return WavetableBank::instance().sample(user_slot, phase);
    }
    return 0;
}

void Wavsynth::note_on(int note, int velocity) {
    build_sin_lut();   // lazy one-time init
    // different noise seed on each note_on - so noise retriggers aren't identical
    if (params.shape == WaveShape::Noise) {
        noise_state_ = (uint32_t)((noise_state_ * 1664525u + 1013904223u) ^ (uint32_t)note);
        if (noise_state_ == 0) noise_state_ = 0xCAFEBABE;
    }

    fx::q16 base_inc = fx::note_to_phase_inc(note, SR);

    // === unison/detune oscillator setup ===
    // this is NOT a hot path (once per note) - we can use double for the detune multipliers.
    n_osc_ = params.unison;
    if (n_osc_ < 1) n_osc_ = 1;
    if (n_osc_ > WAVSYNTH_MAX_OSC) n_osc_ = WAVSYNTH_MAX_OSC;

    // distribute detune symmetrically: osc 0 = center, 1 = +cents, 2 = -cents
    static const double det_mul[WAVSYNTH_MAX_OSC] = { 0.0, +1.0, -1.0 };
    for (int i = 0; i < n_osc_; ++i) {
        double cents = params.detune_cents * det_mul[i];
        double mul = std::pow(2.0, cents / 1200.0);
        osc_inc_[i] = (fx::q16)(base_inc * mul);
        // different starting phases - so the oscs don't start in phase (livelier)
        osc_phase_[i] = (fx::uq16)((i * 0x5555) & 0xFFFF);
        // stereo spread: osc 0 center, 1 right, 2 left
        fx::q15 sp = params.spread;
        if (n_osc_ == 1 || sp == 0) {
            osc_pan_l_[i] = fx::Q15_ONE;
            osc_pan_r_[i] = fx::Q15_ONE;
        } else {
            // pan in [-1..1] by i, scaled by spread
            double pan = det_mul[i];   // 0, +1, -1
            double s = (double)sp / fx::Q15_ONE;
            double l = 1.0 - (pan * s > 0 ? pan * s : 0.0);
            double r = 1.0 + (pan * s < 0 ? pan * s : 0.0);
            osc_pan_l_[i] = (fx::q15)(l * fx::Q15_ONE);
            osc_pan_r_[i] = (fx::q15)(r * fx::Q15_ONE);
        }
    }
    // normalization: sum of n oscs / sqrt(n) (1/1, 1/1.41, 1/1.73)
    static const fx::q15 norm[WAVSYNTH_MAX_OSC+1] = {
        fx::Q15_ONE, fx::Q15_ONE,
        (fx::q15)(fx::Q15_ONE * 71 / 100),
        (fx::q15)(fx::Q15_ONE * 58 / 100)
    };
    osc_gain_ = norm[n_osc_];

    phase_inc_ = base_inc;   // legacy mirror
    velocity_  = static_cast<fx::q15>((velocity * fx::Q15_ONE) / 127);
    stage_     = Stage::Attack;
    stage_pos_ = 0;
    env_       = 0;
    cut_remaining_ = 0;
    active_    = true;
}

void Wavsynth::note_off() {
    if (stage_ != Stage::Idle) {
        stage_     = Stage::Release;
        stage_pos_ = 0;
        release_start_ = env_;   // remember the current level - go linearly to 0 from it
    }
}

void Wavsynth::cut() {
    // KIL: ramp the envelope to zero fast (~48 samples = ~1.5ms @ 32kHz) to avoid a
    // click, then deactivate. much faster than the instrument's natural release.
    if (!active_) return;
    constexpr int32_t CUT_SAMPLES = 48;
    cut_remaining_ = CUT_SAMPLES;
    cut_step_ = env_ / CUT_SAMPLES;
    if (cut_step_ < 1) cut_step_ = 1;
}

bool Wavsynth::render(fx::q15* out, std::size_t frames) {
    if (!active_) {
        for (std::size_t i = 0; i < frames * 2; ++i) out[i] = 0;
        return false;
    }

    for (std::size_t i = 0; i < frames; ++i) {
        // KIL hard-cut takes priority: ramp env to zero, then die.
        if (cut_remaining_ > 0) {
            env_ -= cut_step_;
            if (env_ < 0) env_ = 0;
            if (--cut_remaining_ <= 0 || env_ == 0) {
                env_ = 0;
                stage_ = Stage::Idle;
                active_ = false;
            }
        } else
        // advance the envelope
        switch (stage_) {
            case Stage::Attack: {
                // attack>=2 so (1<<31)/attack fits in int32.
                // at attack=1 the step = (1<<31)/1 = 2^31 doesn't fit in q31 (max 2^31-1) = UB.
                // attack=1 and attack=2 are indistinguishable by ear (both nearly instant).
                uint32_t a = std::max<uint32_t>(params.attack, 2);
                env_ += (fx::q31)((1LL << 31) / a);
                if (env_ >= (1LL << 31) - 1 || stage_pos_ >= params.attack) {
                    env_ = (1LL << 31) - 1;
                    stage_ = Stage::Decay;
                    stage_pos_ = 0;
                }
                break;
            }
            case Stage::Decay: {
                fx::q31 target = static_cast<fx::q31>(params.sustain) << 16;
                fx::q31 step = ((1LL << 31) - target) / std::max<uint32_t>(params.decay, 1);
                env_ -= step;
                if (env_ <= target || stage_pos_ >= params.decay) {
                    env_ = target;
                    // if sustain=0 - go straight to idle (m8-style decay-only env)
                    if (params.sustain == 0) {
                        env_ = 0;
                        stage_ = Stage::Idle;
                        active_ = false;
                    } else {
                        stage_ = Stage::Sustain;
                    }
                }
                break;
            }
            case Stage::Sustain:
                env_ = static_cast<fx::q31>(params.sustain) << 16;
                break;
            case Stage::Release: {
                // LINEAR release: from release_start_ to 0 in exactly params.release samples.
                // it used to be exponential env_/release: over `release` steps it only fell to ~37%,
                // then stage_pos_>=release cut from 37% to 0 = a sharp jump = a rasp (bell/drone).
                uint32_t rel = std::max<uint32_t>(params.release, 1);
                fx::q31 step = release_start_ / (fx::q31)rel;
                if (step < 1) step = 1;   // guarantee it crawls all the way to zero
                env_ -= step;
                if (env_ <= 0 || stage_pos_ >= rel) {
                    env_ = 0;
                    stage_ = Stage::Idle;
                    active_ = false;
                }
                break;
            }
            case Stage::Idle:
                out[i*2] = out[i*2+1] = 0;
                continue;
        }
        ++stage_pos_;

        // velocity * env (common multiplier for all oscs, computed once per frame)
        fx::q15 env_q15 = static_cast<fx::q15>(env_ >> 16);
        fx::q15 amp = fx::mul_q15(fx::mul_q15(env_q15, velocity_), osc_gain_);

        // === sum the unison oscillators ===
        int32_t acc_l = 0, acc_r = 0;
        for (int o = 0; o < n_osc_; ++o) {
            fx::q15 w = sample_wave(params.shape,
                                    static_cast<fx::uq16>(osc_phase_[o] & 0xFFFF),
                                    params.size, &noise_state_, (uint32_t)osc_inc_[o],
                                    params.user_slot);
            fx::q15 v = fx::mul_q15(w, amp);
            acc_l += fx::mul_q15(v, osc_pan_l_[o]);
            acc_r += fx::mul_q15(v, osc_pan_r_[o]);
            osc_phase_[o] = (osc_phase_[o] + osc_inc_[o]) & 0xFFFF;
        }
        out[i*2 + 0] = fx::sat_q15(acc_l);
        out[i*2 + 1] = fx::sat_q15(acc_r);
    }

    return active_;
}

} // namespace trackr::synth
