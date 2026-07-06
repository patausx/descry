// procedural drum/instrument bank generator
// generates basic drums and timbres directly into a SampleBank slot
// no WAV files or SD - everything is computed on the fly
#pragma once
#include "sampler.h"

namespace trackr::synth {

enum class DrumType : uint8_t {
    Kick = 0,
    Snare,
    ClosedHat,
    OpenHat,
    Clap,
    Tom,
    Rim,
    Cowbell,
    Bass808,
    Lead,
    Pluck,
    Pad,
    NUM_TYPES
};

constexpr int DRUM_TYPE_COUNT = (int)DrumType::NUM_TYPES;

const char* drum_type_name(DrumType t);

// fills an existing Sample slot with procedural sound
// overwrites data, channels=1, sets an appropriate root_note
// clears chops/loops
void generate_drum(Sample& dst, DrumType type);

} // namespace trackr::synth
