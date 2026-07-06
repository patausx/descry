#include "fixed.h"
#include <cmath>

namespace trackr::fx {

// precomputed table: midi 0-127 -> freq*65536, in hz*65536
// generated once at startup, then just indexed
static q16 phase_inc_table[128];
static int cached_sample_rate = 0;

static void rebuild_table(int sr) {
    for (int n = 0; n < 128; ++n) {
        double freq = 440.0 * std::pow(2.0, (n - 69) / 12.0);
        double inc  = freq * (1ULL << 16) / sr;
        phase_inc_table[n] = static_cast<q16>(inc);
    }
    cached_sample_rate = sr;
}

q16 note_to_phase_inc(int note, int sample_rate) {
    if (sample_rate != cached_sample_rate) rebuild_table(sample_rate);
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    return phase_inc_table[note];
}

} // namespace trackr::fx
