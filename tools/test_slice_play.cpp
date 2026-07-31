// host-side test: Sampler slice playback semantics.
//   1. chromatic slices address markers in SORTED order even when the chop
//      array has holes / out-of-order entries
//   2. THRU mode plays from the marker to the end (ignores next marker)
//   3. slice N renders exactly the region [chop N, chop N+1)
// build: g++ -std=c++17 -I. tools/test_slice_play.cpp core/synth/sampler.cpp \
//        core/synth/sample_utils.cpp core/audio/fixed.cpp -o /tmp/test_slice_play
#include "../core/synth/sampler.h"
#include "../core/synth/sample_utils.h"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace trackr;

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("FAIL: %s\n", msg); ++fails; } \
    else std::printf("ok:   %s\n", msg); } while (0)

// render the voice until inactive (cap 2s), return frames of non-silence
// and a sample value from ~frame 32 (past any envelope ramp-in)
static int render_all(synth::Sampler& v, int16_t& probe) {
    std::vector<fx::q15> buf(256 * 2);
    int nonzero = 0; bool got_probe = false;
    for (int blk = 0; blk < 900; ++blk) {
        std::memset(buf.data(), 0, buf.size() * sizeof(fx::q15));
        bool alive = v.render(buf.data(), 256);
        if (!got_probe && blk == 0) { probe = buf[32 * 2]; got_probe = true; }
        for (int i = 0; i < 256; ++i)
            if (buf[i * 2] != 0) ++nonzero;
        if (!alive) break;
    }
    return nonzero;
}

int main() {
    // sample: 32000 frames, 4 regions of distinct DC-ish levels
    // region k = frames [k*8000, (k+1)*8000) filled with (k+1)*1000
    auto& s = synth::SampleBank::instance().slot(0);
    s.channels = 1;
    s.root_note = 60;
    s.data.assign(32000, 0);
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 8000; ++i) s.data[k * 8000 + i] = (int16_t)((k + 1) * 1000);

    // chops OUT OF ORDER with holes: array [2]=16000, [5]=0, [9]=24000, [12]=8000
    for (auto& c : s.chops) c = 0xFFFFFFFFu;
    s.chops[2]  = 16000;
    s.chops[5]  = 0;
    s.chops[9]  = 24000;
    s.chops[12] = 8000;
    // sorted: 0, 8000, 16000, 24000 -> slices 1..4 = regions 1..4

    // --- chromatic: note root+1 must play the SECOND sorted region (level 2000)
    {
        synth::Sampler v;
        v.params = synth::SamplerParams{};
        v.params.sample_slot = 0;
        v.params.chromatic_slices = true;
        v.params.release = 1;
        v.note_on(61, 127);
        int16_t first = 0;
        int nz = render_all(v, first);
        CHECK(first > 1500 && first < 2500, "chromatic note+1 starts in sorted region 2");
        CHECK(nz > 7000 && nz < 9000, "chromatic slice length ~= one region");
    }

    // --- fixed slice 3 = third sorted region (level 3000)
    {
        synth::Sampler v;
        v.params = synth::SamplerParams{};
        v.params.sample_slot = 0;
        v.params.slice = 3;
        v.params.release = 1;
        v.note_on(60, 127);
        int16_t first = 0;
        int nz = render_all(v, first);
        CHECK(first > 2500 && first < 3500, "fixed slice 3 starts in sorted region 3");
        CHECK(nz > 7000 && nz < 9000, "fixed slice length ~= one region");
    }

    // --- THRU: slice 2 plays from region 2 to the END (3 regions worth)
    {
        synth::Sampler v;
        v.params = synth::SamplerParams{};
        v.params.sample_slot = 0;
        v.params.slice = 2;
        v.params.play_mode = synth::PlayMode::Thru;
        v.params.release = 1;
        v.note_on(60, 127);
        int16_t first = 0;
        int nz = render_all(v, first);
        CHECK(first > 1500 && first < 2500, "THRU slice 2 starts in region 2");
        CHECK(nz > 22000, "THRU rings out to sample end (~24000 frames)");
    }

    // --- per-slice reverse: slice 2 reversed must start at the END of region 2
    {
        s.slice_rev_mask = (1u << 1);   // sorted rank 1 = slice 2
        synth::Sampler v;
        v.params = synth::SamplerParams{};
        v.params.sample_slot = 0;
        v.params.slice = 2;
        v.params.release = 1;
        v.note_on(60, 127);
        int16_t probe = 0;
        int nz = render_all(v, probe);
        // region 2 is uniform 2000, so the level alone can't prove direction;
        // check length is still one region and that it plays at all
        CHECK(probe > 1500 && probe < 2500, "rev slice 2 plays region 2");
        CHECK(nz > 7000 && nz < 9000, "rev slice 2 length ~= one region");
        s.slice_rev_mask = 0;
    }

    // --- per-slice reverse direction proof: ramp sample, reversed slice must
    //     start high and end low
    {
        auto& r = synth::SampleBank::instance().slot(1);
        r.channels = 1; r.root_note = 60;
        r.data.assign(8000, 0);
        for (int i = 0; i < 8000; ++i) r.data[i] = (int16_t)(i * 4);   // 0 -> ~32000 ramp
        for (auto& c : r.chops) c = 0xFFFFFFFFu;
        r.chops[0] = 0;
        r.slice_rev_mask = 1u;    // slice 1 reversed

        synth::Sampler v;
        v.params = synth::SamplerParams{};
        v.params.sample_slot = 1;
        v.params.slice = 1;
        v.params.release = 1;
        v.note_on(60, 127);
        std::vector<fx::q15> buf(256 * 2);
        v.render(buf.data(), 256);
        // reversed: first rendered frames come from the END of the ramp (high)
        CHECK(buf[200 * 2] > 20000, "reversed slice starts at the ramp's end (high)");
        r.slice_rev_mask = 0;
    }

    // --- beat sync: 2-bar sample synced to 1 bar plays back ~2x faster
    {
        auto& q = synth::SampleBank::instance().slot(2);
        q.channels = 1; q.root_note = 60;
        // 1 bar @120bpm @32k = 64000 frames. make the sample exactly 2 bars.
        const uint32_t bar = synth::bar_frames_at_bpm(120);
        q.data.assign(bar * 2, 1000);
        for (auto& c : q.chops) c = 0xFFFFFFFFu;

        synth::Sampler v;
        v.params = synth::SamplerParams{};
        v.params.sample_slot = 2;
        v.params.release = 1;
        v.params.sync_bars = 1;             // "make this fit 1 bar"
        v.params.bar_frames = bar;
        v.note_on(60, 127);
        int16_t probe = 0;
        int nz = render_all(v, probe);
        // 2 bars of audio squeezed into 1 bar of time = ~bar frames rendered
        int diff = nz - (int)bar;
        if (diff < 0) diff = -diff;
        CHECK(diff < (int)bar / 8, "sync 1bar renders ~1 bar from a 2-bar sample");

        // sync OFF renders the full 2 bars
        synth::Sampler v2;
        v2.params = v.params;
        v2.params.sync_bars = 0;
        v2.note_on(60, 127);
        int nz2 = render_all(v2, probe);
        CHECK(nz2 > nz * 3 / 2, "sync OFF renders much longer than sync 1bar");
    }

    std::printf(fails ? "\n%d FAILURES\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
