#include "drumkit.h"

namespace trackr::synth {

void DrumKitVoice::note_on(int note, int velocity) {
    int pad = note - (int)params.base_note;
    if (pad < 0) pad = 0;
    if (pad >= DRUMKIT_PADS) pad = DRUMKIT_PADS - 1;

    // sliced break-kit: one source sample, pad N triggers sorted slice N.
    // zero PCM duplication, zero free slots required.
    if (params.sliced() && pad < DRUMKIT_PADS) {
        const uint8_t source = params.sliced_sample();
        auto& smp = SampleBank::instance().slot(source);
        int chop_count = 0;
        for (int i = 0; i < Sample::MAX_CHOPS; ++i)
            if (smp.chops[i] != 0xFFFFFFFFu) ++chop_count;
        if (smp.data.empty() || pad >= chop_count) { active_ = false; return; }
        inner_.params = SamplerParams{};
        inner_.params.sample_slot = source;
        inner_.params.slice = (uint8_t)(pad + 1);   // Sampler slice is 1-based
        inner_.params.start = 0;
        inner_.params.length = fx::Q15_ONE;
        inner_.params.release = 4000;
        inner_.note_on(smp.root_note, velocity);
        active_ = true;
        return;
    }

    uint8_t slot = params.slots[pad];
    if (slot == 0xFF || slot >= SAMPLE_BANK_SIZE) {
        active_ = false;
        return;
    }
    auto& smp = SampleBank::instance().slot(slot);
    if (smp.data.empty()) { active_ = false; return; }

    inner_.params = SamplerParams{};
    inner_.params.sample_slot = slot;
    inner_.params.start  = 0;
    inner_.params.length = fx::Q15_ONE;
    inner_.params.loop   = false;
    inner_.params.attack  = 0;
    inner_.params.release = 4000;

    // drum mode: on root_note -> no pitch-shift, clean hit
    inner_.note_on(smp.root_note, velocity);
    active_ = true;
}

void DrumKitVoice::note_off() {
    inner_.note_off();
}

bool DrumKitVoice::render(fx::q15* out, std::size_t frames) {
    if (!active_) {
        for (std::size_t i = 0; i < frames * 2; ++i) out[i] = 0;
        return false;
    }
    bool alive = inner_.render(out, frames);
    if (!alive) active_ = false;
    return alive;
}

} // namespace trackr::synth
