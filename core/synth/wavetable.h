// user wavetable bank: single-cycle waveforms loaded from SD (AKWF-style).
// each slot = 1024-point q15 table; wavsynth's USER shape reads from here.
//
// files: sdmc:/3ds/descry/wavetable/*.wav - each file is treated as ONE CYCLE
// (the whole file is resampled to 1024 points). standard single-cycle packs
// (AKWF etc, ~600 samples) work as-is. loaded once at boot; slot order is
// ALPHABETICAL so project references stay stable while the folder is unchanged.
#pragma once
#include "../audio/fixed.h"

namespace trackr::synth {

class WavetableBank {
public:
    static constexpr int SLOTS = 16;
    static constexpr int SIZE  = 1024;   // points per table (power of two)

    static WavetableBank& instance();

    // scan a directory for .wav files (alphabetical, up to SLOTS), resample
    // each whole file into a 1024-point cycle. returns number loaded.
    int scan_dir(const char* dir);

    int count() const { return count_; }
    const char* name(int slot) const {
        return (slot >= 0 && slot < count_) ? names_[slot] : "--";
    }

    // sample slot at 16-bit phase (0..65535 = one cycle), linear interp.
    // empty/invalid slot returns 0 (silence, never garbage).
    fx::q15 sample(int slot, fx::uq16 phase) const {
        if (slot < 0 || slot >= count_) return 0;
        const fx::q15* t = data_[slot];
        uint32_t idx_full = (uint32_t)phase << 4;      // 16-bit -> 20-bit
        int idx  = (idx_full >> 10) & (SIZE - 1);
        int frac = idx_full & 0x3FF;                   // 0..1023
        fx::q15 a = t[idx];
        fx::q15 b = t[(idx + 1) & (SIZE - 1)];
        return (fx::q15)(a + (((int32_t)(b - a) * frac) >> 10));
    }

private:
    int     count_ = 0;
    fx::q15 data_[SLOTS][SIZE] = {};
    char    names_[SLOTS][20] = {};
};

} // namespace trackr::synth
