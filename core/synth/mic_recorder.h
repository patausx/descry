// abstract microphone recorder
// the platform implements start/stop/poll, core pulls samples and writes to SampleBank
#pragma once
#include "../audio/fixed.h"
#include "sampler.h"
#include <cstddef>

namespace trackr::synth {

class MicRecorder {
public:
    virtual ~MicRecorder() = default;

    // start recording at the given sample rate (the platform may adjust it)
    // returns the actual sr, or 0 if it failed
    virtual int start(int requested_sr) = 0;
    virtual void stop() = 0;

    // pull fresh samples from the platform buffer into our buffer
    // called from the main loop at some rate
    // returns how many samples were written
    virtual std::size_t poll(fx::q15* out, std::size_t max_frames) = 0;

    virtual bool is_recording() const = 0;
};

// high-level wrapper: writes into the chosen sample bank slot,
// automatically stops when max_frames is reached
class SampleRecorder {
public:
    SampleRecorder(MicRecorder& mic) : mic_(mic) {}

    // start recording into slot, up to max_frames (default ~5 seconds)
    bool begin_recording(int slot, std::size_t max_frames = 32000 * 5);
    void stop_recording();
    bool is_recording() const { return recording_; }

    // call every frame to pull fresh samples
    void tick();

    int   active_slot() const { return slot_; }
    float progress() const;   // 0..1

private:
    MicRecorder& mic_;
    bool recording_ = false;
    int  slot_ = -1;
    std::size_t max_frames_ = 0;
    std::size_t written_ = 0;
};

} // namespace trackr::synth
