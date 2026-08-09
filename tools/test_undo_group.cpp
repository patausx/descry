// Host regression for grouped UndoStack records used by Phrase Tools.
#include "../core/sequencer/project.h"
#include "../core/sequencer/undo.h"
#include <cstdio>

using namespace trackr;

#define CHECK(c, msg) do { if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", msg); return 1; } } while (0)

static void record_note(seq::UndoStack& u, seq::Project& p, int row, uint8_t note,
                        uint32_t frame) {
    seq::EditRecord::Payload before{}, after{};
    before.step = p.phrases[0].steps[row];
    p.phrases[0].steps[row].note = note;
    after.step = p.phrases[0].steps[row];
    u.record(seq::EditKind::Step, 0, (uint16_t)row, before, after, frame, 0);
}

int main() {
    seq::Project p;
    seq::UndoStack u;
    for (int r = 0; r < 4; ++r) p.phrases[0].steps[r].note = (uint8_t)(60 + r);

    // One grouped transform changes four rows.
    u.begin_group();
    for (int r = 0; r < 4; ++r) record_note(u, p, r, (uint8_t)(72 + r), 10);
    u.end_group();

    seq::EditKind kind; uint16_t a, b;
    CHECK(u.undo(p, kind, a, b), "group undo exists");
    for (int r = 0; r < 4; ++r)
        CHECK(p.phrases[0].steps[r].note == 60 + r, "one undo restores every grouped row");

    CHECK(u.redo(p, kind, a, b), "group redo exists");
    for (int r = 0; r < 4; ++r)
        CHECK(p.phrases[0].steps[r].note == 72 + r, "one redo reapplies every grouped row");

    // A later ordinary edit remains independent.
    record_note(u, p, 7, 90, 20);
    CHECK(u.undo(p, kind, a, b), "ordinary undo exists");
    CHECK(p.phrases[0].steps[7].note == seq::EMPTY, "ordinary edit undoes alone");
    CHECK(p.phrases[0].steps[0].note == 72, "ordinary undo leaves prior group applied");

    std::puts("grouped undo: ok");
    return 0;
}
