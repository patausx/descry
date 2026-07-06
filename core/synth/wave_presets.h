// ready-made wavsynth presets - a starter bank of timbres for WavsynthParams
// loaded with a single command into any Wavsynth-type instrument (modeled on fm_presets)
#pragma once
#include "wavsynth.h"

namespace trackr::synth {

enum class WavePreset : uint8_t {
    SawLead = 0,  // classic saw lead
    SubBass,      // deep sine sub
    Pluck,        // short triangle pluck
    Pad,          // slow saw pad with a long release
    Organ,        // square organ, sustain to the ceiling
    Stab,         // sharp square stab
    Bass,         // springy saw bass
    Bell,         // sine bell, fast decay
    Chip,         // square 50% chiptune lead
    Sweep,        // saw with a long decay (for filter table)
    Noise,        // percussive noise (hat/snare-ready)
    Drone,        // infinite triangle drone
    // === ambient (Malibu / SAW vibe) - detune + long envelopes ===
    WarmPad,      // warm detune pad, slow fade-in/out
    GlassPad,     // bright sine pad, wide detune
    DeepDrone,    // low evolving drone, max detune
    Shimmer,      // triangle shimmer pad, high sparkle
    NUM_PRESETS
};

constexpr int WAVE_PRESET_COUNT = (int)WavePreset::NUM_PRESETS;

const char* wave_preset_name(WavePreset p);
void wave_load_preset(WavsynthParams& dst, WavePreset p);

} // namespace trackr::synth
