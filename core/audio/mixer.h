// 8-channel mixer with per-track polyphony (4 voice pool)
// takes pointers to voices, sums into a q31 accumulator, saturates to q15
#pragma once
#include "voice.h"
#include "reverb.h"
#include "../dsp/filter.h"
#include "../dsp/lfo.h"
#include <array>
#include <vector>

namespace trackr::audio {

constexpr int NUM_TRACKS = 8;
constexpr int TRACK_POLY = 4;       // max simultaneous notes per track

struct TrackState {
    // pool of voices - all rendered and summed
    Voice*   voices[TRACK_POLY] = {nullptr};
    uint32_t voice_age[TRACK_POLY] = {0};   // for voice stealing (higher = newer)

    fx::q15 volume = fx::Q15_ONE;
    fx::q15 pan    = 0;
    bool muted = false;
    // persistent mixer fader (mixer view). separate from `volume` because volume
    // is FX/instrument-driven and resets on every note trigger; mix_vol is the
    // user's channel fader, applied multiplicatively on top.
    fx::q15 mix_vol = fx::Q15_ONE;
    // output peak meter (post-DSP, post-fader). written by render(), decays there;
    // UI just reads it. q15.
    fx::q15 meter = 0;

    // === per-track DSP fx ===
    fx::q15 cutoff    = fx::Q15_ONE;  // 1.0 = open (no filter)
    fx::q15 resonance = 0;            // resonance Q (0..ONE, usable in Svf2; high = self-osc)
    dsp::FilterType filter_type = dsp::FilterType::LPF;
    uint8_t bits      = 16;           // 16 = clean
    uint8_t downsample = 1;           // 1 = no decimation
    // m8-style separate sends: delay and reverb each get their own bus level
    fx::q15 send_del  = 0;            // to delay bus (0 = dry only)
    fx::q15 send_rev  = 0;            // to reverb bus (0 = dry only)
    // sidechain duck depth: how hard THIS track dips when the duck source
    // track fires (0 = unaffected, Q15_ONE = full mute at envelope peak)
    fx::q15 duck_amt  = 0;

    // === performance hold (kaoss pad / analog sticks) ===
    // while a gesture owns a parameter, note triggers must NOT stomp it with
    // the instrument's FX defaults (apply_inst_fx_defaults checks these bits).
    // set by apply_kaoss_dest / stick writes, cleared when the release ramp
    // finishes and on player stop.
    enum PerfHold : uint16_t {
        HOLD_CUT = 1 << 0, HOLD_RES = 1 << 1, HOLD_DEL = 1 << 2,
        HOLD_REV = 1 << 3, HOLD_BIT = 1 << 4, HOLD_PAN = 1 << 5,
        HOLD_FLT = 1 << 6, HOLD_VOL = 1 << 7,
    };
    uint16_t perf_hold = 0;

    // filter state - new module (Svf2 per channel)
    dsp::Svf2 filter_l;
    dsp::Svf2 filter_r;
    // downsample state
    int     ds_counter = 0;
    fx::q15 ds_held_l = 0;
    fx::q15 ds_held_r = 0;

    // === MG (LFO) - one shared per track, outputs 4 shapes ===
    dsp::Mg mg;
    fx::q15 mg_rate = fx::Q15_ONE / 8;        // default ~2.5 Hz (visible wobble)
    // modulation depth - signed q15 (negative = invert)
    int16_t mg_to_cutoff = 0;                  // MG -> cutoff (whichever shape mg_wave selects)
    int16_t mg_to_vca    = 0;                  // MG -> volume
    uint8_t mg_wave      = 0;                  // 0=TRI 1=SAW 2=SQR 3=S&H - which shape is routed to targets

    // === per-track scope (mono, post-DSP post-fader - what the track contributes) ===
    // written by Mixer::render every 4th frame; silent/muted tracks decay to flatline.
    static constexpr std::size_t TSCOPE_SIZE = 256;
    fx::q15 tscope[TSCOPE_SIZE] = {0};
    std::size_t tscope_pos = 0;

    // === voice-steal fade tail (anti-click) ===
    // when replace_voice steals an active voice, its next ~2ms are rendered
    // with a linear fade into this buffer instead of hard-deleting mid-wave.
    // render() mixes the tail into the next chunk. q31: several steals may
    // accumulate before the next render.
    static constexpr std::size_t FADE_TAIL = 256;  // 8ms @ 32k - steal fade.
                                                   // (2ms clicked on dense poly.)
                                                   // MUST fit one mixer chunk (256):
                                                   // render() consumes the whole
                                                   // buffer in a single chunk
    fx::q31 fade_tail[FADE_TAIL * 2] = {0};
    bool    fade_pending = false;

    // poly-gain smoothing: the 1/sqrt(N) voice-count compensation used to jump
    // instantly between chunks (1.0<->0.7 = +/-3dB steps at 125Hz whenever a
    // voice died/spawned) - audible as cyclic static on dense poly material.
    // render() ramps from this to the new target across each chunk.
    fx::q15 vgain_cur = fx::Q15_ONE;

    // param slew state: kaoss/stick writes land at UI rate (60Hz); stepping the
    // SVF coefficients that coarsely is an audible zipper while dragging.
    // apply_track_dsp() one-poles these toward cutoff/resonance each chunk.
    fx::q15 cutoff_sm = fx::Q15_ONE;
    fx::q15 reso_sm   = 0;
};

class Mixer {
public:
    // render `frames` stereo into out (interleaved L/R)
    void render(fx::q15* out, std::size_t frames);

    TrackState& track(int i) { return tracks_[i]; }

    // add a voice to a track. finds a free slot, otherwise steals oldest active.
    // the stolen voice is removed (with a possible click - acceptable for voice stealing).
    // takes ownership of new_voice.
    void replace_voice(int i, Voice* new_voice);

    // primary voice = the most recent active voice of the track (for UI: current_frame, sample_slot)
    Voice* primary_voice(int i);

    // send note_off to all voices of the track (for STOP/release)
    void note_off_all(int i);

    // hard-cut all voices of the track (KIL command - fast silence)
    void cut_all(int i);

    // remove ALL voices of the track (cleanup)
    void clear_voices(int i);

    // reset master DSP state (DC blocker, per-track filter state, reverb tail counter)
    void reset_master_state() {
        dc_x_l_ = dc_y_l_ = 0;
        dc_x_r_ = dc_y_r_ = 0;
        reverb_tail_frames_ = 0;
        for (auto& t : tracks_) {
            t.filter_l.reset();
            t.filter_r.reset();
            t.ds_held_l = t.ds_held_r = 0;
            t.ds_counter = 0;
        }
    }

    // === thread-safety ===
    // platform code (audio_ndsp on 3ds) creates a RecursiveLock and binds the pointer.
    // on non-3ds platforms it stays nullptr -> lock()/unlock() become no-ops.
    // recursive so render() can call replace_voice() via the player without deadlock.
    void set_audio_lock(void* lock_ptr) { audio_lock_ = lock_ptr; }
    void lock();
    void unlock();
    struct LockGuard {
        Mixer& m;
        explicit LockGuard(Mixer& mx) : m(mx) { m.lock(); }
        ~LockGuard() { m.unlock(); }
        LockGuard(const LockGuard&) = delete;
        LockGuard& operator=(const LockGuard&) = delete;
    };

    // master volume
    fx::q15 master = fx::Q15_ONE;

    // === global delay (ping-pong stereo) ===
    static constexpr std::size_t DELAY_BUF = 8192;   // ~256ms (headroom up to ~250ms)
    fx::q15 delay_l[DELAY_BUF] = {0};
    fx::q15 delay_r[DELAY_BUF] = {0};
    std::size_t delay_pos = 0;
    std::size_t delay_time = 6400;       // ~200ms (8th note @ 120bpm)
    fx::q15 delay_feedback = fx::Q15_ONE * 50 / 100;
    fx::q15 delay_wet      = fx::Q15_ONE * 70 / 100;

    // === global reverb (Schroeder) ===
    Reverb   reverb;
    fx::q15  reverb_wet = fx::Q15_ONE * 50 / 100;     // 50% - after the fixes we can go fatter

    // === sidechain duck (kick-pump) ===
    // trigger_duck() slams the envelope to full; render() decays it linearly
    // over duck_rel_frames. per-track duck_amt scales how hard each track dips.
    void trigger_duck() { duck_env_ = fx::Q15_ONE; }
    fx::q15 duck_env() const { return duck_env_; }
    uint32_t duck_rel_frames = 8000;   // ~250ms @32k default

    // === per-track headroom ===
    static constexpr fx::q15 TRACK_HEADROOM = (fx::q15)(fx::Q15_ONE * 35 / 100);   // -9dB

    // === scope buffer (mono of the last N samples of master output) ===
    static constexpr std::size_t SCOPE_SIZE = 512;
    fx::q15 scope[SCOPE_SIZE] = {0};
    std::size_t scope_write_pos = 0;

    // === resample/bounce capture (recording master output into an internal buffer) ===
    void start_resample(std::size_t max_frames);
    void stop_resample();
    bool is_resampling() const { return resample_active_; }
    std::size_t resample_frames() const { return resample_pos_; }
    const fx::q15* resample_data() const { return resample_buf_.data(); }

private:
    std::array<TrackState, NUM_TRACKS> tracks_;
    uint32_t age_counter_ = 0;
    void* audio_lock_ = nullptr;  // opaque RecursiveLock* (set by platform)
    // temporary buffer for a single voice (stereo interleaved)
    static constexpr std::size_t SCRATCH = 512 * 2;
    fx::q15 scratch_[SCRATCH];
    fx::q15 voice_buf_[SCRATCH];   // buffer for a single voice pool
    // q31 accumulators to avoid clipping the sum of 4 voices / 8 tracks
    fx::q31 voice_acc_[SCRATCH];           // sum of voices of one track
    fx::q31 master_l_[SCRATCH / 2];        // sum of dry+wet across all tracks (before the final clipper)
    fx::q31 master_r_[SCRATCH / 2];
    // FX bus accumulators: separate delay and reverb buses (m8-style DEL/REV sends)
    fx::q31 del_bus_l_[SCRATCH / 2];
    fx::q31 del_bus_r_[SCRATCH / 2];
    fx::q31 rev_bus_l_[SCRATCH / 2];
    fx::q31 rev_bus_r_[SCRATCH / 2];
    // resample state
    std::vector<fx::q15> resample_buf_;
    std::size_t resample_pos_ = 0;
    std::size_t resample_max_ = 0;
    bool        resample_active_ = false;
    // master DC blocker state - used to be statics in render(), now members so reset clears them
    int32_t     dc_x_l_ = 0, dc_y_l_ = 0;
    int32_t     dc_x_r_ = 0, dc_y_r_ = 0;
    // reverb tail keep-alive: counter of frames since the last nonzero input to reverb
    // keep reverb alive for ~2sec more (64000 frames @ 32kHz) after the signal disappears
    uint32_t    reverb_tail_frames_ = 0;
    // sidechain duck envelope (q15, 0 = no duck active)
    fx::q15     duck_env_ = 0;
};

} // namespace trackr::audio
