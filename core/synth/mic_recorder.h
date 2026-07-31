// abstract microphone recorder
// the platform implements start/stop/poll, core pulls samples and writes to SampleBank
#pragma once
#include "../audio/fixed.h"
#include "sampler.h"
#include <cstddef>
#include <utility>

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

    // Record into a private staging Sample. The live SampleBank slot is not
    // touched until the caller publishes the completed take under the audio
    // lock, so capture/resampling never blocks the realtime worker.
    bool begin_recording(int slot, std::size_t max_frames = 32000 * 5);
    void stop_recording();
    bool is_recording() const { return recording_; }

    // call every frame to pull fresh samples; auto-stop may complete a take
    void tick();

    int   active_slot() const { return slot_; }
    float progress() const;   // 0..1
    const Sample& preview_sample() const { return staging_; }

    // Move a finished take out for a short swap into SampleBank. Returns false
    // until stop_recording (manual or automatic) has completed rate correction.
    bool take_completed(Sample& out, int& slot);

private:
    MicRecorder& mic_;
    bool recording_ = false;
    int  slot_ = -1;
    std::size_t max_frames_ = 0;
    std::size_t written_ = 0;
    int actual_sr_ = SAMPLER_SR;   // what the mic really delivers (3ds: 32728)
    Sample staging_;
    bool completed_ = false;
};

} // namespace trackr::synth
