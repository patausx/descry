// musical scales / key snap.
// a scale = 12-bit pitch-class mask relative to the root (bit 0 = root).
// type 0 = chromatic = "off" - everything passes, editor stays semitone-based.
#pragma once
#include <cstdint>

namespace trackr::seq {

constexpr int SCALE_COUNT = 12;

// bit i set = semitone i (from root) is in the scale
constexpr uint16_t SCALE_MASKS[SCALE_COUNT] = {
    0xFFF,  // 0 OFF (chromatic)
    0xAB5,  // 1 MAJ  major            {0,2,4,5,7,9,11}
    0x5AD,  // 2 MIN  natural minor    {0,2,3,5,7,8,10}
    0x9AD,  // 3 HRM  harmonic minor   {0,2,3,5,7,8,11}
    0xAAD,  // 4 MEL  melodic minor    {0,2,3,5,7,9,11}
    0x6AD,  // 5 DOR  dorian           {0,2,3,5,7,9,10}
    0x5AB,  // 6 PHR  phrygian         {0,1,3,5,7,8,10}
    0xAD5,  // 7 LYD  lydian           {0,2,4,6,7,9,11}
    0x6B5,  // 8 MIX  mixolydian       {0,2,4,5,7,9,10}
    0x295,  // 9 PMA  pentatonic major {0,2,4,7,9}
    0x4A9,  // A PMI  pentatonic minor {0,3,5,7,10}
    0x4E9,  // B BLU  blues            {0,3,5,6,7,10}
};

inline const char* scale_name(uint8_t type) {
    static const char* n[SCALE_COUNT] = {
        "OFF", "MAJ", "MIN", "HRM", "MEL", "DOR",
        "PHR", "LYD", "MIX", "PMA", "PMI", "BLU"
    };
    return (type < SCALE_COUNT) ? n[type] : "OFF";
}

inline const char* root_name(uint8_t root) {
    static const char* n[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    return n[root % 12];
}

// is this midi note in the scale? type 0 / out-of-range = always yes.
inline bool scale_has(uint8_t type, uint8_t root, int note) {
    if (type == 0 || type >= SCALE_COUNT) return true;
    if (note < 0 || note > 127) return false;
    int pc = ((note % 12) - (root % 12) + 12) % 12;
    return (SCALE_MASKS[type] >> pc) & 1;
}

// snap to the nearest in-scale note (downward wins ties) - for live key input
inline int scale_snap(uint8_t type, uint8_t root, int note) {
    if (scale_has(type, root, note)) return note;
    for (int d = 1; d < 12; ++d) {
        if (note - d >= 0   && scale_has(type, root, note - d)) return note - d;
        if (note + d <= 127 && scale_has(type, root, note + d)) return note + d;
    }
    return note;
}

// next in-scale note in direction dir (+1/-1) - for cursor note editing.
// returns `note` unchanged if there is nothing further in that direction.
inline int scale_step(uint8_t type, uint8_t root, int note, int dir) {
    int n = note + dir;
    while (n >= 0 && n <= 127) {
        if (scale_has(type, root, n)) return n;
        n += dir;
    }
    return note;
}

} // namespace trackr::seq
