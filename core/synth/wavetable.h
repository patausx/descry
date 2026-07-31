// user wavetable bank: single-cycle waveforms loaded from SD (AKWF-style).
// each slot = 1024-point q15 table; wavsynth's USER shape reads from here.
//
// files: sdmc:/3ds/descry/wavetable/*.wav - each file is treated as ONE CYCLE
// (the whole file is resampled to 1024 points). standard single-cycle packs
// (AKWF etc, ~600 samples) work as-is. loaded once at boot; slot order is
// ALPHABETICAL so project references stay stable while the folder is unchanged.
#pragma once
#include "../audio/fixed.h"
#include <cstdint>

namespace trackr::synth {

struct Sample;

class WavetableBank {
public:
    static constexpr int FILE_SLOTS    = 16; // alphabetical SD .wav files; legacy IDs stay unchanged
    static constexpr int CAPTURE_SLOTS = 16; // fixed physical slots 16..31
    static constexpr int SLOTS         = FILE_SLOTS + CAPTURE_SLOTS;
    static constexpr int SIZE          = 1024;   // points per table (power of two)

    static WavetableBank& instance();

    // scan a directory for .wav files (alphabetical, up to SLOTS), resample
    // each whole file into a 1024-point cycle. returns number loaded.
    int scan_dir(const char* dir);

    int count() const { return active_count_; }
    // UI index <-> physical user_slot mapping. Captures may live at 16..31 while
    // the active list remains dense, so callers must not assume index == slot.
    int slot_at(int active_index) const;
    int index_of_slot(int slot) const;
    bool occupied(int slot) const { return slot >= 0 && slot < SLOTS && occupied_[slot]; }
    const char* name(int slot) const {
        return occupied(slot) ? names_[slot] : "--";
    }

    // Build one seamless 1024-point cycle from a sample window. Pure/offline:
    // zero DC, normalize, no bank mutation and no audio lock required.
    static bool prepare_capture(const Sample& src, uint32_t start_frame,
                                uint32_t end_frame, fx::q15* out);

    // Install into the first free fixed capture slot (16..31). The caller holds
    // the mixer lock for this short 2KB copy. Returns physical slot or -1/full.
    int install_capture(const fx::q15* cycle, const char* name);
    bool remove_capture(int slot);
    // Persist captured slots to <dir>/descry_captures.wtb atomically.
    bool save_captures(const char* dir) const;

    // sample slot at 16-bit phase (0..65535 = one cycle), linear interp.
    // empty/invalid slot returns 0 (silence, never garbage).
    fx::q15 sample(int slot, fx::uq16 phase) const {
        if (!occupied(slot)) return 0;
        const fx::q15* t = data_[slot];
        uint32_t idx_full = (uint32_t)phase << 4;      // 16-bit -> 20-bit
        int idx  = (idx_full >> 10) & (SIZE - 1);
        int frac = idx_full & 0x3FF;                   // 0..1023
        fx::q15 a = t[idx];
        fx::q15 b = t[(idx + 1) & (SIZE - 1)];
        return (fx::q15)(a + (((int32_t)(b - a) * frac) >> 10));
    }

private:
    int     active_count_ = 0;
    bool    occupied_[SLOTS] = {};
    fx::q15 data_[SLOTS][SIZE] = {};
    char    names_[SLOTS][20] = {};

    void rebuild_active_count();
    void load_captures(const char* dir);
};

} // namespace trackr::synth
