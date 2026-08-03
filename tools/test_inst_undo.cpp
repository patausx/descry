// Host regression for instrument-level undo (seq::InstUndoStack).
//
// The cell-level UndoStack only ever covered Step/ChainRow/SongCell, so the
// most destructive edit in the app had no history at all: loading a preset (or
// switching the instrument type) overwrites every parameter AND the name in a
// single keypress, with no way back.
//
// Checks:
//   1. a preset-style overwrite is fully reversible, name included
//   2. redo replays it
//   3. holding A through several presets coalesces into ONE undo step
//   4. a no-op edit records nothing (clamped value at its limit)
//   5. the ring is bounded and the oldest record falls off instead of corrupting
//   6. clear() drops everything (used when a project is replaced)
//
// build: part of `make tests`
#include "core/sequencer/undo.h"
#include "core/synth/fm_presets.h"
#include "core/synth/wave_presets.h"
#include <cstdio>
#include <cstring>

using namespace trackr;

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   std::printf(__VA_ARGS__); std::printf("\n"); ++failures; } \
} while (0)

int main() {
    auto* p = new seq::Project();
    seq::InstUndoStack undo;

    // instrument 3 starts as a named wavsynth
    auto& inst = p->instruments[3];
    inst.type = seq::InstrumentType::Wavsynth;
    inst.wavsynth = synth::WavsynthParams{};
    inst.wavsynth.attack = 1234;
    std::snprintf(inst.name, sizeof(inst.name), "MYPATCH");

    // === 1. a preset load is reversible, name included ===
    {
        undo.snapshot(*p, 3);
        // what the UI does on a preset load: replace params + rename
        inst.type = seq::InstrumentType::FmSynth;
        synth::fm_load_preset(inst.fm, (synth::FmPreset)0);
        std::snprintf(inst.name, sizeof(inst.name), "%s",
                      synth::fm_preset_name((synth::FmPreset)0));
        undo.commit(*p, 3, /*frame*/100);

        CHECK(undo.can_undo(), "preset load left no undo record");
        CHECK(std::strcmp(p->instruments[3].name, "MYPATCH") != 0,
              "test is blind: the name did not actually change");

        uint16_t id = 0xFFFF;
        CHECK(undo.undo(*p, id), "undo refused");
        CHECK(id == 3, "undo reported instrument %u, expected 3", id);
        CHECK(p->instruments[3].type == seq::InstrumentType::Wavsynth,
              "type not restored (got %u)", (unsigned)p->instruments[3].type);
        CHECK(p->instruments[3].wavsynth.attack == 1234,
              "params not restored (attack %u)", p->instruments[3].wavsynth.attack);
        CHECK(std::strcmp(p->instruments[3].name, "MYPATCH") == 0,
              "name not restored (got '%s')", p->instruments[3].name);
    }

    // === 2. redo replays the overwrite ===
    {
        uint16_t id = 0xFFFF;
        CHECK(undo.can_redo(), "no redo available after undo");
        CHECK(undo.redo(*p, id), "redo refused");
        CHECK(id == 3, "redo reported instrument %u", id);
        CHECK(p->instruments[3].type == seq::InstrumentType::FmSynth,
              "redo did not reapply the preset");
        // put it back for the next block
        CHECK(undo.undo(*p, id), "second undo refused");
        CHECK(p->instruments[3].wavsynth.attack == 1234, "state drifted after undo/redo/undo");
    }

    // === 3. rapid consecutive edits coalesce into one undo ===
    {
        seq::InstUndoStack u2;
        auto& t = p->instruments[5];
        t.type = seq::InstrumentType::Wavsynth;
        t.wavsynth = synth::WavsynthParams{};
        t.wavsynth.attack = 0;

        // simulate holding A: 5 edits, 2 frames apart (well inside the 30-frame window)
        for (int i = 1; i <= 5; ++i) {
            u2.snapshot(*p, 5);
            t.wavsynth.attack = (uint32_t)(i * 100);
            u2.commit(*p, 5, (uint32_t)(100 + i * 2));
        }
        uint16_t id;
        CHECK(u2.undo(*p, id), "coalesced undo refused");
        CHECK(p->instruments[5].wavsynth.attack == 0,
              "coalescing broken: expected the ORIGINAL value 0, got %u",
              p->instruments[5].wavsynth.attack);
        CHECK(!u2.can_undo(), "5 rapid edits produced more than one undo record");
    }

    // === 4. edits far apart in time do NOT coalesce ===
    {
        seq::InstUndoStack u3;
        auto& t = p->instruments[6];
        t.type = seq::InstrumentType::Wavsynth;
        t.wavsynth = synth::WavsynthParams{};
        t.wavsynth.attack = 10;

        u3.snapshot(*p, 6);
        t.wavsynth.attack = 20;
        u3.commit(*p, 6, 100);

        u3.snapshot(*p, 6);
        t.wavsynth.attack = 30;
        u3.commit(*p, 6, 500);          // 400 frames later - separate gesture

        uint16_t id;
        CHECK(u3.undo(*p, id), "undo 1 refused");
        CHECK(p->instruments[6].wavsynth.attack == 20, "expected 20, got %u",
              p->instruments[6].wavsynth.attack);
        CHECK(u3.undo(*p, id), "undo 2 refused - the two gestures merged");
        CHECK(p->instruments[6].wavsynth.attack == 10, "expected 10, got %u",
              p->instruments[6].wavsynth.attack);
    }

    // === 5. a no-op edit records nothing ===
    {
        seq::InstUndoStack u4;
        u4.snapshot(*p, 3);
        // value clamped at its limit: nothing changes
        u4.commit(*p, 3, 200);
        CHECK(!u4.can_undo(), "a no-op edit still pushed an undo record");
    }

    // === 6. commit without snapshot is ignored (no garbage `before`) ===
    {
        seq::InstUndoStack u5;
        p->instruments[7].type = seq::InstrumentType::Wavsynth;
        p->instruments[7].wavsynth.attack = 42;
        u5.commit(*p, 7, 300);          // never snapshotted
        CHECK(!u5.can_undo(), "commit without snapshot invented a record");
    }

    // === 7. ring is bounded; oldest falls off, newest still undoes correctly ===
    {
        seq::InstUndoStack u6;
        auto& t = p->instruments[8];
        t.type = seq::InstrumentType::Wavsynth;
        t.wavsynth = synth::WavsynthParams{};
        const int N = seq::InstUndoStack::CAP + 5;
        for (int i = 1; i <= N; ++i) {
            u6.snapshot(*p, 8);
            t.wavsynth.attack = (uint32_t)(i * 1000);
            u6.commit(*p, 8, (uint32_t)(1000 + i * 100));   // far apart: no coalescing
        }
        int steps = 0;
        uint16_t id;
        while (u6.undo(*p, id)) {
            ++steps;
            if (steps > seq::InstUndoStack::CAP + 10) break;   // runaway guard
        }
        CHECK(steps == seq::InstUndoStack::CAP,
              "ring depth is %d, expected exactly CAP=%d", steps, seq::InstUndoStack::CAP);
    }

    // === 8. clear() drops history (project replaced) ===
    {
        seq::InstUndoStack u7;
        u7.snapshot(*p, 3);
        p->instruments[3].wavsynth.attack = 777;
        u7.commit(*p, 3, 400);
        CHECK(u7.can_undo(), "setup: nothing to clear");
        u7.clear();
        CHECK(!u7.can_undo(), "clear() left undo records behind");
        CHECK(!u7.can_redo(), "clear() left redo records behind");
    }

    // === 9. out-of-range ids are refused, not written ===
    {
        seq::InstUndoStack u8;
        u8.snapshot(*p, seq::MAX_INSTRUMENTS + 10);
        u8.commit(*p, seq::MAX_INSTRUMENTS + 10, 500);
        CHECK(!u8.can_undo(), "an out-of-range instrument id entered the history");
    }

    delete p;
    if (failures) { std::printf("test_inst_undo: %d FAILURE(S)\n", failures); return 1; }
    std::printf("test_inst_undo: all checks passed\n");
    return 0;
}
