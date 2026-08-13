// undo/redo subsystem — embedded-friendly, zero heap.
//
// Design (confirmed against Furnace tracker + Max Liani "Undo, the art of"):
//   - DELTA records, not full-project snapshots. A full Project snapshot would be
//     hundreds of KB (sample bank!) × 64 = tens of MB — would kill old-3DS RAM.
//   - One edit = a small before/after pair of the touched cell. ~A PhraseStep is 8 bytes.
//   - Fixed ring buffer, no malloc — no heap fragmentation on 3DS, identical cost on old/new.
//   - Coalescing: consecutive edits of the SAME cell within a short window collapse into one
//     undo entry (so "held A, +10" is one undo, not ten).
//
// v1 handles PhraseStep edits (~90% of all editing). The record format already carries
// coordinates for Chain/Song/etc so those can be wired later without changing the buffer.
#pragma once
#include "types.h"
#include "project.h"
#include <cstdint>
#include <cstring>

namespace trackr::seq {

// What kind of object a record touches. Coordinates in (a,b) are interpreted per-kind.
enum class EditKind : uint8_t {
    None      = 0,
    Step,        // a = phrase id, b = step index;        payload = PhraseStep
    ChainRow,    // a = chain id,  b = row index;          payload = ChainRow
    SongCell,    // a = song row,  b = track;              payload = uint8_t (chain id)
    // (InstByte / TableCell can be added later — buffer format already fits)
};

// One reversible edit. before/after hold the FULL touched cell (cheap: max 9 bytes).
struct EditRecord {
    EditKind kind = EditKind::None;
    uint16_t a = 0;
    uint16_t b = 0;
    // payload union — biggest member is PhraseStep (9 bytes). Stored by value.
    union Payload {
        PhraseStep step;
        ChainRow   chain;
        uint8_t    song_cell;
        Payload() : step{} {}
    } before, after;
    uint32_t frame = 0;   // app frame when recorded (for coalescing window)
    uint16_t group = 0;   // nonzero = adjacent records undo/redo as one operation
};

// Whole-instrument snapshot. Instrument is a POD (union of param structs), so a
// byte copy is a valid save/restore.
// Kept in a SEPARATE, smaller ring: at 112 bytes a before/after pair is ~240
// bytes, and paying that for every note nudge would cost 15 KB of static RAM.
// Instrument edits are rare and coarse, so 12 of them is plenty.
struct InstRecord {
    uint16_t   id = 0;
    Instrument before;
    Instrument after;
    uint32_t   frame = 0;
};

class UndoStack {
public:
    static constexpr int CAP = 64;   // ~64 * sizeof(EditRecord) ≈ 1.5 KB static, fixed.

    // Snapshot a cell's current value into `out`. Used to grab `before` prior to an edit
    // and `after` right after. Returns false if coords are out of range.
    static bool read_cell(const Project& p, EditKind kind, uint16_t a, uint16_t b,
                          EditRecord::Payload& out);
    // Apply a payload back into the project (used by undo→before / redo→after).
    static void write_cell(Project& p, EditKind kind, uint16_t a, uint16_t b,
                           const EditRecord::Payload& in);

    // Record an edit. `before` must have been captured BEFORE the mutation; `after` after.
    // If the last record targets the same cell within `coalesce_frames`, it is merged
    // (we keep the original `before`, update `after`) instead of pushing a new entry.
    void record(EditKind kind, uint16_t a, uint16_t b,
                const EditRecord::Payload& before,
                const EditRecord::Payload& after,
                uint32_t frame, uint32_t coalesce_frames = 30);

    // Batch edits (phrase tools): every record between begin/end is undone/redone
    // together. Nested begin calls are ignored; zero remains the ungrouped sentinel.
    void begin_group();
    void end_group() { active_group_ = 0; }

    // Undo the most recent edit (writes `before` back). Returns false if nothing to undo.
    // On success, fills out_kind/out_a/out_b so the UI can move the cursor to the change.
    bool undo(Project& p, EditKind& out_kind, uint16_t& out_a, uint16_t& out_b,
              uint16_t* step_mask = nullptr);
    // Redo. Returns false if nothing to redo.
    bool redo(Project& p, EditKind& out_kind, uint16_t& out_a, uint16_t& out_b,
              uint16_t* step_mask = nullptr);

    bool can_undo() const { return count_ > 0; }
    bool can_redo() const { return redo_ > 0; }
    void clear() { head_ = 0; count_ = 0; redo_ = 0; active_group_ = 0; }

private:
    EditRecord ring_[CAP];
    // head_ = index just past the newest applied record (next push slot).
    // count_ = number of undoable records behind head_.
    // redo_  = number of redoable records ahead of head_ (valid only until a new push).
    int head_ = 0;
    int count_ = 0;
    int redo_ = 0;
    uint16_t active_group_ = 0;
    uint16_t next_group_ = 1;

    static int prev(int i) { return (i - 1 + CAP) % CAP; }
    static int next(int i) { return (i + 1) % CAP; }
};

// Instrument-level undo, kept apart from UndoStack on purpose:
// a record is ~240 bytes (two 112-byte Instrument copies), so it gets its own
// small ring instead of inflating every note edit. Same coalescing idea: holding
// A to scroll through presets collapses into ONE undo step.
//
// Snapshot discipline is the same as steps: snapshot() before the mutation,
// commit() after. commit() with no net change pushes nothing.
class InstUndoStack {
public:
    static constexpr int CAP = 12;   // 12 * ~240B = ~2.8 KB static

    // capture the current state of instrument `id` as the pending `before`
    void snapshot(const Project& p, uint16_t id);
    // compare against the live instrument and push a record if anything changed
    bool commit(const Project& p, uint16_t id, uint32_t frame,
                uint32_t coalesce_frames = 30);

    // restore. out_id receives the instrument the UI should jump to.
    bool undo(Project& p, uint16_t& out_id);
    bool redo(Project& p, uint16_t& out_id);

    bool can_undo() const { return count_ > 0; }
    bool can_redo() const { return redo_ > 0; }
    void clear() { head_ = 0; count_ = 0; redo_ = 0; snap_taken_ = false; }

private:
    InstRecord ring_[CAP];
    int head_ = 0;
    int count_ = 0;
    int redo_ = 0;

    Instrument snap_;                 // pending `before`
    uint16_t   snap_id_ = 0;
    bool       snap_taken_ = false;

    static int prev(int i) { return (i - 1 + CAP) % CAP; }
    static int next(int i) { return (i + 1) % CAP; }
};

} // namespace trackr::seq
