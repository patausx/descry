// host-side test for sample_auto_slice_transients:
// build a synthetic break (kick/snare/hat at known 16th positions, 120bpm)
// and check the detector drops markers near the real onsets.
// build: g++ -std=c++17 -I. tools/test_slice.cpp core/synth/sample_utils.cpp -o /tmp/test_slice
#include "../core/synth/sample_utils.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace trackr;

static void add_kick(std::vector<int16_t>& d, int at) {
    for (int i = 0; i < 4000 && at + i < (int)d.size(); ++i) {
        double f = 120.0 * std::exp(-i / 1200.0) + 40.0;
        double env = std::exp(-i / 1500.0);
        double v = std::sin(2 * M_PI * f * i / 32000.0) * env * 24000;
        int32_t s = d[at + i] + (int32_t)v;
        d[at + i] = (int16_t)std::max(-32768, std::min(32767, s));
    }
}
static void add_snare(std::vector<int16_t>& d, int at) {
    for (int i = 0; i < 3000 && at + i < (int)d.size(); ++i) {
        double env = std::exp(-i / 900.0);
        double v = ((rand() % 65536) - 32768) / 32768.0 * env * 18000
                 + std::sin(2 * M_PI * 180 * i / 32000.0) * env * 8000;
        int32_t s = d[at + i] + (int32_t)v;
        d[at + i] = (int16_t)std::max(-32768, std::min(32767, s));
    }
}
static void add_hat(std::vector<int16_t>& d, int at) {
    for (int i = 0; i < 800 && at + i < (int)d.size(); ++i) {
        double env = std::exp(-i / 250.0);
        double v = ((rand() % 65536) - 32768) / 32768.0 * env * 9000;
        int32_t s = d[at + i] + (int32_t)v;
        d[at + i] = (int16_t)std::max(-32768, std::min(32767, s));
    }
}

int main() {
    // 1 bar @ 120bpm @ 32k = 64000 frames, 16th = 4000 frames
    constexpr int STEP = 4000;
    synth::Sample s;
    s.channels = 1;
    s.data.assign(64000, 0);

    // classic-ish pattern: K..K S... K.K. S..h + hats on every even 16th
    int kicks[]  = { 0, 3, 8, 10 };
    int snares[] = { 4, 12 };
    std::vector<int> onsets;
    for (int k : kicks)  { add_kick(s.data, k * STEP);  onsets.push_back(k * STEP); }
    for (int sn : snares){ add_snare(s.data, sn * STEP); onsets.push_back(sn * STEP); }
    for (int h = 2; h < 16; h += 4) { add_hat(s.data, h * STEP); onsets.push_back(h * STEP); }

    for (int sens = 0; sens < 3; ++sens) {
        static const int SV[3] = { 80, 150, 220 };
        int n = synth::sample_auto_slice_transients(s, SV[sens]);
        std::printf("sens %d -> %d chops:", SV[sens], n);
        uint32_t sorted[synth::Sample::MAX_CHOPS];
        int cnt = synth::sample_chops_sorted(s, sorted);
        for (int i = 0; i < cnt; ++i) std::printf(" %u", sorted[i]);
        std::printf("\n");
        // check each real onset has a marker within 512 frames (16ms)
        int hit = 0;
        for (int o : onsets) {
            for (int i = 0; i < cnt; ++i) {
                int d = (int)sorted[i] - o;
                if (d < 0) d = -d;
                if (d <= 512) { ++hit; break; }
            }
        }
        std::printf("  matched %d/%zu real onsets\n", hit, onsets.size());
    }
    return 0;
}
