// wavsynth: wavetable oscillator + adsr envelope
// 4 built-in wavetables: sin, saw, square, triangle (256 samples each)
#pragma once
#include "../audio/voice.h"

namespace trackr::synth {

enum class WaveShape : uint8_t { Sine, Saw, Square, Triangle, Noise, User };

struct WavsynthParams {
    WaveShape shape = WaveShape::Saw;
    uint8_t  user_slot = 0;         // WavetableBank slot for shape==User
    fx::q15 size = fx::Q15_ONE;     // pulse width / shape modifier
    // adsr in "ticks" (samples at sr=32000)
    uint32_t attack  = 100;
    uint32_t decay   = 5000;
    fx::q15  sustain = fx::Q15_ONE / 2;
    uint32_t release = 8000;
    // === unison/detune (ambient) ===
    // unison: 1 = single osc (default), 2-3 = fat detune
    // detune_cents: detuning of the outer oscs from center (0..50)
    // spread: stereo spread (0 = mono, Q15_ONE = max width)
    uint8_t unison       = 1;
    uint8_t detune_cents = 12;
    fx::q15 spread       = fx::Q15_ONE * 60 / 100;
};

constexpr int WAVSYNTH_MAX_OSC = 3;

class Wavsynth : public audio::Voice {
public:
    void note_on(int note, int velocity) override;
    void note_off() override;
    void cut() override;
    bool render(fx::q15* out, std::size_t frames) override;

    int     ui_env_stage(int) const override { return (int)stage_; }
    fx::q15 ui_env_level(int) const override { return (fx::q15)(env_ >> 16); }

    WavsynthParams params;

private:
    // === unison/detune: up to WAVSYNTH_MAX_OSC oscillators inside one voice ===
    fx::uq16 osc_phase_[WAVSYNTH_MAX_OSC] = {0,0,0};
    fx::q16  osc_inc_[WAVSYNTH_MAX_OSC]   = {0,0,0};
    fx::q15  osc_pan_l_[WAVSYNTH_MAX_OSC] = {fx::Q15_ONE,fx::Q15_ONE,fx::Q15_ONE};
    fx::q15  osc_pan_r_[WAVSYNTH_MAX_OSC] = {fx::Q15_ONE,fx::Q15_ONE,fx::Q15_ONE};
    int      n_osc_  = 1;             // actual number of oscs for this note
    fx::q15  osc_gain_ = fx::Q15_ONE; // amplitude normalization of the osc sum

    fx::uq16 phase_     = 0;
    fx::q16  phase_inc_ = 0;
    fx::q15  velocity_  = fx::Q15_ONE;

    enum class Stage { Idle, Attack, Decay, Sustain, Release };
    Stage   stage_ = Stage::Idle;
    fx::q31 env_   = 0;     // q31 for envelope smoothness
    fx::q31 release_start_ = 0;  // env_ level at note_off - for LINEAR release to zero
    uint32_t stage_pos_ = 0;
    // KIL hard-cut: when >0, env ramps to zero over this many samples (anti-click)
    int32_t cut_remaining_ = 0;
    fx::q31 cut_step_ = 0;

    // per-instance noise state - otherwise all noise tracks phase-sync in mono
    uint32_t noise_state_ = 0xCAFEBABE;
};

} // namespace trackr::synth
