// Schroeder reverb - 4 comb + 2 allpass, stereo
#pragma once
#include "fixed.h"
#include <cstddef>
#include <cstring>

namespace trackr::audio {

// 1 comb filter with damping
template<int DELAY_LEN>
class Comb {
public:
    fx::q15 process(fx::q15 in) {
        fx::q15 out = buf_[pos_];
        // damping: lowpass on the feedback
        // IMPORTANT: the sum of two q15 values can exceed int16 - saturate to avoid wrap.
        // this is exactly where it crackled on long reverb tails (bass/low end)
        int32_t damp_mix = (int32_t)fx::mul_q15(out, fx::Q15_ONE - damp_) +
                           (int32_t)fx::mul_q15(damp_state_, damp_);
        damp_state_ = fx::sat_q15(damp_mix);
        int32_t fb = fx::mul_q15(damp_state_, feedback_);
        // SOFT clip the buffer write. hard sat_q15 inside a feedback loop
        // re-clips every cycle once the tail energy reaches the q15 ceiling -
        // sustained harsh distortion ("farting") on dense reverb-heavy material.
        // the cubic knee saturates gently and the loop stays musical.
        buf_[pos_] = fx::soft_clip_q31((fx::q31)in + fb);
        pos_ = (pos_ + 1) % DELAY_LEN;
        return out;
    }
    void reset() { std::memset(buf_, 0, sizeof(buf_)); pos_ = 0; damp_state_ = 0; }
    fx::q15 feedback_ = (fx::q15)(fx::Q15_ONE * 65 / 100);   // 0.78 -> 0.65: buf_max = in/(1-fb) ~= 2.86x instead of 4.5x
    fx::q15 damp_     = (fx::q15)(fx::Q15_ONE * 30 / 100);
private:
    fx::q15 buf_[DELAY_LEN] = {0};
    int     pos_ = 0;
    fx::q15 damp_state_ = 0;
};

// allpass filter
template<int DELAY_LEN>
class Allpass {
public:
    fx::q15 process(fx::q15 in) {
        fx::q15 buf_out = buf_[pos_];
        int32_t y = -in + buf_out;
        int32_t fb_in = (fx::q31)in + fx::mul_q15(buf_out, fb_);
        buf_[pos_] = fx::soft_clip_q31(fb_in);   // same soft knee as the combs
        pos_ = (pos_ + 1) % DELAY_LEN;
        return fx::sat_q15(y);
    }
    void reset() { std::memset(buf_, 0, sizeof(buf_)); pos_ = 0; }
    fx::q15 fb_ = (fx::q15)(fx::Q15_ONE * 50 / 100);
private:
    fx::q15 buf_[DELAY_LEN] = {0};
    int     pos_ = 0;
};

// Schroeder reverb: 4 comb in parallel -> 2 allpass in series
class Reverb {
public:
    void process(fx::q15 in_l, fx::q15 in_r, fx::q15& out_l, fx::q15& out_r) {
        int32_t sum_l = 0, sum_r = 0;
        sum_l += c1l_.process(in_l);
        sum_l += c2l_.process(in_l);
        sum_l += c3l_.process(in_l);
        sum_l += c4l_.process(in_l);
        sum_r += c1r_.process(in_r);
        sum_r += c2r_.process(in_r);
        sum_r += c3r_.process(in_r);
        sum_r += c4r_.process(in_r);
        fx::q15 ml = (fx::q15)(sum_l / 4);
        fx::q15 mr = (fx::q15)(sum_r / 4);
        ml = a1l_.process(ml);
        ml = a2l_.process(ml);
        mr = a1r_.process(mr);
        mr = a2r_.process(mr);
        out_l = ml;
        out_r = mr;
    }
    void reset() {
        c1l_.reset(); c2l_.reset(); c3l_.reset(); c4l_.reset();
        c1r_.reset(); c2r_.reset(); c3r_.reset(); c4r_.reset();
        a1l_.reset(); a2l_.reset();
        a1r_.reset(); a2r_.reset();
    }

    void set_room_size(fx::q15 q) {
        c1l_.feedback_ = q; c2l_.feedback_ = q; c3l_.feedback_ = q; c4l_.feedback_ = q;
        c1r_.feedback_ = q; c2r_.feedback_ = q; c3r_.feedback_ = q; c4r_.feedback_ = q;
    }
    void set_damping(fx::q15 q) {
        c1l_.damp_ = q; c2l_.damp_ = q; c3l_.damp_ = q; c4l_.damp_ = q;
        c1r_.damp_ = q; c2r_.damp_ = q; c3r_.damp_ = q; c4r_.damp_ = q;
    }

private:
    // 32kHz comb delay times (Schroeder masgn at 44.1kHz scaled): 810/862/927/984
    Comb<810> c1l_;
    Comb<862> c2l_;
    Comb<927> c3l_;
    Comb<984> c4l_;
    // R is slightly different for stereo (+23 samples)
    Comb<833> c1r_;
    Comb<885> c2r_;
    Comb<950> c3r_;
    Comb<1007> c4r_;
    // allpass: 404 / 320 (32kHz)
    Allpass<404> a1l_;
    Allpass<320> a2l_;
    Allpass<427> a1r_;
    Allpass<343> a2r_;
};

} // namespace trackr::audio
