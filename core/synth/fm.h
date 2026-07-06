// 4-op FM synth (DX7-style, simplified for the tracker)
// 8 fixed algorithms, feedback on op1, full ADSR per operator
//
// Algorithms:
//  0: Stack       1→2→3→4              [carrier: 4]
//  1: Pair        1→2, 3→4             [carriers: 2,4]
//  2: Y-fan       1→2, 1→3, 1→4        [carriers: 2,3,4]
//  3: 3-into-1    1→4, 2→4, 3→4        [carrier: 4]
//  4: Tree        1→2, 1→3, 2→4        [carriers: 3,4]
//  5: Stack+aux   1→2→3, 4 solo        [carriers: 3,4]
//  6: Additive    1+2+3+4              [all carriers]
//  7: Branch      1→3, 2→3, 4          [carriers: 3,4]
#pragma once
#include "../audio/voice.h"
#include <cstdint>

namespace trackr::synth {

// per-operator params
struct FmOpParams {
    uint8_t  ratio_idx = 1;     // index into ratio table (1.0 default)
    uint8_t  level     = 100;   // 0-127 modulation depth / carrier amplitude
    uint16_t attack    = 50;    // frames
    uint16_t decay     = 6000;
    uint8_t  sustain   = 100;   // 0-127
    // op waveform (m8-style shapes): 0=SIN 1=TRI 2=SAW 3=SQR.
    // NOTE: deliberately declared here - fills the struct's old padding hole
    // between sustain and release, so sizeof/layout stay identical.
    uint8_t  wave      = 0;
    uint16_t release   = 4000;
};

constexpr int FM_NUM_OPS = 4;
constexpr int FM_NUM_ALGOS = 8;

struct FmSynthParams {
    uint8_t algorithm = 1;       // 0-7
    uint8_t feedback  = 0;       // 0-7 self-mod on op1
    uint8_t master_volume = 100; // 0-127 overall volume
    FmOpParams ops[FM_NUM_OPS];
};

// get ratio (q16.16) by index 0..15
int32_t fm_ratio_q16(int idx);
const char* fm_ratio_name(int idx);
const char* fm_algo_name(int idx);
const char* fm_op_wave_name(int idx);       // "SIN"/"TRI"/"SAW"/"SQR"

// algorithm topology (for the UI diagram):
// carrier_mask: bit i = op_i goes to the output
// mod_mask(op): bit m = op_m modulates op
uint8_t fm_algo_carrier_mask(int algo);
uint8_t fm_algo_mod_mask(int algo, int op);

// === voice ===
class FmSynth : public audio::Voice {
public:
    void note_on(int note, int velocity) override;
    void note_off() override;
    bool render(fx::q15* out, std::size_t frames) override;

    // steal priority: held notes are sacred (INT32_MAX); released ones report
    // their loudest op envelope -> the pool steals the quietest ringing tail.
    fx::q31 steal_weight() const override {
        if (gated_) return 0x7FFFFFFF;
        fx::q31 mx = 0;
        for (int i = 0; i < FM_NUM_OPS; ++i)
            if (ops_[i].env > mx) mx = ops_[i].env;
        return mx;
    }

    FmSynthParams params;

    // per-op state - public so advance_env (free helper) can access it
    struct OpState {
        uint32_t phase   = 0;       // q16.16 phase
        uint32_t phase_inc = 0;     // q16.16 increment per frame
        // ADSR
        enum class Stage : uint8_t { Idle, Attack, Decay, Sustain, Release };
        Stage   stage = Stage::Idle;
        fx::q31 env   = 0;          // q30
        fx::q31 release_start = 0;
        uint32_t stage_pos = 0;
        // last output (q15) - for feedback
        fx::q15 last_out = 0;
    };

private:
    OpState ops_[FM_NUM_OPS];
    fx::q15 velocity_ = fx::Q15_ONE;
    bool gated_ = false;
};

} // namespace trackr::synth
