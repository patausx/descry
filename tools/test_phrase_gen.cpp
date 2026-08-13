// Host regression for deterministic generative Phrase Tools core.
#include "../core/sequencer/phrase_gen.h"
#include "../core/sequencer/project.h"
#include "../core/sequencer/undo.h"
#include "../core/sequencer/fx.h"
#include "../core/sequencer/scale.h"
#include <cstdio>
#include <cstring>

using namespace trackr;
#define CHECK(c, msg) do { if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", msg); return 1; } } while (0)

static int notes(const seq::Phrase& p, int lo, int hi) {
    int n = 0; for (int i = lo; i <= hi; ++i) n += p.steps[i].note != seq::EMPTY; return n;
}
static seq::FxCmd* find_cmd(seq::PhraseStep& s, uint8_t cmd) {
    for (auto& f : s.fx) if (f.cmd == cmd) return &f; return nullptr;
}

int main() {
    seq::PhraseGenOptions o;
    o.seed = 0x12345678; o.amount = 3; o.default_note = 61; o.instrument = 7;
    o.scale_type = 1; o.scale_root = 0; // C major

    // Euclidean fill is range-local, exact-pulse, playable and scale-snapped.
    seq::Phrase eu{};
    eu.length = 12;
    seq::Phrase outside = eu;
    CHECK(seq::apply_phrase_generator(eu, 2, 9, seq::PhraseGenOp::Euclidean, o), "euclidean changes phrase");
    CHECK(notes(eu, 2, 9) == 3, "euclidean exact pulse count");
    CHECK(eu.steps[2].note != seq::EMPTY, "euclidean begins on range downbeat");
    for (int r = 2; r <= 9; ++r) if (eu.steps[r].note != seq::EMPTY) {
        CHECK(eu.steps[r].instrument == 7, "euclidean hit has explicit instrument");
        CHECK(seq::scale_has(1, 0, eu.steps[r].note), "euclidean note snaps to scale");
    }
    CHECK(std::memcmp(&eu.steps[0], &outside.steps[0], sizeof(seq::PhraseStep) * 2) == 0,
          "euclidean preserves rows before selection");
    CHECK(std::memcmp(&eu.steps[10], &outside.steps[10], sizeof(seq::PhraseStep) * 6) == 0,
          "euclidean preserves rows after selection");
    CHECK(eu.length == 12, "generator preserves phrase length");
    seq::Phrase zero = eu;
    o.amount = 0;
    seq::apply_phrase_generator(zero, 2, 9, seq::PhraseGenOp::Euclidean, o);
    CHECK(notes(zero, 2, 9) == 0, "zero-pulse euclidean clears every trigger");
    seq::Phrase full{};
    o.amount = 8;
    seq::apply_phrase_generator(full, 2, 9, seq::PhraseGenOp::Euclidean, o);
    CHECK(notes(full, 2, 9) == 8, "full-pulse euclidean fills every row");

    // Same input+seed is byte-identical; a stochastic seed affects density.
    seq::Phrase src{};
    for (int r = 0; r < 16; ++r) { src.steps[r].note = 60 + r; src.steps[r].instrument = 2; }
    seq::Phrase a = src, b = src, c = src;
    o.amount = 50; o.seed = 99;
    seq::apply_phrase_generator(a, 0, 15, seq::PhraseGenOp::Density, o);
    seq::apply_phrase_generator(b, 0, 15, seq::PhraseGenOp::Density, o);
    o.seed = 100;
    seq::apply_phrase_generator(c, 0, 15, seq::PhraseGenOp::Density, o);
    CHECK(std::memcmp(&a, &b, sizeof(a)) == 0, "same seed reproduces density");
    CHECK(std::memcmp(&a, &c, sizeof(a)) != 0, "different seed changes density roll");
    o.amount = 100; b = src;
    CHECK(!seq::apply_phrase_generator(b, 0, 15, seq::PhraseGenOp::Density, o), "100% density is no-op");

    // Mutation keeps notes in scale and never creates notes in rests.
    seq::Phrase mut = src;
    mut.steps[5].note = seq::EMPTY; mut.steps[5].instrument = seq::EMPTY;
    o.seed = 7; o.amount = 100; o.scale_type = 10; o.scale_root = 9; // A pentatonic minor
    seq::apply_phrase_generator(mut, 0, 15, seq::PhraseGenOp::Mutate, o);
    CHECK(mut.steps[5].note == seq::EMPTY, "mutation preserves rests");
    for (int r = 0; r < 16; ++r) if (mut.steps[r].note != seq::EMPTY)
        CHECK(seq::scale_has(o.scale_type, o.scale_root, mut.steps[r].note), "mutation stays in scale");

    // FX upsert updates existing effective command and never clobbers full rows.
    seq::Phrase rat{};
    rat.steps[0].note = 60; rat.steps[0].instrument = 1;
    rat.steps[0].fx[0] = {seq::fx_cmd::FIL, 20};
    rat.steps[0].fx[1] = {seq::fx_cmd::RTG, 6};
    rat.steps[0].fx[2] = {seq::fx_cmd::RES, 30};
    rat.steps[1].note = 62; rat.steps[1].instrument = 1;
    rat.steps[1].fx[0] = {seq::fx_cmd::FIL, 10};
    rat.steps[1].fx[1] = {seq::fx_cmd::RES, 20};
    rat.steps[1].fx[2] = {seq::fx_cmd::PAN, 30};
    seq::Phrase rat_before = rat;
    o.seed = 4; o.amount = 100;
    seq::apply_phrase_generator(rat, 0, 1, seq::PhraseGenOp::Ratchet, o);
    CHECK(find_cmd(rat.steps[0], seq::fx_cmd::RTG) != nullptr, "ratchet updates existing command");
    CHECK(rat.steps[0].fx[0].cmd == seq::fx_cmd::FIL && rat.steps[0].fx[2].cmd == seq::fx_cmd::RES,
          "ratchet preserves unrelated fx");
    CHECK(std::memcmp(&rat.steps[1], &rat_before.steps[1], sizeof(seq::PhraseStep)) == 0,
          "full fx row is skipped without clobber");

    // EVN encoding distributes 1-based passes; chance remains in valid range.
    seq::Phrase cond = src;
    o.amount = 4;
    seq::apply_phrase_generator(cond, 0, 3, seq::PhraseGenOp::Every, o);
    for (int r = 0; r < 4; ++r) {
        auto* e = find_cmd(cond.steps[r], seq::fx_cmd::EVN);
        CHECK(e && e->value == (uint8_t)(((r + 1) << 4) | 4), "EVN pass/cycle encoding");
    }
    o.amount = 40; o.seed = 9;
    seq::apply_phrase_generator(cond, 0, 3, seq::PhraseGenOp::ChanceSpread, o);
    for (int r = 0; r < 4; ++r) {
        auto* ch = find_cmd(cond.steps[r], seq::fx_cmd::CHA);
        CHECK(ch && ch->value >= 40 * 255 / 100, "chance respects minimum");
    }

    // Integration contract used by UI: all changed rows undo/redo as one action.
    seq::Project p;
    p.phrases[0] = src;
    seq::Phrase before = p.phrases[0];
    o.seed = 44; o.amount = 60;
    seq::apply_phrase_generator(p.phrases[0], 3, 12, seq::PhraseGenOp::Humanize, o);
    seq::Phrase after = p.phrases[0];
    seq::UndoStack u;
    u.begin_group();
    for (int r = 3; r <= 12; ++r) {
        if (std::memcmp(&before.steps[r], &after.steps[r], sizeof(seq::PhraseStep)) == 0) continue;
        seq::EditRecord::Payload bp{}, ap{}; bp.step = before.steps[r]; ap.step = after.steps[r];
        u.record(seq::EditKind::Step, 0, r, bp, ap, 1, 0);
    }
    u.end_group();
    seq::EditKind kind; uint16_t aa, bb;
    CHECK(u.undo(p, kind, aa, bb), "generator group undo exists");
    CHECK(std::memcmp(&p.phrases[0], &before, sizeof(before)) == 0, "one undo restores exact phrase");
    CHECK(u.redo(p, kind, aa, bb), "generator group redo exists");
    CHECK(std::memcmp(&p.phrases[0], &after, sizeof(after)) == 0, "one redo restores generated phrase");

    std::puts("phrase generators: ok");
    return 0;
}
