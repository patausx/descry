// abstract voice - anything capable of spitting out samples
#pragma once
#include "fixed.h"
#include <cstddef>

namespace trackr::audio {

class Voice {
public:
    virtual ~Voice() = default;

    // start a note (midi 0-127, velocity 0-127)
    virtual void note_on(int note, int velocity) = 0;
    virtual void note_off() = 0;

    // hard-cut: silence the voice as fast as possible (KIL command). default falls
    // back to note_off (soft release); synths with an envelope override for a real cut.
    virtual void cut() { note_off(); }

    // render `frames` stereo samples into out (interleaved L/R q15)
    // returns true if the voice is still active
    virtual bool render(fx::q15* out, std::size_t frames) = 0;

    bool active() const { return active_; }

    // for the UI playhead: returns the current position in frames (-1 if not applicable)
    virtual int current_frame() const { return -1; }
    // for sampler - the sample slot being played (-1 = not a sampler)
    virtual int current_sample_slot() const { return -1; }

    // steal priority for the poly pool: LOWER = better steal candidate.
    // default INT32_MAX = "held / loudness unknown - steal only by age".
    // synths with envelopes override: released voices report their current
    // envelope so the pool steals the quietest dying tail instead of a loud one.
    virtual fx::q31 steal_weight() const { return 0x7FFFFFFF; }

protected:
    bool active_ = false;
};

} // namespace trackr::audio
