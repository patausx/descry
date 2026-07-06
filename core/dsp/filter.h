// dsp/filter.h — Chamberlin 2-pole state-variable filter (header-only)
//
// One step produces THREE outputs at once: LP, BP, HP. Type is selected at the output.
// Same topology as the inline filter that was in mixer.cpp (Chamberlin SVF),
// but moved into a separate module + extended with HP/BP/Notch outputs + easier
// to inline and test.
//
// Fixed-point: f, damp, state - all int32 in q15-equivalent (max +/-32767).
// At high resonance + high cutoff the topology can wind up - we clamp
// the state every step as before.
//
// Usage:
//   dsp::Svf2 filter;
//   filter.set_params(cutoff_q15, resonance_q15);
//   filter.type = dsp::FilterType::LPF;
//   for (frame) {
//     fx::q15 out = filter.step(in);
//   }
//
// This is not a ZDF SVF (that one is better at self-osc, but more expensive). For DSN mode this
// is enough for now. When we get to a 4-pole ladder option - we'll add it as a separate class.

#pragma once
#include "../audio/fixed.h"

namespace trackr::dsp {

enum class FilterType : uint8_t {
    LPF = 0,    // low-pass - the main synth sound
    HPF = 1,    // high-pass - for percussion/leads
    BPF = 2,    // band-pass - vocal/resonant sound
    Notch = 3,  // narrow-cut - for creative effects
    Off = 4,    // bypass (type selected but the filter is off)
};

// One mono channel SVF. For stereo - two instances.
struct Svf2 {
    FilterType type = FilterType::LPF;

    // state - q15-range (we clamp within this range)
    int32_t lp = 0;
    int32_t bp = 0;

    // coefficients
    int32_t f    = (int32_t)fx::Q15_ONE;        // cutoff (0=closed, 32767=open)
    int32_t damp = (int32_t)fx::Q15_ONE / 2;    // = 1 - resonance

    // setup from q15 cutoff [0..32767] and q15 resonance [0..32767]
    // cutoff: 0 = closed, Q15_ONE = max (but we clamp into a safe range)
    // resonance: 0 = no-Q, Q15_ONE = self-oscillation (clamp into a stable range)
    inline void set_params(fx::q15 cutoff, fx::q15 resonance) {
        int32_t cc = (int32_t)cutoff;
        if (cc < 1) cc = 1;
        // ceiling 0.85 - above this the Chamberlin SVF is unstable at high Q
        constexpr int32_t F_MAX = (int32_t)(fx::Q15_ONE * 85 / 100);
        if (cc > F_MAX) cc = F_MAX;
        f = cc;
        int32_t d = (int32_t)fx::Q15_ONE - (int32_t)resonance;
        // self-osc minimum - too low a damp = explosion
        // 1024 = a moderate resonant zone up to self-osc on the edge
        if (d < 1024) d = 1024;
        damp = d;
    }

    // input - q15 sample, output - q15 (the type decides which branch is returned)
    // ALWAYS_INLINE candidate - hot path, on every sample
    inline fx::q15 step(fx::q15 in) {
        if (type == FilterType::Off) return in;

        int32_t s_in = (int32_t)in;
        // SVF: hp = in - lp - damp*bp
        //      bp += f*hp
        //      lp += f*bp
        int32_t hp = s_in - lp - (int32_t)(((int64_t)damp * bp) >> 15);
        bp += (int32_t)(((int64_t)f * hp) >> 15);
        if (bp >  32767) bp =  32767;
        if (bp < -32768) bp = -32768;
        lp += (int32_t)(((int64_t)f * bp) >> 15);
        if (lp >  32767) lp =  32767;
        if (lp < -32768) lp = -32768;

        switch (type) {
            case FilterType::LPF: return (fx::q15)lp;
            case FilterType::HPF: {
                // hp clamp
                if (hp >  32767) hp =  32767;
                if (hp < -32768) hp = -32768;
                return (fx::q15)hp;
            }
            case FilterType::BPF: return (fx::q15)bp;
            case FilterType::Notch: {
                int32_t n = s_in - (int32_t)bp;  // input - BPF = notch
                if (n >  32767) n =  32767;
                if (n < -32768) n = -32768;
                return (fx::q15)n;
            }
            case FilterType::Off:
            default: return in;
        }
    }

    inline void reset() {
        lp = 0;
        bp = 0;
    }
};

// Type names for the UI
inline const char* filter_type_name(FilterType t) {
    switch (t) {
        case FilterType::LPF:   return "LPF";
        case FilterType::HPF:   return "HPF";
        case FilterType::BPF:   return "BPF";
        case FilterType::Notch: return "NTC";
        case FilterType::Off:   return "OFF";
    }
    return "?";
}

} // namespace trackr::dsp
