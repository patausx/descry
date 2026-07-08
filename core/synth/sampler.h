// sampler: plays back a chunk of q15 samples with pitch-shift and loops
// samples are stored in SampleBank, the voice references an index
//
// supports:
//   - mono or stereo (interleaved L/R)
//   - hermite cubic interpolation (4-point Catmull-Rom)
//   - full ADSR envelope (exp release)
//   - loop crossfade (256 frames)
//   - reverse playback
//   - fine-tune cents and per-voice gain
//   - exponential velocity curve
#pragma once
#include "../audio/voice.h"
#include <cstddef>
#include <vector>

namespace trackr::synth {

constexpr int SAMPLER_SR = 32000;

// one sample in the bank. q15 PCM, mono or stereo (interleaved L/R)
struct Sample {
    std::vector<fx::q15> data;      // mono: data[frame], stereo: data[frame*2+ch]
    uint8_t channels = 1;           // 1 = mono, 2 = stereo
    int root_note = 60;             // the note at which the sample sounds "normal"
    uint32_t loop_start = 0;        // in frames (not samples!)
    uint32_t loop_end   = 0;        // 0 = no loop
    bool reversed = false;          // not used directly - playback is controlled by SamplerParams::reverse

    // chop points - up to 16 markers in frames. 0xFFFFFFFF = empty
    static constexpr int MAX_CHOPS = 16;
    uint32_t chops[MAX_CHOPS] = {
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    };

    // helpers
    uint32_t num_frames() const {
        return channels ? (uint32_t)(data.size() / channels) : 0;
    }
    bool empty() const { return data.empty(); }
};

// sample bank - 64 slots, any of which may be empty
constexpr int SAMPLE_BANK_SIZE = 64;
class SampleBank {
public:
    Sample& slot(int i) { return samples_[i]; }
    const Sample& slot(int i) const { return samples_[i]; }
    static SampleBank& instance() { static SampleBank b; return b; }
private:
    Sample samples_[SAMPLE_BANK_SIZE];
};

// playback mode (M8-style). loop/reverse below are kept in sync with this
// in note_on, but play_mode is the source of truth for the engine.
enum class PlayMode : uint8_t {
    Fwd = 0,    // play once forward
    Rev,        // play once backward
    FwdLoop,    // loop forward
    RevLoop,    // loop backward
    Repitch,    // forward, slice stretched to note length (reserved; behaves as Fwd for now)
    Count
};

inline const char* play_mode_name(PlayMode m) {
    switch (m) {
        case PlayMode::Fwd:     return "FWD";
        case PlayMode::Rev:     return "REV";
        case PlayMode::FwdLoop: return "FWDLOOP";
        case PlayMode::RevLoop: return "REVLOOP";
        case PlayMode::Repitch: return "REPITCH";
        default:                return "?";
    }
}

struct SamplerParams {
    int sample_slot = 0;
    fx::q15 start  = 0;             // start position [0, Q15_ONE]
    fx::q15 length = fx::Q15_ONE;   // playback length
    bool loop = false;
    bool reverse = false;           // play backwards

    // M8-style play mode. source of truth; loop/reverse are derived in note_on.
    PlayMode play_mode = PlayMode::Fwd;
    // active slice: 0 = whole sample; 1..MAX_CHOPS = play chop[slice-1]..next.
    // when the instrument is set to "chromatic slices" the played note picks the
    // slice instead (handled in note_on via slice_from_note).
    uint8_t  slice = 0;
    bool     chromatic_slices = false;  // true: note (from root) selects the slice

    int8_t fine_cents = 0;          // -50..+50 cent offset
    fx::q15 gain = fx::Q15_ONE;     // per-voice trim

    // full ADSR (like wavsynth)
    uint32_t attack  = 0;           // in frames
    uint32_t decay   = 0;           // in frames
    fx::q15  sustain = fx::Q15_ONE; // sustain level (q15)
    uint32_t release = 4000;        // in frames
};

class Sampler : public audio::Voice {
public:
    void note_on(int note, int velocity) override;
    void note_off() override;
    bool render(fx::q15* out, std::size_t frames) override;

    int current_frame() const override { return (int)pos_hi_; }
    int current_sample_slot() const override { return params.sample_slot; }

    int     ui_env_stage(int) const override { return (int)stage_; }
    fx::q15 ui_env_level(int) const override { return (fx::q15)(env_ >> 16); }

    SamplerParams params;

private:
    // position in the source sample
    fx::uq16 pos_lo_  = 0;          // fractional part of the position (q16)
    int64_t  pos_hi_  = 0;          // integer part (frame index, signed for reverse)
    int32_t  pos_inc_ = 1 << 16;    // speed in q16.16, signed (negative for reverse)

    fx::q15  velocity_ = fx::Q15_ONE;
    bool     gated_ = false;

    // ADSR state
    enum class Stage : uint8_t { Idle, Attack, Decay, Sustain, Release };
    Stage   stage_ = Stage::Idle;
    fx::q31 env_   = 0;             // q31 amplitude envelope for smoothness
    uint32_t stage_pos_ = 0;
    fx::q31 release_start_env_ = 0; // remember the env at the moment of transition into release

    // mirrors of the bounds (computed in note_on so render doesn't recompute every sample)
    uint32_t play_start_  = 0;      // start frame
    uint32_t play_end_    = 0;      // end frame (exclusive)
    bool     loop_active_ = false;
    uint32_t loop_start_  = 0;
    uint32_t loop_end_    = 0;
};

} // namespace trackr::synth
