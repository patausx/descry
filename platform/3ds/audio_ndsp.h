// 3ds audio output via ndsp (dsp chip, separate from arm11)
//
// ARCHITECTURE (v2 - no crackle):
//   ndsp_callback() (ISR level) -> LightEvent_Signal -> audio worker thread
//   audio worker (on core 1, priority above main) -> fill_buffers() -> render+submit
//   the main loop does NOT participate in audio. tick() is left as a no-op for compatibility.
//
// This eliminates underrun when a UI frame is blocked by vsync > 32ms.
#pragma once
#include "../../core/audio/mixer.h"
#include "../../core/sequencer/player.h"
#include <3ds.h>

namespace trackr::platform {

constexpr int AUDIO_SR        = 32000;
constexpr int FRAMES_PER_BUF  = 1024;       // ~32ms per buffer
constexpr int NUM_BUFFERS     = 3;          // 3x32ms = 96ms of headroom (plenty for the worker)

class Audio3DS {
public:
    bool init(audio::Mixer& mixer, seq::Player& player);
    void shutdown();

    // left for compatibility with the old main.cpp - now a no-op.
    void tick() {}

    // buffer starvation events since boot (all wave buffers DONE at once = the
    // dsp ran out of audio -> audible click). read from the UI for diagnosis.
    uint32_t underruns() const { return xrun_count_; }

    // called from the worker thread (via trampoline). DO NOT CALL directly.
    void worker_loop();

private:
    audio::Mixer*  mixer_  = nullptr;
    seq::Player*   player_ = nullptr;
    ndspWaveBuf    wave_bufs_[NUM_BUFFERS];
    int16_t*       buf_data_[NUM_BUFFERS] = {};
    int            next_buf_ = 0;

    // === threading ===
    LightEvent     wake_event_;          // signaled by the ndsp callback when a buffer is freed
    Thread         worker_handle_ = nullptr;
    RecursiveLock  audio_lock_;          // protects mixer/player from UI<->worker races
    aptHookCookie  apt_cookie_;          // hook for home/sleep/resume
    volatile bool  thread_run_  = false;
    bool           initialized_ = false;
    volatile uint32_t xrun_count_ = 0;   // starvation events (see underruns())
    volatile bool  primed_ = false;      // first buffer submitted (gates xrun counting)
    int stall_cycles_ = 0;               // watchdog: cycles with buffers queued but channel silent
};

} // namespace trackr::platform
