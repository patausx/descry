#include "mic_recorder.h"
#include <cstdio>

namespace trackr::synth {

bool SampleRecorder::begin_recording(int slot, std::size_t max_frames) {
    if (recording_) return false;
    slot_ = slot;
    max_frames_ = max_frames;
    written_ = 0;
    completed_ = false;

    // Prepare private storage, never the live bank slot. reserve/clear may call
    // the heap and therefore must stay outside the global audio lock.
    staging_ = Sample{};
    std::snprintf(staging_.name, sizeof(staging_.name), "mic rec %02d", slot);
    staging_.data.reserve(max_frames);
    staging_.channels    = 1;        // mic = mono
    staging_.root_note   = 60;
    staging_.loop_start  = 0;
    staging_.loop_end    = 0;
    staging_.reversed    = false;

    int actual_sr = mic_.start(SAMPLER_SR);
    if (actual_sr == 0) { slot_ = -1; return false; }
    actual_sr_ = actual_sr;
    recording_ = true;
    return true;
}

void SampleRecorder::stop_recording() {
    if (!recording_) return;
    mic_.stop();
    recording_ = false;

    // rate-correct: the 3ds mic really samples at 32728 Hz but the engine
    // plays everything back at 32000 - untouched, every recording came out
    // ~2.3% (39 cents) flat and slow. linear resample actual_sr -> 32000.
    // (mono by construction, so frames == samples.)
    if (actual_sr_ != SAMPLER_SR && written_ > 1) {
        const std::size_t src_n = staging_.data.size();
        const std::size_t dst_n =
            (std::size_t)((uint64_t)src_n * SAMPLER_SR / (uint32_t)actual_sr_);
        if (dst_n >= 2) {
            std::vector<fx::q15> out(dst_n);
            const uint64_t step_q32 = ((uint64_t)actual_sr_ << 32) / (uint32_t)SAMPLER_SR;
            uint64_t pos_q32 = 0;
            for (std::size_t i = 0; i < dst_n; ++i) {
                std::size_t idx = (std::size_t)(pos_q32 >> 32);
                uint32_t frac = (uint32_t)(pos_q32 & 0xFFFFFFFFu);
                fx::q15 a = staging_.data[idx];
                fx::q15 b = (idx + 1 < src_n) ? staging_.data[idx + 1] : a;
                out[i] = (fx::q15)(a + (fx::q15)(((int64_t)(b - a) * frac) >> 32));
                pos_q32 += step_q32;
            }
            staging_.data = std::move(out);
            written_ = staging_.data.size();
        }
    }
    completed_ = true;
}

bool SampleRecorder::take_completed(Sample& out, int& slot) {
    if (!completed_) return false;
    out = std::move(staging_);
    slot = slot_;
    completed_ = false;
    slot_ = -1;
    return true;
}

void SampleRecorder::tick() {
    if (!recording_) return;

    fx::q15 buf[1024];

    while (true) {
        std::size_t avail = max_frames_ - written_;
        if (avail == 0) { stop_recording(); break; }
        std::size_t want = avail > 1024 ? 1024 : avail;
        std::size_t got = mic_.poll(buf, want);
        if (got == 0) break;
        staging_.data.insert(staging_.data.end(), buf, buf + got);
        written_ += got;
        if (written_ >= max_frames_) { stop_recording(); break; }
    }
}

float SampleRecorder::progress() const {
    if (max_frames_ == 0) return 0.f;
    return static_cast<float>(written_) / max_frames_;
}

} // namespace trackr::synth
