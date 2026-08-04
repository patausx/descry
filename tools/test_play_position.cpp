// Host regression for the PLAYHEAD CONTRACT the UI draws from.
//
// The bug this locks down: the phrase view derived the playhead from
// TrackPlayState::step, which the tick engine advances IMMEDIATELY after
// triggering. So the "current" step was one ahead, and code that compensated
// with (step - 1 + 16) % 16 produced step 15 on every phrase boundary - and it
// only ever looked at track 0, so a phrase living on track 3 showed no playhead
// at all.
//
// The contract now is:
//   play_step       - the step that was last TRIGGERED (i.e. what you hear)
//   play_phrase_id  - the phrase that step came from
//   play_chain_id / play_chain_row - the chain row that phrase came from
// all valid for EVERY track, and never "one ahead".
//
// Also asserts Player::song_length() == last content row + 1 (the song view
// draws its END boundary from it).
//
// build: part of `make tests`
#include "core/sequencer/project.h"
#include "core/sequencer/player.h"
#include "core/audio/mixer.h"
#include <cstdio>
#include <vector>

using namespace trackr;

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   std::printf(__VA_ARGS__); std::printf("\n"); ++failures; } \
} while (0)

constexpr int SR = 32000;

// one instrument, phrases with a note on every step so every trigger is real
static void build(seq::Project& p) {
    p.song.bpm = 240;
    auto& inst = p.instruments[0];
    inst = seq::Instrument{};
    inst.type = seq::InstrumentType::Wavsynth;
    inst.wavsynth = synth::WavsynthParams{};
    for (int ph = 0; ph < 4; ++ph)
        for (int s = 0; s < seq::PHRASE_STEPS; ++s) {
            auto& st = p.phrases[ph].steps[s];
            st.note = (uint8_t)(60 + ph);
            st.instrument = 0;
            st.velocity = 100;
        }
}

// advance until the given track's play_step changes, then return it.
// returns -1 if it never moved (guards against infinite loops).
static int next_trigger(seq::Player& pl, audio::Mixer& mix, int track,
                        long max_frames = SR * 4) {
    constexpr long CHUNK = 32;
    std::vector<fx::q15> buf(CHUNK * 2);
    int start = pl.track_state(track).play_step;
    int start_ph = pl.track_state(track).play_phrase_id;
    for (long f = 0; f < max_frames; f += CHUNK) {
        pl.advance(CHUNK, SR);
        mix.render(buf.data(), CHUNK);
        const auto& ts = pl.track_state(track);
        if ((int)ts.play_step != start || (int)ts.play_phrase_id != start_ph)
            return ts.play_step;
    }
    return -1;
}

int main() {
    // === 1. play_step walks 0,1,2,... - never one ahead, on a NON-ZERO track ===
    {
        auto* p = new seq::Project();
        build(*p);
        auto* mix = new audio::Mixer();
        seq::Player pl(*p, *mix);
        // phrase 1 on track 3 only - exactly the "hats on T3" case that had no
        // playhead at all before.
        pl.play_phrase(3, 1);

        // first tick fires step 0 immediately
        constexpr long CHUNK = 32;
        std::vector<fx::q15> buf(CHUNK * 2);
        pl.advance(CHUNK, SR);
        mix->render(buf.data(), CHUNK);
        CHECK(pl.track_state(3).play_step == 0,
              "first triggered step should be 0, got %d", pl.track_state(3).play_step);
        CHECK(pl.track_state(3).play_phrase_id == 1,
              "play_phrase_id should be 1, got %d", pl.track_state(3).play_phrase_id);
        // track 0 must NOT claim to be playing this phrase (the old UI check)
        CHECK(pl.track_state(0).playing == false, "track 0 should be idle");

        for (int expect = 1; expect < 8; ++expect) {
            int got = next_trigger(pl, *mix, 3);
            CHECK(got == expect, "step %d expected, got %d", expect, got);
        }
        delete mix; delete p;
    }

    // === 2. phrase boundary: play_step is 15 then 0, and never reports 15 while
    //        step 0 of the next phrase is sounding ===
    {
        auto* p = new seq::Project();
        build(*p);
        // chain 0: phrase 0 then phrase 2
        p->chains[0].rows[0].phrase = 0;
        p->chains[0].rows[1].phrase = 2;
        auto* mix = new audio::Mixer();
        seq::Player pl(*p, *mix);
        pl.play_chain(2, 0);          // track 2 this time

        constexpr long CHUNK = 32;
        std::vector<fx::q15> buf(CHUNK * 2);
        pl.advance(CHUNK, SR);
        mix->render(buf.data(), CHUNK);

        // walk to the last step of phrase 0
        for (int i = 1; i <= 15; ++i) next_trigger(pl, *mix, 2);
        const auto& ts = pl.track_state(2);
        CHECK(ts.play_step == 15, "end of phrase: expected 15, got %d", ts.play_step);
        CHECK(ts.play_phrase_id == 0, "still phrase 0, got %d", ts.play_phrase_id);
        CHECK(ts.play_chain_row == 0, "still chain row 0, got %d", ts.play_chain_row);

        // one more trigger: now phrase 2, step 0, chain row 1
        int got = next_trigger(pl, *mix, 2);
        CHECK(got == 0, "after the boundary expected step 0, got %d", got);
        CHECK(pl.track_state(2).play_phrase_id == 2,
              "after the boundary expected phrase 2, got %d",
              pl.track_state(2).play_phrase_id);
        CHECK(pl.track_state(2).play_chain_row == 1,
              "after the boundary expected chain row 1, got %d",
              pl.track_state(2).play_chain_row);
        delete mix; delete p;
    }

    // === 3. two tracks playing DIFFERENT phrases report their own positions ===
    {
        auto* p = new seq::Project();
        build(*p);
        // phrase 3 is short (4 steps) so the two tracks drift apart
        p->phrases[3].length = 4;
        auto* mix = new audio::Mixer();
        seq::Player pl(*p, *mix);
        pl.play_phrase(0, 0);
        pl.play_phrase(5, 3);

        constexpr long CHUNK = 32;
        std::vector<fx::q15> buf(CHUNK * 2);
        // ~10 steps' worth of audio
        for (long f = 0; f < SR * 3; f += CHUNK) {
            pl.advance(CHUNK, SR);
            mix->render(buf.data(), CHUNK);
            CHECK(pl.track_state(0).play_phrase_id == 0 ||
                  pl.track_state(0).play_phrase_id == seq::EMPTY,
                  "track 0 drifted onto phrase %d", pl.track_state(0).play_phrase_id);
            CHECK(pl.track_state(5).play_step < 4,
                  "short phrase must stay in 0..3, got %d", pl.track_state(5).play_step);
        }
        delete mix; delete p;
    }

    // === 4. song_length() == last content row + 1 (the END line the UI draws) ===
    {
        auto* p = new seq::Project();
        build(*p);
        auto* mix = new audio::Mixer();
        seq::Player pl(*p, *mix);
        CHECK(pl.song_length() == 0, "empty song length should be 0, got %d",
              pl.song_length());

        p->song.rows[0].chain[0] = 0;
        p->song.rows[1].chain[0] = 0;
        CHECK(pl.song_length() == 2, "expected 2, got %d", pl.song_length());

        // a chain on a LATER row of a DIFFERENT track extends the song
        p->song.rows[9].chain[6] = 0;
        CHECK(pl.song_length() == 10, "expected 10, got %d", pl.song_length());
        delete mix; delete p;
    }

    if (failures) { std::printf("test_play_position: %d failure(s)\n", failures); return 1; }
    std::printf("test_play_position: all checks passed\n");
    return 0;
}
