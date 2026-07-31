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

// Build sampler pitch tables outside the first-note realtime path.
void warmup_sampler();

// one sample in the bank. q15 PCM, mono or stereo (interleaved L/R)
struct Sample {
    static constexpr int NAME_LEN = 32;

    std::vector<fx::q15> data;      // mono: data[frame], stereo: data[frame*2+ch]
    char name[NAME_LEN] = {0};      // display name; persisted beside sample_XX.s16
    uint8_t channels = 1;           // 1 = mono, 2 = stereo
    int root_note = 60;             // the note at which the sample sounds "normal"
    uint32_t loop_start = 0;        // in frames (not samples!)
    uint32_t loop_end   = 0;        // 0 = no loop
    bool reversed = false;          // not used directly - playback is controlled by SamplerParams::reverse

    // chop points - up to 32 markers in frames. 0xFFFFFFFF = empty.
    // 32 (was 16): a 2-bar amen at 16ths is 32 hits - the whole point of a
    // break machine is being able to address every hit.
    static constexpr int MAX_CHOPS = 32;
    uint32_t chops[MAX_CHOPS];

    // per-slice reverse: bit k = the k-th slice in SORTED order plays backwards.
    // lives here (not in SamplerParams) because it belongs to the slicing of
    // this audio, like chops - and so it survives in the .s16 file.
    // reversed snare/ride = instant jungle classic.
    uint32_t slice_rev_mask = 0;

    Sample() { for (auto& c : chops) c = 0xFFFFFFFFu; }

    // helpers
    uint32_t num_frames() const {
        return channels ? (uint32_t)(data.size() / channels) : 0;
    }
    bool empty() const { return data.empty(); }
};

// sample bank - 64 slots, any of which may be empty
constexpr int SAMPLE_BANK_SIZE = 64;
// Conservative aggregate PCM budget. Samples live in normal heap RAM alongside UI,
// projects and temporary decode buffers; leave New3DS extended memory ample margin.
constexpr std::size_t SAMPLE_RAM_BUDGET_BYTES = 32u * 1024u * 1024u;

class SampleBank {
public:
    Sample& slot(int i) { return samples_[i]; }
    const Sample& slot(int i) const { return samples_[i]; }
    static SampleBank& instance() { static SampleBank b; return b; }

    std::size_t bytes_used() const {
        std::size_t total = 0;
        for (const auto& s : samples_) total += s.data.capacity() * sizeof(fx::q15);
        return total;
    }
    std::size_t bytes_available_replacing(int slot_index) const {
        std::size_t used = bytes_used();
        if (slot_index >= 0 && slot_index < SAMPLE_BANK_SIZE)
            used -= samples_[slot_index].data.capacity() * sizeof(fx::q15);
        return used < SAMPLE_RAM_BUDGET_BYTES ? SAMPLE_RAM_BUDGET_BYTES - used : 0;
    }
    bool can_replace_bytes(int slot_index, std::size_t bytes) const {
        return bytes <= bytes_available_replacing(slot_index);
    }
    bool can_replace(int slot_index, const Sample& candidate) const {
        return can_replace_bytes(slot_index, candidate.data.capacity() * sizeof(fx::q15));
    }
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
    Thru,       // amigo-style: slice STARTS at its marker but plays through to the
                // end of the window instead of cutting at the next marker. lets a
                // ride/crash slice ring out over the rest of the break.
    Count
};

inline const char* play_mode_name(PlayMode m) {
    switch (m) {
        case PlayMode::Fwd:     return "FWD";
        case PlayMode::Rev:     return "REV";
        case PlayMode::FwdLoop: return "FWDLOOP";
        case PlayMode::RevLoop: return "REVLOOP";
        case PlayMode::Repitch: return "REPITCH";
        case PlayMode::Thru:    return "THRU";
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

    // === beat sync (akai/amiga style REPITCH, not time-stretch) ===
    // sync_bars: 0 = off. >0 = "this sample is N bars long" -> playback rate is
    // scaled so it lasts exactly N bars at the project tempo. a 174bpm break
    // dropped into a 140bpm project just fits, pitched down like a real sampler.
    uint8_t  sync_bars = 0;
    // frames per bar at the CURRENT project tempo. the voice has no access to
    // Song, so Project::make_voice/refresh_voice_params fills this in.
    uint32_t bar_frames = 0;

    int8_t fine_cents = 0;          // -50..+50 cent offset
    fx::q15 gain = fx::Q15_ONE;     // per-voice trim

    // full ADSR (like wavsynth)
    uint32_t attack  = 0;           // in frames
    uint32_t decay   = 0;           // in frames
    fx::q15  sustain = fx::Q15_ONE; // sustain level (q15)
    uint32_t release = 4000;        // in frames
};

// frames per bar (4 beats) at a given bpm. used to fill SamplerParams::bar_frames.
inline uint32_t bar_frames_at_bpm(int bpm) {
    if (bpm < 20) bpm = 20;
    return (uint32_t)((uint64_t)SAMPLER_SR * 240 / (uint32_t)bpm);
}

class Sampler : public audio::Voice {
public:
    void note_on(int note, int velocity) override;
    void note_off() override;
    bool render(fx::q15* out, std::size_t frames) override;

    // LOAD-browser audition uses an App-owned transient Sample instead of
    // destroying a real SampleBank slot. The caller must cut this preview voice
    // before replacing/freeing that Sample.
    void set_external_sample(const Sample* s) { external_sample_ = s; }

    int current_frame() const override { return (int)pos_hi_; }
    int current_sample_slot() const override { return external_sample_ ? -2 : params.sample_slot; }

    int     ui_env_stage(int) const override { return (int)stage_; }
    fx::q15 ui_env_level(int) const override { return (fx::q15)(env_ >> 16); }

    SamplerParams params;

private:
    const Sample* source_sample() const {
        return external_sample_ ? external_sample_ : &SampleBank::instance().slot(params.sample_slot);
    }

    const Sample* external_sample_ = nullptr;

    // position in the source sample
    fx::uq16 pos_lo_  = 0;          // fractional part of the position (q16)
    int64_t  pos_hi_  = 0;          // integer part (frame index, signed for reverse)
    int32_t  pos_inc_ = 1 << 16;    // speed in q16.16, signed (negative for reverse)

    fx::q15  velocity_ = fx::Q15_ONE;
    bool     gated_ = false;

    // ADSR state
    enum class Stage : uint8_t { Idle, Attack, Decay, Sustain, Release };
    Stage   stage_ = Stage::Idle;
    Stage   rate_stage_ = Stage::Idle;
    uint32_t rate_duration_ = 0xFFFFFFFFu;
    int32_t rate_sustain_ = -1;
    fx::q31 env_step_ = 1;
    fx::q31 env_target_ = 0;
    void refresh_env_rate();
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
