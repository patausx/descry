// dsp/lfo.h - Modulation Generator (LFO) with 4 simultaneous shapes
//
// Architecture like the Korg DSN-12 (dug out of the ROM, see research/DSN12_REVERSE.md):
// their MG is not "one LFO with a switch", but *four parallel shapes
// from one phase*: TRI, SAW, SQR, S&H. In the patch matrix each shape is a separate
// mod source.
//
// We do exactly the same. This lets us modulate several targets at once
// with different shapes from one MG.
//
// Fixed-point:
//   phase: uint32 (0..0xFFFFFFFF = one cycle)
//   phase_inc: uint32 = freq_hz * (2^32 / sample_rate)
//   output: q15 (+/-32767)
//
// S&H: a new random sample is taken when phase wraps (once per cycle).
// We can tick more often if needed (via ratio), but in the DSN-12 it's once per cycle.

#pragma once
#include "../audio/fixed.h"

namespace trackr::dsp {

// One MG output per step - all 4 shapes at once
struct MgOut {
    fx::q15 tri;   // triangle
    fx::q15 saw;   // saw (ramp up)
    fx::q15 sqr;   // square
    fx::q15 sh;    // sample & hold (random, updated on wrap)
};

struct Mg {
    uint32_t phase     = 0;
    uint32_t phase_inc = 0;            // = rate_hz * (2^32 / sample_rate)
    int32_t  sh_value  = 0;            // current sample&hold (q15)
    uint32_t noise_state = 0xCAFEBABE; // LCG state for S&H random
    uint32_t prev_phase = 0;           // for wrap detection

    // frequency setup in Hz at sample_rate (usually 32000)
    // rate_hz - q16.16 (i.e. 1.0 Hz = 65536, 10 Hz = 655360)
    inline void set_rate_hz_q16(fx::q16 rate_hz_q16, int sample_rate) {
        // phase_inc = rate_hz * 2^32 / sample_rate
        //           = (rate_hz_q16 / 2^16) * 2^32 / sample_rate
        //           = rate_hz_q16 * 2^16 / sample_rate
        uint64_t inc = ((uint64_t)(uint32_t)rate_hz_q16 << 16) / (uint32_t)sample_rate;
        phase_inc = (uint32_t)inc;
    }

    // Convenient mapping: q15 [0..32767] -> 0.1Hz..20Hz (typical LFO range)
    // Linear for simplicity. Can do exponential later.
    inline void set_rate_norm(fx::q15 norm, int sample_rate) {
        // 0..32767 → 0.1Hz..20Hz
        // freq = 0.1 + (norm/32767) * 19.9 Hz
        // in q16: freq_q16 = 6554 + norm * 19.9 * 65536 / 32767
        //                 ~= 6554 + norm * 39.8
        // More precisely: 19.9 * 65536 / 32767 = 39.795... ~= 40
        int32_t freq_q16 = 6554 + (int32_t)norm * 40;
        if (freq_q16 < 6554) freq_q16 = 6554;       // floor 0.1 Hz
        set_rate_hz_q16(freq_q16, sample_rate);
    }

    inline void reset_phase() {
        phase = 0;
        prev_phase = 0;
    }

    // one step - updates phase, returns 4 shapes
    inline MgOut step() {
        prev_phase = phase;
        phase += phase_inc;
        bool wrapped = phase < prev_phase;  // unsigned wrap detect

        // S&H - new random on wrap
        if (wrapped || phase_inc == 0) {
            // LCG: Numerical Recipes
            noise_state = noise_state * 1664525u + 1013904223u;
            // take the top 16 bits and center in q15 +/-32767
            sh_value = (int32_t)((noise_state >> 16) & 0x7FFF);
            if (noise_state & 0x80000000u) sh_value = -sh_value;
        }

        MgOut out;

        // top 16 bits of the phase as an index [0..65535]
        uint32_t p = phase >> 16;        // 16-bit phase, 0..65535
        uint32_t half = 0x8000;          // 32768

        // SAW: linear ramp -1 -> +1 over the cycle
        // p=0     → -32768
        // p=32768 → 0
        // p=65535 → +32767
        out.saw = (fx::q15)((int32_t)p - 32768);

        // TRI: -1 → +1 → -1
        // p < half: -1 + 2*(p/half)
        // p >= half: +1 - 2*((p-half)/half)
        // q15:
        if (p < half) {
            // 0 → -32768, half → +32767
            // = -32768 + p*2*32768/half  but half=32768 means = -32768 + p*2
            // wait p<32768 → max p*2 = 65534 → -32768+65534 = 32766 ✓
            out.tri = (fx::q15)((int32_t)p * 2 - 32768);
        } else {
            // p=32768 → +32767, p=65535 → -32766
            // = 32767 - (p-32768)*2 + (1)
            int32_t pp = (int32_t)p - 32768;
            out.tri = (fx::q15)(32767 - pp * 2);
        }

        // SQR: -1 for the first half, +1 for the second
        out.sqr = (p < half) ? (fx::q15)(-32767) : (fx::q15)(32767);

        // S&H - current sh_value value
        out.sh = (fx::q15)sh_value;

        return out;
    }
};

// Shape names for the UI
inline const char* mg_wave_name(int idx) {
    switch (idx) {
        case 0: return "TRI";
        case 1: return "SAW";
        case 2: return "SQR";
        case 3: return "S&H";
    }
    return "?";
}

} // namespace trackr::dsp
