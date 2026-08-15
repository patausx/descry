// Host regression for the offline render path (song export).
//
// Covers three bugs that shipped in v1.0.5 and earlier:
//   1. render used a DEFAULT audio::Mixer - channel faders, master volume,
//      delay/reverb settings and the mute mask never reached the wav. This test
//      asserts seq::Player::apply_song_mixer actually transfers them.
//   2. the render loop stopped after 1 second of silence, so a song with a rest
//      longer than a second was truncated. Asserted via a song whose first bar
//      is empty: a naive "stop on silence" cut would end before the notes.
//   3. song playback loops forever, so the export had no real end marker and
//      relied on a hard 60s cap. Player::song_wrapped() must go true exactly
//      after one full pass over the content rows, and never before.
//
// Also checks that the persisted mute mask survives play_song() (PLAY used to
// blanket-unmute every track).
//
// build: part of `make tests`
#include "core/sequencer/project.h"
#include "core/sequencer/player.h"
#include "core/audio/mixer.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>

using namespace trackr;

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   std::printf(__VA_ARGS__); std::printf("\n"); ++failures; } \
} while (0)

// a minimal audible project: one wavsynth instrument, notes on track 0.
// `lead_silent_rows` empty song rows are placed BEFORE the row with content, so
// the render has to survive a long musical rest at the start.
static void build_project(seq::Project& p, int lead_silent_rows, int content_rows) {
    p.song.bpm = 240;                 // fast: keeps the test quick
    auto& inst = p.instruments[0];
    inst = seq::Instrument{};
    inst.type = seq::InstrumentType::Wavsynth;
    inst.wavsynth = synth::WavsynthParams{};

    // phrase 0: a note on every 4th step
    for (int s = 0; s < seq::PHRASE_STEPS; ++s) {
        auto& st = p.phrases[0].steps[s];
        if (s % 4 == 0) { st.note = 60; st.instrument = 0; st.velocity = 100; }
    }
    p.chains[0].rows[0].phrase = 0;

    for (int r = 0; r < content_rows; ++r)
        p.song.rows[lead_silent_rows + r].chain[0] = 0;
}

// render like platform/3ds/main.cpp does, returning peak level and frame count.
struct RenderResult {
    long   frames = 0;
    int    peak = 0;
    bool   wrapped = false;
};
static RenderResult render(seq::Project& p, audio::Mixer& mix, bool apply_song_mixer,
                           long max_frames) {
    constexpr int SR = 32000;
    constexpr long CHUNK = 256;
    if (apply_song_mixer) seq::Player::apply_song_mixer(p, mix);
    seq::Player player(p, mix);
    player.play_song(0);

    RenderResult r;
    std::vector<fx::q15> buf(CHUNK * 2);
    long tail = 0;
    while (r.frames < max_frames) {
        player.advance(CHUNK, SR);
        mix.render(buf.data(), CHUNK);
        for (long i = 0; i < CHUNK * 2; ++i) {
            int v = std::abs((int)buf[i]);
            if (v > r.peak) r.peak = v;
        }
        r.frames += CHUNK;
        if (player.song_wrapped()) {
            r.wrapped = true;
            tail += CHUNK;
            if (tail >= SR) break;          // short tail, enough for the test
        }
    }
    for (int t = 0; t < seq::NUM_TRACKS; ++t) mix.clear_voices(t);
    return r;
}

int main() {
    constexpr int SR = 32000;

    // === 1. faders reach the render ===
    // full-volume reference vs the same song with track 0 pulled to zero.
    {
        auto* p = new seq::Project();
        build_project(*p, 0, 1);
        p->song.track_vol[0] = 255;
        p->song.master_vol   = 255;

        auto* mix = new audio::Mixer();
        RenderResult loud = render(*p, *mix, true, SR * 6);
        delete mix;
        CHECK(loud.peak > 1000, "reference render is silent (peak %d)", loud.peak);

        // pull the fader down: a render that honors the mixer must get quieter
        p->song.track_vol[0] = 0;
        auto* mix2 = new audio::Mixer();
        RenderResult quiet = render(*p, *mix2, true, SR * 6);
        delete mix2;
        CHECK(quiet.peak * 4 < loud.peak,
              "track fader ignored by render: loud peak %d, fader-0 peak %d",
              loud.peak, quiet.peak);

        // and the bug itself: skipping apply_song_mixer must NOT sound the same
        // as honoring it (this is what the old renderer did).
        auto* mix3 = new audio::Mixer();
        RenderResult unsynced = render(*p, *mix3, false, SR * 6);
        delete mix3;
        CHECK(unsynced.peak > quiet.peak * 4,
              "test is blind: unsynced render (%d) should differ from synced (%d)",
              unsynced.peak, quiet.peak);
        delete p;
    }

    // === 2. master volume reaches the render ===
    {
        auto* p = new seq::Project();
        build_project(*p, 0, 1);
        p->song.master_vol = 255;
        auto* m1 = new audio::Mixer();
        int loud = render(*p, *m1, true, SR * 6).peak;
        delete m1;

        p->song.master_vol = 16;
        auto* m2 = new audio::Mixer();
        int soft = render(*p, *m2, true, SR * 6).peak;
        delete m2;
        CHECK(soft * 2 < loud, "master volume ignored by render: %d vs %d", loud, soft);
        delete p;
    }

    // === 3. a long musical rest must not end the export ===
    // 2 empty song rows at 240bpm = ~4s of silence before the first note.
    // the old "stop after 1s of silence" rule ended the render here.
    {
        auto* p = new seq::Project();
        build_project(*p, 2, 1);
        auto* mix = new audio::Mixer();
        RenderResult r = render(*p, *mix, true, SR * 20);
        delete mix;
        CHECK(r.peak > 1000,
              "render found no audio after a multi-second rest (peak %d) - "
              "silence bail-out truncated the song", r.peak);
        delete p;
    }

    // === 4. song_wrapped(): exactly one pass, not before ===
    {
        auto* p = new seq::Project();
        build_project(*p, 0, 4);          // 4 content rows
        auto* mix = new audio::Mixer();
        seq::Player::apply_song_mixer(*p, *mix);
        seq::Player player(*p, *mix);
        player.play_song(0);

        constexpr long CHUNK = 256;
        long frames = 0;
        long wrap_frame = -1;
        std::vector<fx::q15> buf(CHUNK * 2);
        while (frames < (long)SR * 30) {
            player.advance(CHUNK, SR);
            mix->render(buf.data(), CHUNK);
            frames += CHUNK;
            if (player.song_wrapped()) { wrap_frame = frames; break; }
        }
        CHECK(wrap_frame > 0, "song never wrapped in 30s - render would hit the time cap");

        // 4 rows * 16 steps * 6 ticks at 240bpm. one step = 60/(240*4) s = 62.5ms
        // -> 64 steps = 4.0s. allow a generous window but catch order-of-magnitude
        // errors (e.g. wrapping on the first row).
        double wrap_s = (double)wrap_frame / SR;
        CHECK(wrap_s > 3.0 && wrap_s < 5.5,
              "wrap at %.2fs, expected ~4.0s (one pass of 4 rows)", wrap_s);

        for (int t = 0; t < seq::NUM_TRACKS; ++t) mix->clear_voices(t);
        delete mix;
        delete p;
    }

    // === 5. play_song must not clear the persisted mute mask ===
    {
        auto* p = new seq::Project();
        build_project(*p, 0, 1);
        p->track_mute = 0x05;             // tracks 0 and 2 muted
        auto* mix = new audio::Mixer();
        seq::Player player(*p, *mix);
        player.play_song(0);
        CHECK(mix->track(0).muted, "PLAY cleared the mute on track 0");
        CHECK(mix->track(2).muted, "PLAY cleared the mute on track 2");
        CHECK(!mix->track(1).muted, "PLAY muted track 1, which was not in the mask");
        delete mix;
        delete p;
    }

    // === 6. mute mask silences the render ===
    {
        auto* p = new seq::Project();
        build_project(*p, 0, 1);
        auto* m1 = new audio::Mixer();
        int loud = render(*p, *m1, true, SR * 6).peak;
        delete m1;

        p->track_mute = 0x01;             // the only sounding track
        auto* m2 = new audio::Mixer();
        int muted = render(*p, *m2, true, SR * 6).peak;
        delete m2;
        CHECK(loud > 1000, "reference render silent (%d)", loud);
        CHECK(muted == 0, "muted track still audible in the render (peak %d)", muted);
        delete p;
    }

    // === 7. offline stop-at-wrap must not trigger the next loop ===
    // Live mode loops normally; export mode stops song tracks at the shared
    // boundary so row 0 cannot leak into the delay/reverb tail.
    {
        auto* p = new seq::Project();
        build_project(*p, 0, 1);
        auto* mix = new audio::Mixer();
        seq::Player player(*p, *mix);
        player.set_stop_at_song_wrap(true);
        player.play_song(0);
        constexpr long CHUNK = 32;
        std::vector<fx::q15> buf(CHUNK * 2);
        long frames = 0;
        while (!player.song_wrapped() && frames < SR * 4L) {
            player.advance(CHUNK, SR);
            mix->render(buf.data(), CHUNK);
            frames += CHUNK;
        }
        CHECK(player.song_wrapped(), "single-row song never wrapped");
        CHECK(player.track_state(0).song_row == 0, "wrapped song must report row 0");
        CHECK(!player.track_state(0).playing,
              "offline stop-at-wrap left song track playing into the next loop");
        CHECK(player.track_state(0).play_step == 15,
              "offline wrap should preserve last sounding step 15, got %d",
              player.track_state(0).play_step);
        delete mix;
        delete p;
    }

    if (failures) { std::printf("test_render_mix: %d FAILURE(S)\n", failures); return 1; }
    std::printf("test_render_mix: all checks passed\n");
    return 0;
}
