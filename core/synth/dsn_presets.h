// ready-made DSN presets - 12 classic analog timbres for DsnSynthParams
// loaded with a single command into any DsnSynth-type instrument (like fm_presets)
#pragma once
#include "dsn_synth.h"

namespace trackr::synth {

enum class DsnPreset : uint8_t {
    Init = 0,    // blank patch: single saw, filter open, no modulation
    Acid,        // 303-style resonant acid bass
    Hoover,      // rave hoover (pwm + detuned saw, pitch drop-in)
    Sync,        // hard-sync lead with eg2 sweep
    Sub,         // clean sine sub bass
    Pwm,         // lush PWM pad/keys
    Strings,     // detuned saw strings, slow attack
    Lead,        // pulse+saw solo lead with vibrato
    Pluck,       // filter-pluck (eg1->cutoff, no sustain)
    Wobble,      // LFO->cutoff dub wobble bass
    Trem,        // tremolo pad (mg2->vca)
    Zap,         // pitch-drop zap/perc (eg2->pitch)
    Wind,        // filtered noise wash
    Kick,        // analog kick (sine + eg2 pitch drop)
    Snare,       // tri + noise through BPF, snappy
    Hat,         // noise through HPF, tight decay
    Tom,         // sine tom, medium pitch drop
    NUM_PRESETS
};

constexpr int DSN_PRESET_COUNT = (int)DsnPreset::NUM_PRESETS;

const char* dsn_preset_name(DsnPreset p);
void dsn_load_preset(DsnSynthParams& dst, DsnPreset p);

} // namespace trackr::synth
