// project: everything in one - song + chains + phrases + instruments + sample bank
#pragma once
#include "types.h"
#include "../synth/wavsynth.h"
#include "../synth/sampler.h"
#include "../synth/drumkit.h"
#include "../synth/fm.h"
#include "../synth/dsn_synth.h"

namespace trackr::seq {

enum class InstrumentType : uint8_t {
    None    = 0,
    Wavsynth = 1,
    Sampler  = 2,
    DrumKit  = 3,
    FmSynth  = 4,
    DsnSynth = 5,   // DSN analog voice: 2 VCO + VCF + 2 EG + 2 MG
    // Macrosynth, MIDI - later
};

constexpr int INSTRUMENT_TYPE_COUNT = 6;   // for UI clamps

struct Instrument {
    InstrumentType type = InstrumentType::None;
    char name[16] = {0};
    uint8_t table_id = seq::EMPTY;   // mod table or EMPTY
    bool    poly = false;            // false = MONO (a new note sends old ones into release), true = POLY (accumulate up to 4)

    // === per-instrument FX defaults (M8-style) ===
    // applied to the track at the start of a step that triggers this instrument,
    // BEFORE the step's own FX commands (so FIL/SND/RES in the phrase override them).
    // stored compactly (0..255 / signed) and converted to q15 when applied.
    // defaults below = "do nothing" so existing projects are unchanged.
    uint8_t fx_filter_type = 0;      // 0=off, 1=LP, 2=HP, 3=BP, 4=Notch
    uint8_t fx_cutoff      = 255;    // 255 = fully open
    uint8_t fx_resonance   = 0;      // 0 = none
    uint8_t fx_send_del    = 0;      // 0 = dry; >0 = send to delay bus (m8 DEL)
    uint8_t fx_send_rev    = 0;      // 0 = dry; >0 = send to reverb bus (m8 REV)
    uint8_t fx_volume      = 255;    // 255 = unity
    int8_t  fx_pan         = 0;      // -128..127, 0 = center
    uint8_t fx_bits        = 16;     // 16 = clean; <16 = bitcrush

    union {
        synth::WavsynthParams wavsynth;
        synth::SamplerParams  sampler;
        synth::DrumKitParams  drumkit;
        synth::FmSynthParams  fm;
        synth::DsnSynthParams dsn;
    };
    Instrument() : type(InstrumentType::None), wavsynth{} {}
};

class Project {
public:
    Song   song;
    Chain  chains[MAX_CHAINS];
    Phrase phrases[MAX_PHRASES];
    Table  tables[MAX_TABLES];
    Instrument instruments[MAX_INSTRUMENTS];

    char name[24] = "untitled";

    // per-table playback speed: ticks per row, 1 = every tick (m8 TIC01), up to 16.
    // 0 = legacy files = treated as 1. APPENDED AT THE END of the struct so v10
    // project files load as a clean prefix (serialize.cpp zero-fills the tail).
    uint8_t table_speed[MAX_TABLES] = {};

    // v12 tail: global reverb character (0 = legacy file -> use built-in default).
    // UI writes 1..255. lives in Project (not Song) to keep the serialized prefix
    // layout intact for older files.
    uint8_t rev_size = 0;   // comb feedback / room size (0 -> default ~0.65)
    uint8_t rev_damp = 0;   // feedback lowpass damping  (0 -> default ~30%)

    // create a voice for the instrument (caller owns)
    audio::Voice* make_voice(uint8_t instrument_id);

    // push current params into an already-sounding voice of this instrument
    // (live tweak on held notes). returns true if the voice was refreshed.
    // caller must hold the mixer audio lock.
    bool refresh_voice_params(audio::Voice* v, uint8_t instrument_id);

    // === clone (lsdj/m8-style "make unique copy") ===
    // a slot is FREE when it has no content AND nothing references it
    // (an empty phrase can legitimately be used as a rest).
    bool phrase_empty(uint8_t id) const;        // no notes/insts/fx
    bool chain_empty(uint8_t id) const;         // all rows EMPTY
    bool phrase_referenced(uint8_t id) const;   // used by any chain row
    bool chain_referenced(uint8_t id) const;    // used by any song cell
    // first free slot scanning from near+1 (wraps) so clones land close to the source.
    // -1 = no free slot.
    int find_free_phrase(uint8_t near_id) const;
    int find_free_chain(uint8_t near_id) const;
    // copy phrase src into a free slot. returns new id or -1 (bank full).
    int clone_phrase(uint8_t src);
    // deep clone: copy chain src AND every phrase it references into free slots,
    // remapping the new chain's rows (same phrase used twice -> one new copy).
    // if the phrase bank runs out mid-way, remaining rows keep the ORIGINAL
    // phrase ids (still playable, just shared). returns new chain id or -1.
    int clone_chain_deep(uint8_t src);
};

} // namespace trackr::seq
