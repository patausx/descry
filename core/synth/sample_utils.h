// utilities for working with Sample - anti-click, zero crossing search, etc
//
// the zero-crossing snap idea is borrowed from nitrotracker (0xtob), sampledisplay.cpp:
// when setting loop start/end, the sampler searches for the nearest zero crossing within a small
// radius around the given point. this removes the classic clicks at the loop seam,
// because the amplitude passes smoothly through 0 without a jump.
//
// our Sample is q15 mono or stereo (interleaved). for stereo we only look at the left channel:
// this is enough to remove clicks (the right channel is usually correlated), and matches the
// nitrotracker approach.
#pragma once
#include "sampler.h"
#include <cstdint>

namespace trackr::synth {

// finds the nearest zero-crossing to frame_pos within +/- radius frames.
// returns frame_pos if nothing was found (no-op for the caller).
// if found - returns the frame of the nearest sign change.
//
// "frame" = logical index, not a sample. for stereo data[frame*2] = L channel.
//
// constexpr SNAP_RADIUS_DEFAULT = 32 (same as 0xtob - a compromise between accuracy and
// "not drifting too far from what the user selected").
constexpr uint32_t SNAP_RADIUS_DEFAULT = 32;

uint32_t find_zero_crossing_near(const Sample& s,
                                  uint32_t frame_pos,
                                  uint32_t radius = SNAP_RADIUS_DEFAULT);

// === destructive sample edit ops (shared by instrument WAVE panel & sample editor) ===
// all operate on raw q15 data in place; "norm" args are q15 [0..Q15_ONE] of the range.
void sample_trim_norm(Sample& s, fx::q15 start_norm, fx::q15 length_norm);  // crop; resets loops+chops
void sample_normalize(Sample& s);                       // peak -> ~-0.8dBFS
void sample_reverse(Sample& s);
void sample_gain_db(Sample& s, int delta_db);           // +/-1..3 dB, clamps
void sample_fade_in(Sample& s, uint32_t a, uint32_t b);   // raw data index range
void sample_fade_out(Sample& s, uint32_t a, uint32_t b);
void sample_auto_slice(Sample& s, int n_chops);         // spread N chops evenly
int  sample_chop_count(const Sample& s);

} // namespace trackr::synth
