#include "fm.h"
#include "../audio/fixed.h"
#include <cmath>
#include <cstring>

namespace trackr::synth {

constexpr int SR = 32000;
constexpr fx::q31 ENV_ONE = (fx::q31)1 << 30;

// === ratio table (16 values, q16.16) ===
// classic m8/DX-style ratios
static const float RATIOS[16] = {
    0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f, 5.0f,
    6.0f, 7.0f, 8.0f, 9.0f, 10.0f, 12.0f, 14.0f, 16.0f
};
static const char* RATIO_NAMES[16] = {
    "0.5", "1.0", "1.5", "2.0", "2.5", "3.0", "4.0", "5.0",
    "6.0", "7.0", "8.0", "9.0", "10",  "12",  "14",  "16"
};
static const char* ALGO_NAMES[FM_NUM_ALGOS] = {
    "STACK",    // 1→2→3→4
    "PAIR",     // 1→2, 3→4
    "Y-FAN",    // 1→2,3,4
    "3->1",     // 1,2,3 → 4
    "TREE",     // 1→2,3 / 2→4
    "STACK+",   // 1→2→3, 4
    "ADD",      // 1+2+3+4
    "BRANCH"    // 1→3, 2→3, 4
};

int32_t fm_ratio_q16(int idx) {
    if (idx < 0) idx = 0;
    if (idx > 15) idx = 15;
    return (int32_t)(RATIOS[idx] * 65536.0f);
}
const char* fm_ratio_name(int idx) {
    if (idx < 0 || idx > 15) return "?";
    return RATIO_NAMES[idx];
}
const char* fm_algo_name(int idx) {
    if (idx < 0 || idx >= FM_NUM_ALGOS) return "?";
    return ALGO_NAMES[idx];
}

static const char* OP_WAVE_NAMES[4] = { "SIN", "TRI", "SAW", "SQR" };
const char* fm_op_wave_name(int idx) {
    if (idx < 0 || idx > 3) return "?";
    return OP_WAVE_NAMES[idx];
}

// === algorithms: per operator (algo, op) - bitmask of modulators and a carrier flag ===
// bit i modulates op[j] if set in mod_mask[algo][j]
// carrier_mask[algo] - bitmask of which ops are carriers (output)
struct Algo {
    uint8_t mod_mask[FM_NUM_OPS];   // mod_mask[j] = ops that modulate op[j] (bit i = op_i)
    uint8_t carrier_mask;           // bit i = op_i at the output
};

static const Algo ALGOS[FM_NUM_ALGOS] = {
    // 0 STACK: 1->2->3->4 (only op4 is a carrier)
    { {0b0000, 0b0001, 0b0010, 0b0100}, 0b1000 },
    // 1 PAIR: 1→2, 3→4 (carriers: 2,4)
    { {0b0000, 0b0001, 0b0000, 0b0100}, 0b1010 },
    // 2 Y-FAN: 1→2, 1→3, 1→4 (carriers: 2,3,4)
    { {0b0000, 0b0001, 0b0001, 0b0001}, 0b1110 },
    // 3 3-INTO-1: 1,2,3 → 4 (carrier: 4)
    { {0b0000, 0b0000, 0b0000, 0b0111}, 0b1000 },
    // 4 TREE: 1→2, 1→3, 2→4 (carriers: 3,4)
    { {0b0000, 0b0001, 0b0001, 0b0010}, 0b1100 },
    // 5 STACK+: 1→2→3, 4 standalone (carriers: 3,4)
    { {0b0000, 0b0001, 0b0010, 0b0000}, 0b1100 },
    // 6 ADD: 1+2+3+4 (all carriers)
    { {0b0000, 0b0000, 0b0000, 0b0000}, 0b1111 },
    // 7 BRANCH: 1→3, 2→3, op4 standalone (carriers: 3,4)
    { {0b0000, 0b0000, 0b0011, 0b0000}, 0b1100 }
};

// topology accessors for the UI algorithm diagram
uint8_t fm_algo_carrier_mask(int algo) {
    return ALGOS[algo & 7].carrier_mask;
}
uint8_t fm_algo_mod_mask(int algo, int op) {
    if (op < 0 || op >= FM_NUM_OPS) return 0;
    return ALGOS[algo & 7].mod_mask[op];
}

// === sin LUT (1024 points q15) - logically shared with wavsynth but local ===
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
// fast_sin: phase in q16.16, take mod 1.0 -> 1024 lookup + 6-bit fractional lerp
static inline fx::q15 fast_sin(uint32_t phase_q16) {
    // take the low 16 bits as the period phase, widen to 20 bits for indexing
    uint32_t p16 = phase_q16 & 0xFFFF;
    uint32_t idx_full = p16 << 4;     // 16 → 20
    int idx  = (idx_full >> 10) & 0x3FF;
    int frac = idx_full & 0x3FF;
    fx::q15 a = g_sin_lut[idx];
    fx::q15 b = g_sin_lut[(idx + 1) & 0x3FF];
    int32_t diff = (int32_t)b - a;
    return (fx::q15)(a + ((diff * frac) >> 10));
}

// op oscillator: wave 0=SIN 1=TRI 2=SAW 3=SQR, phase q16.16 (low 16 bits = cycle).
// tri/saw/sqr are computed straight from phase - naive shapes, NO blep. that's fine
// (and even desirable) here: FM operators want harmonic-rich raw waves, and
// aliasing at 32kHz on modulator ops is masked by the modulation itself.
static inline fx::q15 op_wave(uint8_t wave, uint32_t phase_q16) {
    uint32_t p = phase_q16 & 0xFFFF;
    switch (wave) {
        default:
        case 0: return fast_sin(phase_q16);
        case 1: {   // TRI: -1 → +1 → -1
            int32_t v = (p < 32768) ? ((int32_t)p * 2 - 32768)
                                    : ((65535 - (int32_t)p) * 2 - 32768);
            return (fx::q15)v;
        }
        case 2:     // SAW: ramp -1 → +1
            return (fx::q15)((int32_t)p - 32768);
        case 3:     // SQR
            return (p < 32768) ? (fx::q15)32767 : (fx::q15)(-32767);
    }
}

// midi note → freq Hz
static double midi_to_hz(int note) {
    return 440.0 * std::pow(2.0, (note - 69) / 12.0);
}

// === FmSynth ===
void FmSynth::note_on(int note, int velocity) {
    build_sin_lut();
    double base_hz = midi_to_hz(note);

    for (int i = 0; i < FM_NUM_OPS; ++i) {
        auto& op = ops_[i];
        auto& p  = params.ops[i];
        // phase increment q16.16: freq * (1<<16) / SR
        double freq = base_hz * RATIOS[p.ratio_idx & 15];
        op.phase_inc = (uint32_t)(freq * 65536.0 / SR);
        op.phase = 0;
        op.last_out = 0;
        // ADSR start
        op.stage = (p.attack > 0) ? OpState::Stage::Attack : OpState::Stage::Decay;
        op.env = (p.attack > 0) ? 0 : ENV_ONE;
        op.stage_pos = 0;
        op.release_start = 0;
    }
    velocity_ = (fx::q15)((velocity * fx::Q15_ONE) / 127);
    gated_ = true;
    active_ = true;
}

void FmSynth::note_off() {
    if (!gated_) return;
    gated_ = false;
    for (int i = 0; i < FM_NUM_OPS; ++i) {
        ops_[i].release_start = ops_[i].env;
        ops_[i].stage = OpState::Stage::Release;
        ops_[i].stage_pos = 0;
    }
}

// advance one frame of the operator's ADSR, return env q30
static fx::q31 advance_env(FmSynth::OpState& op, const FmOpParams& p) {
    using Stage = FmSynth::OpState::Stage;
    switch (op.stage) {
        case Stage::Attack: {
            if (p.attack > 0) op.env += ENV_ONE / (fx::q31)p.attack;
            if (op.env >= ENV_ONE || op.stage_pos >= p.attack) {
                op.env = ENV_ONE;
                op.stage = Stage::Decay;
                op.stage_pos = 0;
            }
            break;
        }
        case Stage::Decay: {
            if (p.decay == 0) {
                op.stage = Stage::Sustain;
                op.env = (fx::q31)p.sustain << 23;   // sustain 0-127 → q30 (127→~ENV_ONE)
            } else {
                fx::q31 target = (fx::q31)p.sustain << 23;
                if (target > ENV_ONE) target = ENV_ONE;
                fx::q31 step = (ENV_ONE - target) / (fx::q31)p.decay;
                op.env -= step;
                if (op.env <= target || op.stage_pos >= p.decay) {
                    op.env = target;
                    op.stage = Stage::Sustain;
                }
            }
            // sustain=0: decay-only envelope - the op is DONE once it hits zero.
            // without this the op parks in Sustain with env=0 forever: the voice
            // never reports idle, the poly pool stays full of silent zombies and
            // every new note forces a voice steal (audible as constant grit).
            if (op.stage == Stage::Sustain && op.env <= 0) {
                op.env = 0;
                op.stage = Stage::Idle;
            }
            break;
        }
        case Stage::Sustain: {
            fx::q31 target = (fx::q31)p.sustain << 23;
            if (target > ENV_ONE) target = ENV_ONE;
            if (target <= 0) { op.env = 0; op.stage = Stage::Idle; break; }  // same zombie guard
            op.env = target;
            break;
        }
        case Stage::Release: {
            if (p.release == 0) {
                op.env = 0;
                op.stage = Stage::Idle;
            } else {
                fx::q31 step = op.release_start / (fx::q31)p.release;
                op.env -= step;
                if (op.env <= 0 || op.stage_pos >= p.release) {
                    op.env = 0;
                    op.stage = Stage::Idle;
                }
            }
            break;
        }
        case Stage::Idle:
            return 0;
    }
    ++op.stage_pos;
    return op.env;
}

bool FmSynth::render(fx::q15* out, std::size_t frames) {
    if (!active_) {
        for (std::size_t i = 0; i < frames * 2; ++i) out[i] = 0;
        return false;
    }

    int algo = params.algorithm & 7;
    const Algo& A = ALGOS[algo];
    int fb_amt = params.feedback & 7;     // 0-7

    // master volume q15
    fx::q15 mvol = (fx::q15)((params.master_volume * fx::Q15_ONE) / 127);

    bool any_alive = false;

    for (std::size_t f = 0; f < frames; ++f) {
        // advance all ADSRs
        for (int i = 0; i < FM_NUM_OPS; ++i) {
            advance_env(ops_[i], params.ops[i]);
            if (ops_[i].stage != OpState::Stage::Idle) any_alive = true;
        }

        // compute operator by operator (1->2->3->4)
        // op_out[i] - output of operator i (q15)
        int32_t op_out[FM_NUM_OPS] = {0,0,0,0};

        for (int i = 0; i < FM_NUM_OPS; ++i) {
            auto& op = ops_[i];
            auto& p  = params.ops[i];

            // compute the phase modulation from the modulators
            // mod_mask[i] says who modulates op[i]
            int32_t mod_phase = 0;
            uint8_t mm = A.mod_mask[i];
            for (int m = 0; m < FM_NUM_OPS; ++m) {
                if (mm & (1 << m)) {
                    mod_phase += op_out[m];
                }
            }
            // feedback on op1 (if op == 0 and there is feedback)
            if (i == 0 && fb_amt > 0) {
                // op1's last output modulates itself, scaled feedback
                int32_t fb_mod = ((int32_t)op.last_out * fb_amt) >> 5;     // 0-7 / 32
                mod_phase += fb_mod;
            }

            // phase modulation: add mod to the oscillator's phase.
            // mod_phase in q15 (roughly +/-32768 max). oscillator phase in q16 (65536 = 1 cycle).
            // it used to be `mod_phase << 1` - that gave phase deviation up to ~1.0 cycle,
            // on STACK algos with 3 cascades that produced chaos/crackle on the bell preset.
            // now: 1 q15 unit = 1 q16 unit -> max deviation ~0.5 cycle. closer to the classic
            // DX/m8 modulation index, cleaner sound on cascaded algorithms.
            uint32_t modulated_phase = op.phase + (uint32_t)mod_phase;

            // waveform lookup (SIN via LUT; TRI/SAW/SQR computed from phase)
            fx::q15 raw = op_wave(p.wave, modulated_phase);

            // amplitude = env * level (level 0-127 → q15 scale)
            fx::q31 env_q30 = op.env;                              // q30
            int32_t env_q15 = (int32_t)(env_q30 >> 15);            // q15
            int32_t lvl = ((int32_t)p.level * fx::Q15_ONE) / 127;  // q15
            int32_t amp = (env_q15 * lvl) >> 15;                   // q15

            int32_t out_v = ((int32_t)raw * amp) >> 15;            // q15
            // remember for feedback (only op1)
            op.last_out = (fx::q15)out_v;
            op_out[i] = out_v;

            // advance the phase
            op.phase += op.phase_inc;
        }

        // sum the carriers
        int32_t mix = 0;
        int n_carriers = 0;
        for (int i = 0; i < FM_NUM_OPS; ++i) {
            if (A.carrier_mask & (1 << i)) {
                mix += op_out[i];
                ++n_carriers;
            }
        }
        if (n_carriers == 0) n_carriers = 1;
        mix /= n_carriers;     // headroom - divide by the number of carriers

        // master + velocity
        int32_t v = mix;
        v = (v * (int32_t)mvol) >> 15;
        v = (v * (int32_t)velocity_) >> 15;
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;

        out[f*2 + 0] = (fx::q15)v;
        out[f*2 + 1] = (fx::q15)v;
    }

    if (!any_alive) {
        active_ = false;
        return false;
    }
    return true;
}

} // namespace trackr::synth
