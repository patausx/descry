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
    recording_ = true;
    return true;
}

void SampleRecorder::stop_recording() {
    if (!recording_) return;
    mic_.stop();
    recording_ = false;
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
