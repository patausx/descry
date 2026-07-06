#include "audio_ndsp.h"
#include <cstring>
#include <malloc.h>

namespace trackr::platform {

// === static thunk for the ndsp callback ===
// ndsp triggers it on a dsp-event (system thread), we mustn't do anything heavy in it -
// only signal the event that our worker wakes up on.
static LightEvent* g_wake_event_for_callback = nullptr;
static Audio3DS*   g_audio_for_apt_hook      = nullptr;
static void ndsp_callback(void* /*arg*/) {
    if (g_wake_event_for_callback) {
        LightEvent_Signal(g_wake_event_for_callback);
    }
}

// === APT hook: catch HOME/SLEEP/RESUME ===
// on exit from suspend/sleep ndsp sometimes either skips the callback,
// or leaves the channel paused. explicitly wake the worker + clear the pause.
//
// ALSO: core1 cpu-time limit. we run at 80% for the synth - but while descry
// sits in the background (HOME menu) that reservation starves the system core
// and the HOME menu lags hard. so: give the time back on suspend, reclaim on resume.
static void apt_hook_cb(APT_HookType hook, void* /*param*/) {
    if (!g_audio_for_apt_hook) return;
    switch (hook) {
        case APTHOOK_ONRESTORE:
        case APTHOOK_ONWAKEUP:
            // reclaim core1 time for the synth
            APT_SetAppCpuTimeLimit(80);
            // clear pause if the system set it
            ndspChnSetPaused(0, false);
            // wake the worker - let it re-check the buffers and get back into rhythm
            if (g_wake_event_for_callback) LightEvent_Signal(g_wake_event_for_callback);
            break;
        case APTHOOK_ONSUSPEND:
        case APTHOOK_ONSLEEP:
            // explicitly set pause - otherwise ndsp may play into the void and break the buffers
            ndspChnSetPaused(0, true);
            // hand core1 back to the system - audio is paused anyway, and the
            // HOME menu needs the syscore to stay responsive
            APT_SetAppCpuTimeLimit(10);
            break;
        default: break;
    }
}

// === thunk for threadCreate ===
static void worker_trampoline(void* arg) {
    static_cast<Audio3DS*>(arg)->worker_loop();
}

bool Audio3DS::init(audio::Mixer& mixer, seq::Player& player) {
    mixer_  = &mixer;
    player_ = &player;

    if (R_FAILED(ndspInit())) return false;

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    // POLYPHASE = sinc-based SRC, cleaner than LINEAR at 32000 -> 32728 (native NDSP rate)
    ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(0, AUDIO_SR);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);

    float mix[12] = {0};
    mix[0] = 1.0f;  // front L
    mix[1] = 1.0f;  // front R
    ndspChnSetMix(0, mix);

    // allocate audio buffers in linear memory (dsp requirement)
    const std::size_t bytes = FRAMES_PER_BUF * 2 * sizeof(int16_t);
    for (int i = 0; i < NUM_BUFFERS; ++i) {
        buf_data_[i] = static_cast<int16_t*>(linearAlloc(bytes));
        if (!buf_data_[i]) return false;
        std::memset(buf_data_[i], 0, bytes);
        std::memset(&wave_bufs_[i], 0, sizeof(ndspWaveBuf));
        wave_bufs_[i].data_vaddr = buf_data_[i];
        wave_bufs_[i].nsamples   = FRAMES_PER_BUF;
        wave_bufs_[i].status     = NDSP_WBUF_DONE;
    }

    // === threading setup ===
    LightEvent_Init(&wake_event_, RESET_ONESHOT);
    g_wake_event_for_callback = &wake_event_;
    g_audio_for_apt_hook      = this;
    ndspSetCallback(ndsp_callback, nullptr);
    aptHook(&apt_cookie_, apt_hook_cb, this);

    RecursiveLock_Init(&audio_lock_);
    mixer_->set_audio_lock(&audio_lock_);

    // worker priority: 1 above main (main is usually 0x30 in homebrew)
    s32 main_prio = 0x30;
    svcGetThreadPriority(&main_prio, CUR_THREAD_HANDLE);
    s32 worker_prio = main_prio - 1;
    if (worker_prio < 0x18) worker_prio = 0x18;  // don't intrude into system priorities

    // core 1 cpu-time limit: raised 30 -> 80. MOONLIGHT-class material (12 poly
    // FM voices + reverb) was hitting the 30% ceiling -> underruns = "farting"
    // ~20s in when the melody track piled voices on. 80% is safe: main loop
    // (UI) lives on core 0, core 1 is ours to burn.
    APT_SetAppCpuTimeLimit(80);

    thread_run_ = true;
    worker_handle_ = threadCreate(worker_trampoline, this,
                                   0x4000,        // stack 16KB
                                   worker_prio,
                                   1,             // core 1 (system-shared)
                                   false);        // not detached → joinable

    if (!worker_handle_) {
        // fallback: try on core 0 (Old 3DS without cpu-time-limit preconfigured)
        worker_handle_ = threadCreate(worker_trampoline, this,
                                       0x4000,
                                       worker_prio,
                                       -1,        // -1 = default (core 0)
                                       false);
    }

    if (!worker_handle_) {
        // couldn't bring up the worker - we don't fall back to "tick from main loop",
        // better to explicitly return false so main can fail with a clear error
        thread_run_ = false;
        return false;
    }

    initialized_ = true;
    return true;
}

void Audio3DS::shutdown() {
    if (!initialized_) return;

    // 1) signal the worker to exit
    thread_run_ = false;
    LightEvent_Signal(&wake_event_);

    // 2) wait
    if (worker_handle_) {
        threadJoin(worker_handle_, U64_MAX);
        threadFree(worker_handle_);
        worker_handle_ = nullptr;
    }

    // 3) kill the apt hook and callback BEFORE ndspExit so we don't catch a stray signal
    aptUnhook(&apt_cookie_);
    ndspSetCallback(nullptr, nullptr);
    g_wake_event_for_callback = nullptr;
    g_audio_for_apt_hook      = nullptr;

    // hand the core1 time back to the system - the limit outlives the process
    // (hbmenu inherits it and the HOME menu lags after we exit)
    APT_SetAppCpuTimeLimit(10);

    // 4) unbind the lock from the mixer before anyone else tries to touch it
    if (mixer_) mixer_->set_audio_lock(nullptr);

    ndspChnReset(0);
    for (int i = 0; i < NUM_BUFFERS; ++i) {
        if (buf_data_[i]) linearFree(buf_data_[i]);
        buf_data_[i] = nullptr;
    }
    ndspExit();
    initialized_ = false;
}

void Audio3DS::worker_loop() {
    // 100ms watchdog timeout: even if the ndsp callback is lost (edge case after APT events
    // or a dsp-glitch), we wake up and check all buffers ourselves. a guarantee against full silence.
    constexpr s64 WATCHDOG_NS = 100'000'000LL;  // 100ms

    while (thread_run_) {
        // wait for a signal from ndsp / the apt hook / shutdown - but no longer than 100ms
        LightEvent_WaitTimeout(&wake_event_, WATCHDOG_NS);
        if (!thread_run_) break;

        // starvation check: if EVERY buffer is already DONE, the dsp consumed all
        // queued audio before we refilled - that gap is an audible click. count it
        // so the scope debug footer can show real on-device numbers.
        // primed_: at init ALL buffers start as DONE by construction - counting
        // that would bake a false "XRUN 1" into every session.
        if (primed_) {
            int done_count = 0;
            for (int n = 0; n < NUM_BUFFERS; ++n)
                if (wave_bufs_[n].status == NDSP_WBUF_DONE) ++done_count;
            if (done_count == NUM_BUFFERS) ++xrun_count_;
        }

        // go over ALL buffers (not just next_buf_) - after an APT-resume
        // it can happen that several buffers are DONE at once in different places.
        for (int n = 0; n < NUM_BUFFERS; ++n) {
            int idx = (next_buf_ + n) % NUM_BUFFERS;
            auto& wb = wave_bufs_[idx];
            if (wb.status != NDSP_WBUF_DONE) continue;

            // === critical section: sequencer + render ===
            // RecursiveLock - the UI may be doing replace_voice() at this time and it will wait.
            {
                audio::Mixer::LockGuard _g(*mixer_);
                if (player_) player_->advance(FRAMES_PER_BUF, AUDIO_SR);
                mixer_->render(reinterpret_cast<fx::q15*>(buf_data_[idx]),
                               FRAMES_PER_BUF);
            }

            DSP_FlushDataCache(buf_data_[idx],
                              FRAMES_PER_BUF * 2 * sizeof(int16_t));
            ndspChnWaveBufAdd(0, &wb);
            primed_ = true;   // pipeline live - starvation counting is meaningful now
            next_buf_ = (idx + 1) % NUM_BUFFERS;
        }
    }
}

} // namespace trackr::platform
