// undo/redo implementation. See undo.h for design rationale.
#include "undo.h"
#include "project.h"

namespace trackr::seq {

bool UndoStack::read_cell(const Project& p, EditKind kind, uint16_t a, uint16_t b,
                          EditRecord::Payload& out) {
    switch (kind) {
        case EditKind::Step:
            if (a >= MAX_PHRASES || b >= PHRASE_STEPS) return false;
            out.step = p.phrases[a].steps[b];
            return true;
        case EditKind::ChainRow:
            if (a >= MAX_CHAINS || b >= CHAIN_ROWS) return false;
            out.chain = p.chains[a].rows[b];
            return true;
        case EditKind::SongCell:
            if (a >= SONG_ROWS || b >= NUM_TRACKS) return false;
            out.song_cell = p.song.rows[a].chain[b];
            return true;
        default:
            return false;
    }
}

void UndoStack::write_cell(Project& p, EditKind kind, uint16_t a, uint16_t b,
                           const EditRecord::Payload& in) {
    switch (kind) {
        case EditKind::Step:
            if (a >= MAX_PHRASES || b >= PHRASE_STEPS) return;
            p.phrases[a].steps[b] = in.step;
            return;
        case EditKind::ChainRow:
            if (a >= MAX_CHAINS || b >= CHAIN_ROWS) return;
            p.chains[a].rows[b] = in.chain;
            return;
        case EditKind::SongCell:
            if (a >= SONG_ROWS || b >= NUM_TRACKS) return;
            p.song.rows[a].chain[b] = in.song_cell;
            return;
        default:
            return;
    }
}

void UndoStack::record(EditKind kind, uint16_t a, uint16_t b,
                       const EditRecord::Payload& before,
                       const EditRecord::Payload& after,
                       uint32_t frame, uint32_t coalesce_frames) {
    // No-op edits don't pollute the stack (e.g. value clamped at its limit).
    if (std::memcmp(&before, &after, sizeof(EditRecord::Payload)) == 0) return;

    // Coalesce: if the newest record is the same cell and recent, just extend its `after`.
    // (redo_ must be 0 — once you've undone, the next edit starts a fresh branch.)
    if (count_ > 0 && redo_ == 0) {
        EditRecord& last = ring_[prev(head_)];
        if (last.kind == kind && last.a == a && last.b == b &&
            frame - last.frame <= coalesce_frames) {
            last.after = after;
            last.frame = frame;
            // If the coalesced result equals the original before, the net edit is nothing:
            // drop the record entirely so undo doesn't leave a dead entry.
            if (std::memcmp(&last.before, &last.after, sizeof(EditRecord::Payload)) == 0) {
                head_ = prev(head_);
                --count_;
            }
            return;
        }
    }

    // Fresh record. Pushing invalidates any redo branch.
    EditRecord& rec = ring_[head_];
    rec.kind = kind;
    rec.a = a;
    rec.b = b;
    rec.before = before;
    rec.after = after;
    rec.frame = frame;

    head_ = next(head_);
    if (count_ < CAP) ++count_;     // ring overwrites oldest once full
    redo_ = 0;
}

bool UndoStack::undo(Project& p, EditKind& out_kind, uint16_t& out_a, uint16_t& out_b) {
    if (count_ == 0) return false;
    int idx = prev(head_);
    const EditRecord& rec = ring_[idx];
    write_cell(p, rec.kind, rec.a, rec.b, rec.before);
    out_kind = rec.kind; out_a = rec.a; out_b = rec.b;
    head_ = idx;
    --count_;
    ++redo_;
    return true;
}

bool UndoStack::redo(Project& p, EditKind& out_kind, uint16_t& out_a, uint16_t& out_b) {
    if (redo_ == 0) return false;
    const EditRecord& rec = ring_[head_];
    write_cell(p, rec.kind, rec.a, rec.b, rec.after);
    out_kind = rec.kind; out_a = rec.a; out_b = rec.b;
    head_ = next(head_);
    ++count_;
    --redo_;
    return true;
}

// === instrument-level undo ===
// whole-Instrument snapshots. covers what cell-level undo could never reach:
// preset loads and type switches, which overwrite EVERY param plus the name in
// one keypress and used to be unrecoverable.

void InstUndoStack::snapshot(const Project& p, uint16_t id) {
    if (id >= MAX_INSTRUMENTS) { snap_taken_ = false; return; }
    snap_ = p.instruments[id];
    snap_id_ = id;
    snap_taken_ = true;
}

void InstUndoStack::commit(const Project& p, uint16_t id, uint32_t frame,
                           uint32_t coalesce_frames) {
    if (!snap_taken_ || id != snap_id_ || id >= MAX_INSTRUMENTS) { snap_taken_ = false; return; }
    snap_taken_ = false;
    const Instrument& now = p.instruments[id];
    // no net change -> nothing to remember (value clamped at its limit, etc.)
    if (std::memcmp(&snap_, &now, sizeof(Instrument)) == 0) return;

    // coalesce: same instrument, recent -> extend the existing record's `after`,
    // so holding A through 12 presets is ONE undo, not twelve.
    if (count_ > 0 && redo_ == 0) {
        InstRecord& last = ring_[prev(head_)];
        if (last.id == id && frame - last.frame <= coalesce_frames) {
            last.after = now;
            last.frame = frame;
            if (std::memcmp(&last.before, &last.after, sizeof(Instrument)) == 0) {
                head_ = prev(head_);
                --count_;
            }
            return;
        }
    }

    InstRecord& rec = ring_[head_];
    rec.id = id;
    rec.before = snap_;
    rec.after = now;
    rec.frame = frame;
    head_ = next(head_);
    if (count_ < CAP) ++count_;
    redo_ = 0;
}

bool InstUndoStack::undo(Project& p, uint16_t& out_id) {
    if (count_ == 0) return false;
    int idx = prev(head_);
    const InstRecord& rec = ring_[idx];
    if (rec.id < MAX_INSTRUMENTS) p.instruments[rec.id] = rec.before;
    out_id = rec.id;
    head_ = idx;
    --count_;
    ++redo_;
    return true;
}

bool InstUndoStack::redo(Project& p, uint16_t& out_id) {
    if (redo_ == 0) return false;
    const InstRecord& rec = ring_[head_];
    if (rec.id < MAX_INSTRUMENTS) p.instruments[rec.id] = rec.after;
    out_id = rec.id;
    head_ = next(head_);
    ++count_;
    --redo_;
    return true;
}

} // namespace trackr::seq
