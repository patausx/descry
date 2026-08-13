// Deterministic, allocation-free phrase generators used by the Phrase Tools UI.
//
// Generators only touch rows in [lo, hi]. Random operations use the explicit seed,
// so the same phrase + settings always produces the same result and can be tested
// on the host exactly as it behaves on the 3DS.
#pragma once
#include "types.h"
#include <cstdint>

namespace trackr::seq {

enum class PhraseGenOp : uint8_t {
    Euclidean = 0,   // amount = pulse count across the selected rows
    Density,        // amount = percent of existing notes retained
    Humanize,       // amount = velocity range; also adds a small positive DLY
    Ratchet,        // amount = percent of notes receiving RTG 01..03
    Mutate,         // amount = percent of notes moved by a few scale degrees
    RandomNotes,    // amount = semitone span around default_note
    ChanceSpread,   // amount = minimum trigger chance percent (up to 100%)
    Every,          // amount = cycle length; distributes passes 1..N
};

struct PhraseGenOptions {
    uint32_t seed = 1;
    int amount = 0;
    uint8_t scale_type = 0;
    uint8_t scale_root = 0;
    uint8_t default_note = 60;
    uint8_t instrument = 0;
};

// Returns true when at least one PhraseStep changed.
bool apply_phrase_generator(Phrase& phrase, int lo, int hi, PhraseGenOp op,
                            const PhraseGenOptions& options);

} // namespace trackr::seq
