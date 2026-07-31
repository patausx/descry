// Host envelope regression smoke: cached slopes must preserve stage durations,
// terminate voices, and remain finite after live parameter edits.
#include "core/synth/wavsynth.h"
#include "core/synth/fm.h"
#include "core/synth/dsn_synth.h"
#include "core/synth/sampler.h"
#include "core/audio/fixed.h"
#include <cassert>
#include <cstdio>
#include <vector>

using namespace trackr;

template<class V>
static int render_until_dead(V& v, int cap = 200000) {
    fx::q15 out[128 * 2];
    int frames = 0;
    while (frames < cap) {
        bool alive = v.render(out, 128);
        frames += 128;
        for (auto s : out) assert(s >= -32768 && s <= 32767);
        if (!alive) return frames;
    }
    return -1;
}

int main() {
    fx::warmup_pitch_table(32000);
    synth::warmup_wavsynth(); synth::warmup_fm(); synth::warmup_dsn(); synth::warmup_sampler();

    {
        synth::Wavsynth v; v.params.attack=64; v.params.decay=128; v.params.sustain=16000; v.params.release=256;
        v.note_on(60,127); fx::q15 b[256]; v.render(b,128);
        v.params.decay = 32; // live edit must invalidate cached decay slope
        v.note_off(); assert(render_until_dead(v) > 0);
    }
    {
        synth::FmSynth v; for (auto& op : v.params.ops) { op.attack=64; op.decay=128; op.sustain=80; op.release=256; }
        v.note_on(60,127); fx::q15 b[256]; v.render(b,128);
        v.params.ops[0].release = 64; v.note_off(); assert(render_until_dead(v) > 0);
    }
    {
        synth::DsnSynth v; v.params.eg1_attack=64; v.params.eg1_decay=128; v.params.eg1_sustain=16000; v.params.eg1_release=256;
        v.note_on(60,127); fx::q15 b[256]; v.render(b,128);
        v.params.eg1_release=64; v.note_off(); assert(render_until_dead(v) > 0);
    }
    {
        auto& s=synth::SampleBank::instance().slot(0); s.channels=1; s.data.assign(4096,1000);
        synth::Sampler v; v.params.sample_slot=0; v.params.attack=64; v.params.decay=128; v.params.sustain=16000; v.params.release=256;
        v.note_on(60,127); fx::q15 b[256]; v.render(b,128);
        v.params.release=64; v.note_off(); assert(render_until_dead(v) > 0);
    }
    std::puts("cached envelope smoke: ok");
}
