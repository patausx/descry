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
    uint8_t _pad = 0;                            // alignment
    DrumKitParams() {
        for (int i = 0; i < DRUMKIT_PADS; ++i) slots[i] = 0xFF;
    }
};

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
