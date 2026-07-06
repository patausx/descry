// host-side render test: run the REAL engine (Player+Mixer) over the moonlight
// project for N seconds and analyze the output for clicks/farts.
// build: g++ -std=c++17 -I. -O2 tools/host_render.cpp \
//          core/audio/mixer.cpp core/audio/fixed.cpp core/sequencer/player.cpp \
//          core/sequencer/project.cpp core/synth/fm.cpp core/synth/dsn_synth.cpp \
//          core/synth/wavsynth.cpp core/synth/sampler.cpp core/synth/drumkit.cpp \
//          core/synth/wavetable.cpp core/synth/wav_loader.cpp core/dsp/*.cpp? (none)
//          -o /tmp/hostrender
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
    int seconds = argc > 2 ? atoi(argv[2]) : 40;
    // argv[3]: optional bisect switch: "norev" | "solo0".."solo7"
    const char* mode = argc > 3 ? argv[3] : "";

    static seq::Project proj;
    if (!seq::load_project(proj, path)) { std::printf("load failed\n"); return 1; }
    std::printf("loaded %s (%s) mode=%s\n", path, proj.name, mode[0] ? mode : "full");

    if (!std::strcmp(mode, "norev")) proj.song.rev_wet = 0;
    if (!std::strncmp(mode, "solo", 4)) {
        int keep = mode[4] - '0';
        for (int t = 0; t < 8; ++t)
            if (t != keep) proj.song.track_vol[t] = 0;
    }

    static audio::Mixer mixer;
    // song settings -> mixer (main.cpp does this via sync_mixer in the UI; here manual)
    mixer.reverb_wet = (fx::q15)((int)proj.song.rev_wet * fx::Q15_ONE / 255);
    mixer.delay_wet  = (fx::q15)((int)proj.song.dly_wet * fx::Q15_ONE / 255);
    mixer.delay_feedback = (fx::q15)((int)proj.song.dly_fb * fx::Q15_ONE / 255);
    for (int t = 0; t < 8; ++t)
        mixer.track(t).mix_vol = (fx::q15)((int)proj.song.track_vol[t] * fx::Q15_ONE / 255);
    seq::Player player(proj, mixer);
    player.play_song(0);

    constexpr int SR = 32000;
    constexpr int CHUNK = 256;
    const long total = (long)SR * seconds;
    std::vector<fx::q15> buf(CHUNK * 2);
    std::vector<fx::q15> all;
    all.reserve(total * 2);

    for (long done = 0; done < total; done += CHUNK) {
        player.advance(CHUNK, SR);
        mixer.render(buf.data(), CHUNK);
        all.insert(all.end(), buf.begin(), buf.end());
    }

    // analysis: per-0.5s window - peak, max sample jump, and KNEE% - share of
    // samples in the soft-clip compression zone (|v|>24000). sustained knee%
    // = sustained saturation = the "farting" sound.
    std::printf("\n t(s) | peak  | maxjump | knee%%\n");
    const long win = SR / 2;
    for (long w = 0; w * win * 2 < (long)all.size(); ++w) {
        long start = w * win * 2;
        long end = std::min(start + win * 2, (long)all.size());
        int peak = 0, maxjump = 0; long knee = 0;
        for (long i = start + 2; i < end; ++i) {
            int v = std::abs((int)all[i]);
            if (v > peak) peak = v;
            if (v > 24000) ++knee;
            int j = std::abs((int)all[i] - (int)all[i - 2]);   // same channel
            if (j > maxjump) maxjump = j;
        }
        std::printf("%5.1f | %5d | %7d | %5.2f\n",
                    w * 0.5, peak, maxjump, 100.0 * knee / (end - start));
    }

    // write wav for listening on the host
    FILE* f = std::fopen("/tmp/render.wav", "wb");
    if (f) {
        uint32_t data_bytes = (uint32_t)(all.size() * 2);
        uint32_t riff_sz = 36 + data_bytes;
        uint16_t ch = 2, bits = 16; uint32_t sr = SR;
        uint32_t byte_rate = sr * ch * bits / 8; uint16_t block = ch * bits / 8;
        std::fwrite("RIFF", 4, 1, f); std::fwrite(&riff_sz, 4, 1, f);
        std::fwrite("WAVE", 4, 1, f); std::fwrite("fmt ", 4, 1, f);
        uint32_t fmt_sz = 16; uint16_t pcm = 1;
        std::fwrite(&fmt_sz, 4, 1, f); std::fwrite(&pcm, 2, 1, f);
        std::fwrite(&ch, 2, 1, f); std::fwrite(&sr, 4, 1, f);
        std::fwrite(&byte_rate, 4, 1, f); std::fwrite(&block, 2, 1, f);
        std::fwrite(&bits, 2, 1, f);
        std::fwrite("data", 4, 1, f); std::fwrite(&data_bytes, 4, 1, f);
        std::fwrite(all.data(), 2, all.size(), f);
        std::fclose(f);
        std::printf("\nwrote /tmp/render.wav\n");
    }
    return 0;
}
