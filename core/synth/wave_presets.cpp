#include "wave_presets.h"
#include "../audio/fixed.h"

namespace trackr::synth {

// timings in frames @ 32000 Hz:
//   3200 = 0.1s   16000 = 0.5s   32000 = 1.0s
// size = q15 PWM/shape modifier (Q15_ONE = max)
// sustain = q15 level (Q15_ONE = 1.0, /2 = 0.5)

using fx::Q15_ONE;

static WavsynthParams mk(WaveShape sh, fx::q15 size,
                         uint32_t a, uint32_t d, fx::q15 s, uint32_t r) {
    WavsynthParams p;
    p.shape   = sh;
    p.size    = size;
    p.attack  = a;
    p.decay   = d;
    p.sustain = s;
    p.release = r;
    return p;
}

void wave_load_preset(WavsynthParams& dst, WavePreset p) {
    switch (p) {
        // INIT: a true blank patch - struct defaults, nothing dialed in.
        // for people who build every patch from scratch (discord request).
        case WavePreset::Init:
            dst = WavsynthParams{}; break;
        // saw lead: instant attack, holds at 70%, medium release
        case WavePreset::SawLead:
            dst = mk(WaveShape::Saw, Q15_ONE, 80, 6000, Q15_ONE*70/100, 6000); break;
        // sub-bass: pure sine, fast attack, holds near the ceiling, short tail
        case WavePreset::SubBass:
            dst = mk(WaveShape::Sine, Q15_ONE, 60, 8000, Q15_ONE*90/100, 2500); break;
        // pluck: triangle, instant, decay to zero, no sustain
        case WavePreset::Pluck:
            dst = mk(WaveShape::Triangle, Q15_ONE, 40, 4500, 0, 1200); break;
        // pad: saw, slow fade-in, holds, very long release
        case WavePreset::Pad:
            dst = mk(WaveShape::Saw, Q15_ONE, 9000, 12000, Q15_ONE*75/100, 20000); break;
        // organ: square, holds at the ceiling, no decay dip
        case WavePreset::Organ:
            dst = mk(WaveShape::Square, Q15_ONE, 200, 3000, Q15_ONE*95/100, 1500); break;
        // stab: square, sharp, decay to zero, short
        case WavePreset::Stab:
            dst = mk(WaveShape::Square, Q15_ONE, 40, 3500, Q15_ONE*20/100, 1000); break;
        // bass: saw, fast attack, springy decay, medium sustain
        case WavePreset::Bass:
            dst = mk(WaveShape::Saw, Q15_ONE, 60, 5000, Q15_ONE*60/100, 1800); break;
        // bell: sine, instant, long exponential decay, no sustain
        case WavePreset::Bell:
            dst = mk(WaveShape::Sine, Q15_ONE, 40, 14000, 0, 6000); break;
        // chip: square 50% PWM, classic chiptune lead
        case WavePreset::Chip:
            dst = mk(WaveShape::Square, Q15_ONE/2, 40, 4000, Q15_ONE*80/100, 2000); break;
        // sweep: saw, long decay - for a filter mod table
        case WavePreset::Sweep:
            dst = mk(WaveShape::Saw, Q15_ONE, 200, 20000, Q15_ONE*50/100, 10000); break;
        // noise: percussive noise, instant, decay to zero (hat/snare)
        case WavePreset::Noise:
            dst = mk(WaveShape::Noise, Q15_ONE, 20, 2500, 0, 600); break;
        // drone: triangle, slow fade-in, holds forever
        case WavePreset::Drone:
            dst = mk(WaveShape::Triangle, Q15_ONE, 16000, 8000, Q15_ONE, 16000); break;
        // === ambient presets: detune + long envelopes (Malibu vibe) ===
        // warm pad: saw, attack 1.2s, release 6s, 3 oscs medium detune
        case WavePreset::WarmPad:
            dst = mk(WaveShape::Saw, Q15_ONE, 38000, 20000, Q15_ONE*80/100, 192000);
            dst.unison = 3; dst.detune_cents = 14; dst.spread = Q15_ONE*65/100; break;
        // bright glassy pad: sine, wide detune, very long tail
        case WavePreset::GlassPad:
            dst = mk(WaveShape::Sine, Q15_ONE, 48000, 16000, Q15_ONE*85/100, 224000);
            dst.unison = 3; dst.detune_cents = 22; dst.spread = Q15_ONE*80/100; break;
        // low evolving drone: saw, max detune, holds forever
        case WavePreset::DeepDrone:
            dst = mk(WaveShape::Saw, Q15_ONE, 64000, 24000, Q15_ONE, 256000);
            dst.unison = 3; dst.detune_cents = 30; dst.spread = Q15_ONE*70/100; break;
        // triangle shimmer: high sparkle, 2 oscs subtle detune
        case WavePreset::Shimmer:
            dst = mk(WaveShape::Triangle, Q15_ONE, 28000, 18000, Q15_ONE*75/100, 160000);
            dst.unison = 2; dst.detune_cents = 9; dst.spread = Q15_ONE*85/100; break;
        default: break;
    }
}

const char* wave_preset_name(WavePreset p) {
    switch (p) {
        case WavePreset::Init:    return "INIT";
        case WavePreset::SawLead: return "SAW LEAD";
        case WavePreset::SubBass: return "SUB BASS";
        case WavePreset::Pluck:   return "PLUCK";
        case WavePreset::Pad:     return "PAD";
        case WavePreset::Organ:   return "ORGAN";
        case WavePreset::Stab:    return "STAB";
        case WavePreset::Bass:    return "BASS";
        case WavePreset::Bell:    return "BELL";
        case WavePreset::Chip:    return "CHIP";
        case WavePreset::Sweep:   return "SWEEP";
        case WavePreset::Noise:   return "NOISE";
        case WavePreset::Drone:   return "DRONE";
        case WavePreset::WarmPad:  return "WARM PAD";
        case WavePreset::GlassPad: return "GLASS PAD";
        case WavePreset::DeepDrone:return "DEEP DRONE";
        case WavePreset::Shimmer:  return "SHIMMER";
        default: return "?";
    }
}

} // namespace trackr::synth
