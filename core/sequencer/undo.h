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
#include <cstdint>
#include <cstring>

namespace trackr::seq {

class Project;  // fwd

// What kind of object a record touches. Coordinates in (a,b) are interpreted per-kind.
enum class EditKind : uint8_t {
    None      = 0,
    Step,        // a = phrase id, b = step index;        payload = PhraseStep
    ChainRow,    // a = chain id,  b = row index;          payload = ChainRow
    SongCell,    // a = song row,  b = track;              payload = uint8_t (chain id)
    // (InstByte / TableCell can be added later — buffer format already fits)
};

// One reversible edit. before/after hold the FULL touched cell (cheap: max 8 bytes).
struct EditRecord {
    EditKind kind = EditKind::None;
    uint16_t a = 0;
    uint16_t b = 0;
    // payload union — biggest member is PhraseStep (8 bytes). Stored by value.
    union Payload {
        PhraseStep step;
        ChainRow   chain;
        uint8_t    song_cell;
        Payload() : step{} {}
    } before, after;
    uint32_t frame = 0;   // app frame when recorded (for coalescing window)
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

    // Undo the most recent edit (writes `before` back). Returns false if nothing to undo.
    // On success, fills out_kind/out_a/out_b so the UI can move the cursor to the change.
    bool undo(Project& p, EditKind& out_kind, uint16_t& out_a, uint16_t& out_b);
    // Redo. Returns false if nothing to redo.
    bool redo(Project& p, EditKind& out_kind, uint16_t& out_a, uint16_t& out_b);

    bool can_undo() const { return count_ > 0; }
    bool can_redo() const { return redo_ > 0; }
    void clear() { head_ = 0; count_ = 0; redo_ = 0; }

private:
    EditRecord ring_[CAP];
    // head_ = index just past the newest applied record (next push slot).
    // count_ = number of undoable records behind head_.
    // redo_  = number of redoable records ahead of head_ (valid only until a new push).
    int head_ = 0;
    int count_ = 0;
    int redo_ = 0;

    static int prev(int i) { return (i - 1 + CAP) % CAP; }
    static int next(int i) { return (i + 1) % CAP; }
};

} // namespace trackr::seq
