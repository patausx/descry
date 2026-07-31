// Host smoke for sample-window -> persistent wavetable capture.
#include "core/synth/wavetable.h"
#include "core/synth/sampler.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace trackr;

int main() {
    synth::Sample s;
    s.channels = 2;
    s.data.resize(4096 * 2);
    for (int i = 0; i < 4096; ++i) {
        int16_t v = (int16_t)(12000 + std::sin(i * 2.0 * 3.14159265358979 / 128.0) * 8000);
        s.data[i * 2] = v;
        s.data[i * 2 + 1] = (int16_t)(v / 2);
    }
    fx::q15 cycle[synth::WavetableBank::SIZE];
    assert(synth::WavetableBank::prepare_capture(s, 512, 2048, cycle));
    long long sum = 0; int peak = 0;
    for (auto v : cycle) { sum += v; int a = std::abs((int)v); if (a > peak) peak = a; }
    assert(std::llabs(sum) < 2048); // DC removed to rounding noise
    assert(peak > 30000 && peak <= 32767);

    auto& bank = synth::WavetableBank::instance();
    int slot = bank.install_capture(cycle, "hostcap");
    assert(slot == synth::WavetableBank::FILE_SLOTS);
    assert(bank.occupied(slot));
    assert(bank.index_of_slot(slot) == 0);
    assert(bank.slot_at(0) == slot);
    assert(bank.sample(slot, 12345) != 0);

    std::system("mkdir -p /tmp/descry-wt-test");
    assert(bank.save_captures("/tmp/descry-wt-test"));
    std::puts("wavetable capture smoke: ok");
}
