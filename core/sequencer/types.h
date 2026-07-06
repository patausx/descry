// sequencer: song > chain > phrase, like in m8
// song:   8 tracks x 256 rows, each row = chain index or empty (0xFF)
// chain:  16 rows, each = phrase index + transpose
// phrase: 16 steps, each = note + instrument + 3 fx slots
#pragma once
#include <cstdint>
#include <array>

namespace trackr::seq {

constexpr int NUM_TRACKS    = 8;
constexpr int SONG_ROWS     = 256;
constexpr int CHAIN_ROWS    = 16;
constexpr int PHRASE_STEPS  = 16;
constexpr int TICKS_PER_STEP = 6;   // M8 default: a step is subdivided into 6 ticks.
                                    // ALL FX timing (KIL/OFF/DLY/RET) is measured in ticks.
constexpr int TABLE_ROWS    = 16;
constexpr int MAX_CHAINS    = 256;
constexpr int MAX_PHRASES   = 256;
constexpr int MAX_TABLES    = 32;
constexpr int MAX_INSTRUMENTS = 128;

constexpr uint8_t EMPTY = 0xFF;

struct FxCmd {
    uint8_t cmd   = 0x00;     // effect type (0 = none)
    uint8_t value = 0x00;
};

struct PhraseStep {
    uint8_t note       = EMPTY;  // midi 0-127, EMPTY = no note
    uint8_t instrument = EMPTY;
    uint8_t velocity   = 0x7F;
    FxCmd   fx[3];
};

// === mod table ===
// 16 ticks, each with 3 fx slots. no notes
// played in a loop while the instrument's note is playing
struct TableRow {
    FxCmd fx[3];
};
struct Table {
    TableRow rows[TABLE_ROWS];
};

struct Phrase {
    PhraseStep steps[PHRASE_STEPS];
    // playable length 1..16 (steps beyond keep their data, just don't play).
    // per-phrase length = polymetry: a 12-step phrase against a 16-step one
    // drifts accents every bar. (m8 can't do this.)
    uint8_t length = PHRASE_STEPS;
};

// clamped accessor - stale/corrupt values fall back to full length
inline int phrase_len(const Phrase& p) {
    return (p.length >= 1 && p.length <= PHRASE_STEPS) ? p.length : PHRASE_STEPS;
}

struct ChainRow {
    uint8_t phrase    = EMPTY;
    int8_t  transpose = 0;
};

struct Chain {
    ChainRow rows[CHAIN_ROWS];
};

struct SongRow {
    uint8_t chain[NUM_TRACKS];
    SongRow() { for (auto& c : chain) c = EMPTY; }
};

struct Song {
    SongRow rows[SONG_ROWS];
    uint8_t bpm = 120;
    uint8_t groove = 6;   // ticks per step (m8-style). 6 = straight 16ths at the
                          // classic 24-tick beat; groove_steps[] overrides when set
    uint8_t swing = 0;    // 0 = no swing, 1..50 = % offset of off-beat steps (50 = max shuffle)

    // === mixer settings (mixer view; synced into audio::Mixer every frame) ===
    uint8_t track_vol[NUM_TRACKS] = {255,255,255,255,255,255,255,255};  // channel faders
    uint8_t master_vol = 255;
    uint8_t dly_time   = 200;  // *32 frames -> 6400 (~200ms @32k)
    uint8_t dly_fb     = 128;  // 50%
    uint8_t dly_wet    = 178;  // 70%
    uint8_t rev_wet    = 128;  // 50%

    // === sidechain duck (kick-pump) ===
    // duck_src: track whose NOTES pump the duck envelope (0xFF = off).
    // duck_rel: release 0..255 -> ~60ms..1s. track_duck[t]: per-track dip depth.
    uint8_t duck_src = 0xFF;
    uint8_t duck_rel = 60;
    uint8_t track_duck[NUM_TRACKS] = {0};

    // === groove pattern (m8-style): ticks-per-step per pattern slot ===
    // 0 = empty slot = end of pattern (loops). all-empty = fall back to `groove`.
    // classic swing = {7,5}, hard shuffle = {8,4}, straight = {6,6} or empty.
    uint8_t groove_steps[PHRASE_STEPS] = {0};

    // === key / scale (song-global) ===
    // scale_type indexes seq::SCALE_MASKS (0 = OFF/chromatic). scale_root 0..11 (C..B).
    // affects: note editing steps in-scale, touch keyboard snap + highlight,
    // pads become scale degrees. never rewrites stored notes.
    uint8_t scale_root = 0;   // 0 = C
    uint8_t scale_type = 0;   // 0 = OFF
};

// playback state of a single track
struct TrackPlayState {
    bool      playing = false;
    bool      song_mode_ = false;   // true = song-driven, false = standalone chain
    uint16_t  song_row = 0;
    uint8_t   chain_id = EMPTY;
    uint8_t   chain_row = 0;
    uint8_t   phrase_id = EMPTY;
    uint8_t   step = 0;
    int8_t    transpose = 0;
    // EVN condition: counts phrase passes on this track since play started
    // (increments every time the track finishes a phrase / starts the next row).
    // EVN 'C xy' fires when (phrase_pass % y) == x-1.
    uint8_t   phrase_pass = 0;

    // === FX intra-step state (all timers count in TICKS, M8-style) ===
    // KIL: hard-cut the note after N ticks. OFF: note-off (release) after N ticks.
    int32_t   kill_ticks    = 0;       // >0 = pending; cut/release fires when it reaches 0
    bool      kill_is_cut   = false;   // true = KIL (hard cut), false = OFF (soft release)
    // HOP: pending jump target (-1 = none). applied INSTEAD of the step increment
    // so "HOP 05" really plays step 5 next (not 6).
    int8_t    hop_target    = -1;
    // DLY: defer this step's note_on by N ticks (flam/ghost)
    int32_t   delay_ticks   = 0;       // >0 = note_on pending in this many ticks
    uint8_t   delay_note    = 0;       // note/inst/vel captured for the deferred trigger
    uint8_t   delay_inst    = EMPTY;
    uint8_t   delay_vel     = 100;
    // RET: retrigger the note every N ticks, for a limited count
    uint8_t   retrig_count  = 0;       // how many retriggers remain (0 = inactive)
    int32_t   retrig_period = 0;       // ticks between retriggers
    int32_t   retrig_remaining = 0;    // tick countdown to the next retrigger
    // ARP: cycle through semitone offsets every N ticks while the note holds
    int32_t   arp_period    = 0;       // ticks between arp notes (0 = inactive)
    int32_t   arp_remaining = 0;       // tick countdown to the next arp note
    uint8_t   arp_offs[3]   = {0,0,0}; // semitone offsets: [0]=base(0), [1]=hi nibble, [2]=lo nibble
    uint8_t   arp_len       = 0;       // how many offsets in the cycle (1..3)
    uint8_t   arp_idx       = 0;       // current position in the cycle
    uint8_t   last_note     = 0;       // last played note (for retrig/arp)
    uint8_t   last_inst     = EMPTY;
    uint8_t   last_velocity = 100;

    // === mod table state ===
    uint8_t   table_id = EMPTY;        // index of the active table (EMPTY = no modulation)
    uint8_t   table_row = 0;           // current row in the table
    bool      table_active = false;    // true while the note is alive
    uint8_t   table_tick_ctr = 0;      // ticks since the last row advance (SPD divider)
};

} // namespace trackr::seq
