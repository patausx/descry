// Regression for the global Song-row clock.
//
// A Song row is one shared time container: its duration is the longest chain in
// that row. Short chains loop inside it, EMPTY cells wait silently, and no track
// may race into another row. song_wrapped() is emitted only by the shared boundary.
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

int main() {
    constexpr int SR = 32000;
    constexpr int CHUNK = 64;
    auto* p = new seq::Project();
    p->song.bpm = 240; // one 16-step phrase = exactly one second

    auto& inst = p->instruments[0];
    inst.type = seq::InstrumentType::Wavsynth;
    inst.wavsynth = synth::WavsynthParams{};
    for (int ph = 0; ph < 5; ++ph) {
        auto& st = p->phrases[ph].steps[0];
        st.note = (uint8_t)(60 + ph);
        st.instrument = 0;
        st.velocity = 100;
    }

    // chain 0 lasts one phrase; chain 1 lasts four. Row 0 therefore lasts four
    // phrases. Track 2 is EMPTY and must wait for that same boundary.
    p->chains[0].rows[0].phrase = 0;
    for (int r = 0; r < 4; ++r) p->chains[1].rows[r].phrase = (uint8_t)(r + 1);
    p->song.rows[0].chain[0] = 0;
    p->song.rows[0].chain[1] = 1;
    p->song.rows[1].chain[0] = 0;
    p->song.rows[1].chain[1] = 0;
    p->song.rows[1].chain[2] = 0;

    auto* mix = new audio::Mixer();
    seq::Player player(*p, *mix);
    player.play_song(0);
    std::vector<fx::q15> buf(CHUNK * 2);

    long frames = 0;
    bool saw_short_loop = false;
    bool entered_row_1 = false;
    long row_1_frame = -1;
    while (frames < SR * 7L && !player.song_wrapped()) {
        player.advance(CHUNK, SR);
        mix->render(buf.data(), CHUNK);
        frames += CHUNK;

        const uint16_t row = player.track_state(0).song_row;
        for (int t = 1; t < seq::NUM_TRACKS; ++t)
            CHECK(player.track_state(t).song_row == row,
                  "track %d escaped shared row: T0=%u T%d=%u at %.3fs", t, row, t,
                  player.track_state(t).song_row, (double)frames / SR);

        if (frames > SR && row == 0) {
            saw_short_loop = saw_short_loop ||
                (player.track_state(0).phrase_id == 0 &&
                 player.track_state(0).play_step < 2 &&
                 player.track_state(2).phrase_id == seq::EMPTY &&
                 player.track_state(1).phrase_id != seq::EMPTY);
        }
        if (!entered_row_1 && row == 1) {
            entered_row_1 = true;
            row_1_frame = frames;
        }
    }

    CHECK(saw_short_loop, "short cell did not loop while EMPTY waited and long chain continued");
    CHECK(entered_row_1, "song never entered row 1");
    CHECK(row_1_frame > (long)(SR * 3.9) && row_1_frame < (long)(SR * 4.2),
          "row 1 started at %.3fs, expected ~4s", (double)row_1_frame / SR);
    CHECK(player.song_wrapped(), "shared song clock never wrapped");
    CHECK(frames > (long)(SR * 4.8) && frames < (long)(SR * 5.2),
          "song wrapped at %.3fs, expected ~5s", (double)frames / SR);

    delete mix;
    delete p;
    if (failures) { std::printf("test_song_sync: %d failure(s)\n", failures); return 1; }
    std::puts("test_song_sync: all checks passed");
    return 0;
}
