// drum kit: one instrument = 16 pads (note -> sample slot)
// midi note N → pad index = N - base_note (clamp 0..15)
// each pad plays its own sample at its native root_note (no pitch shift)
#pragma once
#include "../audio/voice.h"
#include "sampler.h"
#include <cstdint>

namespace trackr::synth {

constexpr int DRUMKIT_PADS = 16;

struct DrumKitParams {
    uint8_t base_note = 60;                     // C-4 = pad 0
    uint8_t slots[DRUMKIT_PADS];                // sample slot per pad, EMPTY=empty

    // zero-copy break-kit mode, packed into the old alignment byte so the
    // serialized struct stays EXACTLY 18 bytes and legacy projects remain valid:
    //   0 = normal independent-slot kit (all old projects)
    //   1..64 = source SampleBank slot + 1; pad N triggers sorted slice N.
    uint8_t sliced_sample_enc = 0;

    DrumKitParams() {
        for (int i = 0; i < DRUMKIT_PADS; ++i) slots[i] = 0xFF;
    }
    bool sliced() const { return sliced_sample_enc >= 1 && sliced_sample_enc <= SAMPLE_BANK_SIZE; }
    uint8_t sliced_sample() const { return sliced() ? (uint8_t)(sliced_sample_enc - 1) : 0xFF; }
    void set_sliced_sample(int slot) {
        sliced_sample_enc = (slot >= 0 && slot < SAMPLE_BANK_SIZE) ? (uint8_t)(slot + 1) : 0;
    }
};
static_assert(sizeof(DrumKitParams) == 18, "DrumKitParams layout must stay project-compatible");

class DrumKitVoice : public audio::Voice {
public:
    void note_on(int note, int velocity) override;
    void note_off() override;
    bool render(fx::q15* out, std::size_t frames) override;

    int current_frame() const override { return inner_.current_frame(); }
    int current_sample_slot() const override { return inner_.current_sample_slot(); }

    DrumKitParams params;

private:
    Sampler inner_;
};

} // namespace trackr::synth
