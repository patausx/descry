#include "core/synth/mic_recorder.h"
#include <cassert>
#include <cstdio>

using namespace trackr;

struct FakeMic final : synth::MicRecorder {
    bool on = false;
    int left = 32728;
    int start(int) override { on = true; return 32728; }
    void stop() override { on = false; }
    std::size_t poll(fx::q15* out, std::size_t max) override {
        if (!on || left <= 0) return 0;
        std::size_t n = max < (std::size_t)left ? max : (std::size_t)left;
        for (std::size_t i = 0; i < n; ++i) out[i] = (fx::q15)((left - (int)i) & 0x7fff);
        left -= (int)n;
        return n;
    }
    bool is_recording() const override { return on; }
};

int main() {
    FakeMic mic;
    synth::SampleRecorder rec(mic);
    assert(rec.begin_recording(7, 32728));
    rec.tick(); // auto-stops at cap and rate-corrects outside SampleBank
    assert(!rec.is_recording());
    assert(synth::SampleBank::instance().slot(7).empty());
    synth::Sample take; int slot = -1;
    assert(rec.take_completed(take, slot));
    assert(slot == 7);
    assert(take.channels == 1);
    assert(take.data.size() == 32000);
    assert(!rec.take_completed(take, slot));
    std::puts("mic staging smoke: ok");
}
