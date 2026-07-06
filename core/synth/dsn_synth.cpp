// dsn_synth.cpp — DSN analog voice implementation.
//
// hot-path rules: no float/double per-sample (unlike wavsynth's polyblep which
// uses double - here polyblep is pure integer). pitch/cutoff/pw/vca-mod are
// recomputed at control rate (every CTRL_BLOCK=16 samples), oscillators, filter
// and envelopes run at full rate.
#include "dsn_synth.h"
#include <cmath>
#include <algorithm>

namespace trackr::synth {

constexpr int SR = 32000;

// === sin LUT (shared with nothing - wavsynth's is static there) ===
static fx::q15 g_dsn_sin_lut[1024];
static bool    g_dsn_sin_built = false;

static void build_dsn_sin_lut() {
    if (g_dsn_sin_built) return;
    for (int i = 0; i < 1024; ++i) {
        double a = 2.0 * 3.14159265358979 * i / 1024.0;
        g_dsn_sin_lut[i] = (fx::q15)(std::sin(a) * 32767.0);
    }
    g_dsn_sin_built = true;
}

static inline fx::q15 dsn_sin(uint32_t phase16) {
    uint32_t idx_full = (phase16 & 0xFFFF) << 4;    // 16-bit → 20-bit
    int idx  = (idx_full >> 10) & 0x3FF;
    int frac = idx_full & 0x3FF;
    fx::q15 a = g_dsn_sin_lut[idx];
    fx::q15 b = g_dsn_sin_lut[(idx + 1) & 0x3FF];
    return (fx::q15)(a + (((int32_t)(b - a) * frac) >> 10));
}

// === integer polyBLEP ===
// phase, inc: q16 period units (0..65535 = one cycle). returns q15 correction.
// t < dt:      x = t/dt,        v = 2x - x^2 - 1
// t > 1-dt:    x = (t-1)/dt,    v = x^2 + 2x + 1
static inline int32_t iblep(uint32_t phase, uint32_t inc) {
    if (inc == 0) return 0;
    uint32_t t = phase & 0xFFFF;
    uint32_t dt = inc > 32768 ? 32768 : inc;    // cap at half period
    if (t < dt) {
        int32_t x = (int32_t)(((uint64_t)t << 15) / dt);          // q15 [0..1)
        int32_t x2 = (x * x) >> 15;
        return (x << 1) - x2 - 32767;                             // q15
    }
    uint32_t r = 0x10000 - t;
    if (r <= dt) {
        int32_t x = -(int32_t)(((uint64_t)r << 15) / dt);         // q15 (-1..0]
        int32_t x2 = (x * x) >> 15;
        return x2 + (x << 1) + 32767;
    }
    return 0;
}

// one oscillator sample. phase/inc q16, pw_thr = pulse threshold in q16 units.
static inline fx::q15 dsn_osc(DsnWave w, uint32_t phase, uint32_t inc,
                              uint32_t pw_thr, uint32_t* noise) {
    uint32_t p = phase & 0xFFFF;
    switch (w) {
        case DsnWave::Sine:
            return dsn_sin(p);
        case DsnWave::Saw: {
            int32_t s = (int32_t)p - 32768;
            s -= iblep(p, inc);
            return fx::sat_q15(s);
        }
        case DsnWave::Pulse: {
            // variable-width pulse with BLEP on both edges.
            int32_t s = (p < pw_thr) ? 32767 : -32768;
            s -= iblep(p, inc);                                   // rising edge @ 0
            s += iblep((p + 0x10000 - pw_thr) & 0xFFFF, inc);     // falling edge @ pw
            // DC offset compensation: mean = 2*pw - 1
            s -= (int32_t)(pw_thr >> 1) * 2 - 32768;
            return fx::sat_q15(s);
        }
        case DsnWave::Tri: {
            int32_t v = (p < 32768) ? ((int32_t)p * 2 - 32768)
                                    : ((65535 - (int32_t)p) * 2 - 32768);
            return (fx::q15)v;
        }
        case DsnWave::Noise: {
            uint32_t s = *noise;
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            *noise = s;
            return (fx::q15)((int32_t)(s & 0xFFFF) - 32768);
        }
    }
    return 0;
}

// semitone pitch (q16) -> phase increment (q16). linear interp between the
// per-semitone table entries in fixed.cpp (max error ~0.17% mid-semitone - inaudible).
static inline uint32_t pitch_to_inc(int32_t pitch_q16) {
    if (pitch_q16 < 0) pitch_q16 = 0;
    if (pitch_q16 > (127 << 16)) pitch_q16 = 127 << 16;
    int n = pitch_q16 >> 16;
    uint32_t frac = (uint32_t)(pitch_q16 & 0xFFFF);
    int32_t a = fx::note_to_phase_inc(n, SR);
    int32_t b = fx::note_to_phase_inc(n < 127 ? n + 1 : 127, SR);
    return (uint32_t)(a + (int32_t)(((int64_t)(b - a) * frac) >> 16));
}

// === DsnEg ===
void DsnEg::step(uint32_t a, uint32_t d, fx::q15 s, uint32_t r) {
    switch (stage) {
        case Stage::Attack: {
            uint32_t aa = std::max<uint32_t>(a, 2);
            env += (fx::q31)((1LL << 31) / aa);
            if (env >= (fx::q31)((1LL << 31) - 1) || pos >= a) {
                env = (fx::q31)((1LL << 31) - 1);
                stage = Stage::Decay;
                pos = 0;
            }
            break;
        }
        case Stage::Decay: {
            fx::q31 target = (fx::q31)s << 16;
            fx::q31 stp = (fx::q31)(((1LL << 31) - target) / std::max<uint32_t>(d, 1));
            env -= stp;
            if (env <= target || pos >= d) {
                env = target;
                stage = (s == 0) ? Stage::Idle : Stage::Sustain;
            }
            break;
        }
        case Stage::Sustain:
            env = (fx::q31)s << 16;
            break;
        case Stage::Release: {
            uint32_t rr = std::max<uint32_t>(r, 1);
            fx::q31 stp = release_start / (fx::q31)rr;
            if (stp < 1) stp = 1;
            env -= stp;
            if (env <= 0 || pos >= rr) {
                env = 0;
                stage = Stage::Idle;
            }
            break;
        }
        case Stage::Idle:
            break;
    }
    ++pos;
}

// === DsnSynth ===
void DsnSynth::note_on(int note, int velocity) {
    build_dsn_sin_lut();

    int32_t target = (note + params.octave * 12) << 16;
    if (target < 0) target = 0;
    if (target > (127 << 16)) target = 127 << 16;
    pitch_target_q16_ = target;

    if (params.portamento == 0 || !active_) {
        pitch_cur_q16_ = target;      // no glide (or first note - nothing to glide from)
        glide_step_q16_ = 0;
    } else {
        // constant-time glide: reach the target in porta*16ms regardless of distance.
        // frames = porta * 512 (@32kHz: porta=127 → ~2s). step per control block.
        uint32_t frames = (uint32_t)params.portamento * 512;
        int32_t diff = pitch_target_q16_ - pitch_cur_q16_;
        int32_t blocks = (int32_t)(frames / CTRL_BLOCK);
        if (blocks < 1) blocks = 1;
        glide_step_q16_ = diff / blocks;
        if (glide_step_q16_ == 0) glide_step_q16_ = diff > 0 ? 1 : -1;
    }

    velocity_ = (fx::q15)((velocity * fx::Q15_ONE) / 127);

    // retrigger envelopes (attack continues from current level - no click)
    eg1_.gate_on();
    eg2_.gate_on();
    // MG rates (MG1 free-running across notes; reset only on fresh voice)
    mg1_.set_rate_norm(params.mg1_rate, SR / CTRL_BLOCK);
    mg2_.set_rate_norm(params.mg2_rate, SR / CTRL_BLOCK);
    if (!active_) {
        mg1_.reset_phase();
        mg2_.reset_phase();
        ph1_ = ph2_ = 0;
        filter_.reset();
    }
    filter_.type = (dsp::FilterType)params.vcf_type;

    gated_ = true;
    cut_remaining_ = 0;
    cut_amp_q15_ = fx::Q15_ONE;
    ctrl_count_ = 0;   // force control refresh on first sample
    active_ = true;
}

void DsnSynth::note_off() {
    gated_ = false;
    eg1_.gate_off();
    eg2_.gate_off();
}

void DsnSynth::cut() {
    if (!active_) return;
    cut_remaining_ = 48;   // ~1.5ms @ 32kHz
    cut_amp_q15_ = fx::Q15_ONE;
}

bool DsnSynth::render(fx::q15* out, std::size_t frames) {
    if (!active_) {
        for (std::size_t i = 0; i < frames * 2; ++i) out[i] = 0;
        return false;
    }

    const bool eg_mode = (params.vca_mode != 0);
    // drive pre-gain: 1.0 + 3*drive (q13 style: gain_q13 = 8192 + drive*3*8192/32767)
    const int32_t drive_gain_q13 = 8192 + (((int32_t)params.drive * 3 * 8192) >> 15);
    const bool has_drive = params.drive > 0;

    for (std::size_t i = 0; i < frames; ++i) {
        // === control-rate block ===
        if (ctrl_count_ <= 0) {
            ctrl_count_ = CTRL_BLOCK;

            // glide
            if (glide_step_q16_ != 0) {
                pitch_cur_q16_ += glide_step_q16_;
                if ((glide_step_q16_ > 0 && pitch_cur_q16_ >= pitch_target_q16_) ||
                    (glide_step_q16_ < 0 && pitch_cur_q16_ <= pitch_target_q16_)) {
                    pitch_cur_q16_ = pitch_target_q16_;
                    glide_step_q16_ = 0;
                }
            } else {
                pitch_cur_q16_ = pitch_target_q16_;
            }

            // MGs tick at control rate (rate set with SR/CTRL_BLOCK)
            dsp::MgOut m1 = mg1_.step();
            dsp::MgOut m2 = mg2_.step();
            const fx::q15 m1v[4] = { m1.tri, m1.saw, m1.sqr, m1.sh };
            const fx::q15 m2v[4] = { m2.tri, m2.saw, m2.sqr, m2.sh };
            mg1_val_ = m1v[params.mg1_wave & 3];
            mg2_val_ = m2v[params.mg2_wave & 3];

            const int32_t e1 = eg1_.q15();
            const int32_t e2 = eg2_.q15();

            // pitch modulation: sum of (depth*src)>>15 in q15, full scale = +/-24 semi
            // q16 offset = mod_q15 * 48  (24 semi << 16 / 32768 = 48)
            int32_t pmod = (((int32_t)params.eg1_to_pitch * e1) >> 15)
                         + (((int32_t)params.eg2_to_pitch * e2) >> 15)
                         + (((int32_t)params.mg1_to_pitch * mg1_val_) >> 15);
            int32_t pitch1 = pitch_cur_q16_ + pmod * 48;

            // VCO2 offset: semitones + cents (1 cent = 655 q16)
            int32_t pitch2 = pitch1 + ((int32_t)params.vco2_semi << 16)
                                    + (int32_t)params.vco2_detune * 655;

            inc1_ = pitch_to_inc(pitch1);
            inc2_ = pitch_to_inc(pitch2);

            // cutoff modulation
            int32_t co = (int32_t)params.cutoff
                       + (((int32_t)params.eg1_to_cutoff * e1) >> 15)
                       + (((int32_t)params.eg2_to_cutoff * e2) >> 15)
                       + (((int32_t)params.mg1_to_cutoff * mg1_val_) >> 15);
            if (co < 0) co = 0;
            if (co > fx::Q15_ONE) co = fx::Q15_ONE;
            filter_.type = (dsp::FilterType)params.vcf_type;
            filter_.set_params((fx::q15)co, params.resonance);

            // pulse width: base +/- mg2, clamp 5%..95% (q16 threshold = pw<<1)
            int32_t pw = (int32_t)params.vco1_pw
                       + (((int32_t)params.mg2_to_pw * mg2_val_) >> 15);
            constexpr int32_t PW_MIN = fx::Q15_ONE * 5 / 100;
            constexpr int32_t PW_MAX = fx::Q15_ONE * 95 / 100;
            if (pw < PW_MIN) pw = PW_MIN;
            if (pw > PW_MAX) pw = PW_MAX;
            pw_thr_ = (uint32_t)pw << 1;

            // tremolo: 1.0 + depth*mg2 (clamped 0..1)
            int32_t vm = fx::Q15_ONE + (((int32_t)params.mg2_to_vca * mg2_val_) >> 15);
            if (vm < 0) vm = 0;
            if (vm > fx::Q15_ONE) vm = fx::Q15_ONE;
            vca_mod_q15_ = vm;
        }
        --ctrl_count_;

        // === envelopes (full rate) ===
        eg1_.step(params.eg1_attack, params.eg1_decay, params.eg1_sustain, params.eg1_release);
        eg2_.step(params.eg2_attack, params.eg2_decay, params.eg2_sustain, params.eg2_release);

        // === oscillators ===
        fx::q15 o1 = dsn_osc(params.vco1_wave, ph1_, inc1_, pw_thr_, &noise_state_);
        uint32_t ph1_new = ph1_ + inc1_;
        bool wrap1 = (ph1_new & 0xFFFF) < (ph1_ & 0xFFFF) && inc1_ < 0x10000;
        ph1_ = ph1_new & 0xFFFF;

        if (params.vco2_sync && wrap1) ph2_ = 0;   // hard sync
        fx::q15 o2 = dsn_osc(params.vco2_wave, ph2_, inc2_, pw_thr_, &noise_state_);
        ph2_ = (ph2_ + inc2_) & 0xFFFF;

        // === balance mix ===
        const fx::q15 bal = params.balance;
        int32_t mix = ((int32_t)o1 * (fx::Q15_ONE - bal) + (int32_t)o2 * bal) >> 15;

        // === VCF ===
        fx::q15 flt = filter_.step(fx::sat_q15(mix));

        // === VCA ===
        int32_t amp;   // q15
        if (eg_mode) {
            amp = eg1_.q15();
        } else {
            // GATE mode: fast ramp 0<->1 (~2ms) to avoid clicks
            constexpr int32_t GATE_STEP = 512;
            if (gated_) {
                gate_amp_q15_ += GATE_STEP;
                if (gate_amp_q15_ > fx::Q15_ONE) gate_amp_q15_ = fx::Q15_ONE;
            } else {
                gate_amp_q15_ -= GATE_STEP;
                if (gate_amp_q15_ < 0) gate_amp_q15_ = 0;
            }
            amp = gate_amp_q15_;
        }
        amp = (amp * vca_mod_q15_) >> 15;
        amp = (amp * velocity_) >> 15;
        amp = (amp * params.vca_level) >> 15;

        // KIL hard-cut ramp overrides everything
        if (cut_remaining_ > 0) {
            cut_amp_q15_ -= fx::Q15_ONE / 48;
            if (cut_amp_q15_ < 0) cut_amp_q15_ = 0;
            amp = (amp * cut_amp_q15_) >> 15;
            if (--cut_remaining_ <= 0) {
                active_ = false;
                eg1_.reset(); eg2_.reset();
            }
        }

        int32_t v = ((int32_t)flt * amp) >> 15;

        // === drive (soft overdrive) ===
        if (has_drive) {
            v = (v * drive_gain_q13) >> 13;
            v = fx::soft_clip_q31(v);
        }

        fx::q15 s = fx::sat_q15(v);
        out[i * 2 + 0] = s;
        out[i * 2 + 1] = s;

        // === lifetime ===
        if (eg_mode) {
            if (eg1_.idle() && !gated_) active_ = false;
            // decay-to-zero (sustain=0) with gate still held: also dead
            if (eg1_.idle() && eg1_.env == 0 && gated_ && eg1_.stage == DsnEg::Stage::Idle
                && params.eg1_sustain == 0) active_ = false;
        } else {
            if (!gated_ && gate_amp_q15_ == 0) active_ = false;
        }
        if (!active_) {
            // zero the tail of the buffer and bail
            for (std::size_t j = i + 1; j < frames; ++j) {
                out[j * 2] = out[j * 2 + 1] = 0;
            }
            return false;
        }
    }
    return true;
}

} // namespace trackr::synth
