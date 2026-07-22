#include "mic_recorder.h"

namespace trackr::synth {

bool SampleRecorder::begin_recording(int slot, std::size_t max_frames) {
    if (recording_) return false;
    slot_ = slot;
    max_frames_ = max_frames;
    written_ = 0;

    // prepare the slot: clear and reserve
    auto& s = SampleBank::instance().slot(slot);
    s.data.clear();
    s.data.reserve(max_frames);
    s.channels    = 1;        // mic = mono
    s.root_note   = 60;
    s.loop_start  = 0;
    s.loop_end    = 0;
    s.reversed    = false;
    for (int i = 0; i < Sample::MAX_CHOPS; ++i) s.chops[i] = 0xFFFFFFFFu;

    int actual_sr = mic_.start(SAMPLER_SR);
    if (actual_sr == 0) return false;
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
        auto& s = SampleBank::instance().slot(slot_);
        const std::size_t src_n = s.data.size();
        const std::size_t dst_n =
            (std::size_t)((uint64_t)src_n * SAMPLER_SR / (uint32_t)actual_sr_);
        if (dst_n >= 2) {
            std::vector<fx::q15> out(dst_n);
            const uint64_t step_q32 = ((uint64_t)actual_sr_ << 32) / (uint32_t)SAMPLER_SR;
            uint64_t pos_q32 = 0;
            for (std::size_t i = 0; i < dst_n; ++i) {
                std::size_t idx = (std::size_t)(pos_q32 >> 32);
                uint32_t frac = (uint32_t)(pos_q32 & 0xFFFFFFFFu);
                fx::q15 a = s.data[idx];
                fx::q15 b = (idx + 1 < src_n) ? s.data[idx + 1] : a;
                out[i] = (fx::q15)(a + (fx::q15)(((int64_t)(b - a) * frac) >> 32));
                pos_q32 += step_q32;
            }
            s.data = std::move(out);
        }
    }
}

void SampleRecorder::tick() {
    if (!recording_) return;

    auto& s = SampleBank::instance().slot(slot_);
    fx::q15 buf[1024];

    while (true) {
        std::size_t avail = max_frames_ - written_;
        if (avail == 0) { stop_recording(); break; }
        std::size_t want = avail > 1024 ? 1024 : avail;
        std::size_t got = mic_.poll(buf, want);
        if (got == 0) break;
        s.data.insert(s.data.end(), buf, buf + got);
        written_ += got;
        if (written_ >= max_frames_) { stop_recording(); break; }
    }
}

float SampleRecorder::progress() const {
    if (max_frames_ == 0) return 0.f;
    return static_cast<float>(written_) / max_frames_;
}

} // namespace trackr::synth
