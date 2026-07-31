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

void warmup_wavsynth() { build_sin_lut(); }

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
// integer PolyBLEP: same edge correction as the old double implementation,
// but no software floating-point/div helpers in the per-oscillator hot path.
static inline int32_t polyblep_q15(uint32_t phase, uint32_t phase_inc) {
    if (phase_inc == 0) return 0;
    const uint32_t t = phase & 0xFFFF;
    const uint32_t dt = phase_inc > 32768 ? 32768 : phase_inc;
    if (t < dt) {
        int32_t x = (int32_t)(((uint64_t)t << 15) / dt); // q15 [0,1)
        int32_t x2 = (x * x) >> 15;
        return (x << 1) - x2 - 32767;
    }
    const uint32_t r = 0x10000 - t;
    if (r <= dt) {
        int32_t x = -(int32_t)(((uint64_t)r << 15) / dt); // q15 (-1,0]
        int32_t x2 = (x * x) >> 15;
        return x2 + (x << 1) + 32767;
    }
    return 0;
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

void Wavsynth::refresh_env_rate() {
    uint32_t duration = 0;
    int32_t sustain = params.sustain;
    switch (stage_) {
        case Stage::Attack:  duration = params.attack;  break;
        case Stage::Decay:   duration = params.decay;   break;
        case Stage::Release: duration = params.release; sustain = (int32_t)(release_start_ >> 16); break;
        default: return;
    }
    if (rate_stage_ == stage_ && rate_duration_ == duration && rate_sustain_ == sustain) return;
    rate_stage_ = stage_;
    rate_duration_ = duration;
    rate_sustain_ = sustain;
    if (stage_ == Stage::Attack) {
        uint32_t a = duration < 2 ? 2 : duration;
        env_step_ = (fx::q31)((1LL << 31) / a);
        env_target_ = (fx::q31)((1LL << 31) - 1);
    } else if (stage_ == Stage::Decay) {
        env_target_ = (fx::q31)params.sustain << 16;
        uint32_t d = duration ? duration : 1;
        env_step_ = (fx::q31)(((1LL << 31) - env_target_) / d);
    } else {
        uint32_t r = duration ? duration : 1;
        env_target_ = 0;
        env_step_ = release_start_ / (fx::q31)r;
        if (env_step_ < 1) env_step_ = 1;
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
                refresh_env_rate();
                env_ += env_step_;
                if (env_ >= (1LL << 31) - 1 || stage_pos_ >= params.attack) {
                    env_ = (1LL << 31) - 1;
                    stage_ = Stage::Decay;
                    stage_pos_ = 0;
                }
                break;
            }
            case Stage::Decay: {
                refresh_env_rate();
                env_ -= env_step_;
                if (env_ <= env_target_ || stage_pos_ >= params.decay) {
                    env_ = env_target_;
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
                refresh_env_rate();
                const uint32_t rel = params.release ? params.release : 1;
                env_ -= env_step_;
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
