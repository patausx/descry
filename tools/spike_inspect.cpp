// spike inspector: renders the project and dumps context around big jumps
// build: same objects as host_render
#include "core/sequencer/project.h"
#include "core/sequencer/player.h"
#include "core/sequencer/serialize.h"
#include "core/audio/mixer.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

using namespace trackr;

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "/tmp/moonlight.tr3d";
    double t_from = argc > 3 ? atof(argv[2]) : 20.0;
    double t_to   = argc > 3 ? atof(argv[3]) : 23.0;

    static seq::Project proj;
    if (!seq::load_project(proj, path)) return 1;
    static audio::Mixer mixer;
    mixer.reverb_wet = (fx::q15)((int)proj.song.rev_wet * fx::Q15_ONE / 255);
    mixer.delay_wet  = (fx::q15)((int)proj.song.dly_wet * fx::Q15_ONE / 255);
    mixer.delay_feedback = (fx::q15)((int)proj.song.dly_fb * fx::Q15_ONE / 255);
    for (int t = 0; t < 8; ++t)
        mixer.track(t).mix_vol = (fx::q15)((int)proj.song.track_vol[t] * fx::Q15_ONE / 255);
    seq::Player player(proj, mixer);
    player.play_song(0);

    constexpr int SR = 32000, CHUNK = 256;
    long total = (long)(SR * (t_to + 0.5));
    std::vector<fx::q15> all;
    all.reserve(total * 2);
    std::vector<fx::q15> buf(CHUNK * 2);
    for (long done = 0; done < total; done += CHUNK) {
        player.advance(CHUNK, SR);
        mixer.render(buf.data(), CHUNK);
        all.insert(all.end(), buf.begin(), buf.end());
    }

    long i0 = (long)(t_from * SR) * 2, i1 = (long)(t_to * SR) * 2;
    int shown = 0;
    for (long i = i0 + 2; i < i1 && shown < 12; i += 2) {           // L channel
        int j = std::abs((int)all[i] - (int)all[i - 2]);
        if (j > 1500) {
            std::printf("t=%.4fs jump=%d ctx:", (double)(i / 2) / SR, j);
            for (long k = i - 12; k <= i + 12; k += 2)
                std::printf(" %d", (int)all[k]);
            std::printf("\n");
            ++shown;
            i += 64;   // skip past this event
        }
    }
    if (!shown) std::printf("no jumps >1500 in [%.1f..%.1f]s\n", t_from, t_to);
    return 0;
}
