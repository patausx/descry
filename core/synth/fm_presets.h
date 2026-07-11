// ready-made FM presets - 12 classic timbres for FmSynthParams
// loaded with a single command into any FmSynth-type instrument
#pragma once
#include "fm.h"

namespace trackr::synth {

enum class FmPreset : uint8_t {
    Init = 0,    // blank patch: single sine carrier (DX7 INIT VOICE style)
    EP,          // electric piano (Yamaha-style)
    Bass,        // sub-bass with feedback
    Lead,        // sharp synth lead
    Bell,        // tubular bell
    Brass,       // synth brass
    Pluck,       // plucked string
    Organ,       // additive organ
    Pad,         // slow attack pad
    Stab,        // chord stab
    Wood,        // marimba/wood
    Wah,         // talky wah-tone
    Ice,         // glass/icy texture
    NUM_PRESETS
};

constexpr int FM_PRESET_COUNT = (int)FmPreset::NUM_PRESETS;

const char* fm_preset_name(FmPreset p);
void fm_load_preset(FmSynthParams& dst, FmPreset p);

} // namespace trackr::synth
