// player: ticks every "tick" (sub-step), advances positions,
// triggers note_on on the voices
#pragma once
#include "types.h"
#include "../audio/mixer.h"

namespace trackr::seq {

class Project; // forward - contains song + chains + phrases + instruments
struct Instrument; // forward - for apply_inst_fx_defaults

class Player {
public:
    Player(Project& proj, audio::Mixer& mixer) : project_(proj), mixer_(mixer) {}

    void play_song(uint16_t from_row = 0);
    void play_chain(int track, uint8_t chain_id);
    void play_phrase(int track, uint8_t phrase_id);
    void stop();
    void stop_track(int track);

    // === live mode: quantized chain launch (m8 live view style) ===
    // queue a chain to start on `track` at the next bar boundary (16 global steps).
    // QUEUE_STOP queues a track stop instead. queueing the same target again
    // cancels it (toggle). if nothing is playing at all, the chain starts
    // immediately and establishes the bar clock.
    static constexpr uint8_t QUEUE_STOP = 0xFE;
    static constexpr int     LIVE_QUANT_STEPS = 16;
    void queue_chain(int track, uint8_t chain_id);
    uint8_t queued(int track) const { return queued_chain_[track]; }
    // global steps until the next launch boundary (for UI countdown)
    int steps_to_bar() const {
        return (int)((LIVE_QUANT_STEPS - (global_step_ % LIVE_QUANT_STEPS)) % LIVE_QUANT_STEPS);
    }

    // apply an instrument's FX defaults (filter/sends/vol/pan/crush) to a mixer track.
    // shared by trigger_step and by UI previews - so the touch keyboard sounds
    // exactly like the sequenced note.
    static void apply_inst_fx_defaults(const Instrument& inst, audio::TrackState& mt);

    // call every audio block: advises how many frames until the next tick
    // and triggers steps when the tick arrives
    // fires due ticks, then consumes up to max_frames; returns frames consumed.
    // call in a loop, rendering the returned count after each call, for
    // tick-accurate (sub-buffer) event timing.
    int32_t advance_upto(int32_t max_frames, int sample_rate);
    void advance(std::size_t frames, int sample_rate);

    bool playing() const { return any_playing_; }
    const TrackPlayState& track_state(int i) const { return tracks_[i]; }

private:
    void trigger_step(int track);
    void fire_note(int track, int note, uint8_t inst, uint8_t vel);  // create+start a voice (shared by trigger/retrig/delay/arp)
    void next_chain_row(int track);
    void next_song_row(int track);
    int  song_content_rows() const;      // last row with any chain +1 (song loop point)
    void apply_track_fx(int track, uint8_t cmd, uint8_t val);   // V/F/B/T — immediate to mixer
    void apply_table_tick(int track);                            // call on every tick
    void reset_track_fx(int track);      // clean per-track mixer DSP (shared by play_*/launch)
    void launch_queued();                // start queued chains on the bar boundary

    Project& project_;
    audio::Mixer& mixer_;
    std::array<TrackPlayState, NUM_TRACKS> tracks_;
    bool any_playing_ = false;

    // === tick engine (M8-style) ===
    // the base time unit is a TICK. frames_to_next_tick_ counts down audio frames
    // until the next tick fires. every TICKS_PER_STEP ticks we advance to a new step.
    int32_t  frames_to_next_tick_ = 0;   // frames until the next tick
    int32_t  tick_in_step_ = 0;          // 0..TICKS_PER_STEP-1: which tick of the current step
    uint32_t tick_counter_ = 0;          // global tick count (for swing parity)
    uint32_t swing_step_ = 0;            // step counter for swing parity (even=lengthen, odd=shorten)
    bool     first_tick_ = true;         // fire step 0 on the very first tick after play

    // groove pattern playback: position in song.groove_steps + ticks of the current step
    uint32_t groove_pos_ = 0;
    int32_t  cur_tps_ = TICKS_PER_STEP;
    int      groove_tps_next();          // pull ticks-per-step for the step being started

    // === live mode queue state ===
    uint8_t  queued_chain_[NUM_TRACKS] = {EMPTY, EMPTY, EMPTY, EMPTY,
                                          EMPTY, EMPTY, EMPTY, EMPTY};
    uint32_t global_step_ = 0;           // steps since play started (bar quantization grid)

    void on_tick();                      // advance one tick: maybe trigger a step, run per-tick FX

    // xorshift32 RNG for trigger-chance (O command). deterministic, cheap.
    uint32_t rng_state_ = 0x1A2B3C4Du;
    inline uint32_t rng_next() {
        uint32_t x = rng_state_;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        rng_state_ = x;
        return x;
    }
};

} // namespace trackr::seq
