// dsn_synth: full analog-style voice — the heart of descry (DSN-12 killer)
//
// architecture per voice (mono, like the DSN-12's 12 mono voices):
//
//   VCO1 (tri/saw/pulse+PW/sine/noise) ─┐
//                                       ├─ BALANCE ─ VCF (Svf2) ─ VCA (gate/EG + drive) ─ out
//   VCO2 (shape + semi + detune, sync) ─┘
//
//   modulators: EG1 (ADSR), EG2 (ADSR — our win, DSN-12 has one),
//               MG1, MG2 (dsp::Mg, 4 shapes — second MG is our win too)
//
// stage 3: hardwired mod routing (no patch matrix yet):
//   EG1 → cutoff, pitch          (classic filter env / pitch env)
//   EG2 → cutoff, pitch          (second envelope, free assign later)
//   MG1 → pitch, cutoff          (vibrato / wobble)
//   MG2 → VCO1 PW, VCA           (PWM / tremolo)
//
// fixed-point everywhere; pitch modulation via integer exp2 (q16),
// control-rate block = 16 samples for pitch/cutoff recompute.
#pragma once
#include "../audio/voice.h"
#include "../dsp/filter.h"
#include "../dsp/lfo.h"

namespace trackr::synth {

enum class DsnWave : uint8_t { Tri = 0, Saw, Pulse, Sine, Noise };

inline const char* dsn_wave_name(DsnWave w) {
    switch (w) {
        case DsnWave::Tri:   return "TRI";
        case DsnWave::Saw:   return "SAW";
        case DsnWave::Pulse: return "PUL";
        case DsnWave::Sine:  return "SIN";
        case DsnWave::Noise: return "NOI";
    }
    return "?";
}

// POD params - lives in the Instrument union, serialized raw.
struct DsnSynthParams {
    // === PITCH ===
    int8_t  octave     = 0;      // -2..+2
    uint8_t portamento = 0;      // 0 = off, 1..127 = glide time (constant-rate)

    // === VCO1 ===
    DsnWave vco1_wave = DsnWave::Saw;
    fx::q15 vco1_pw   = fx::Q15_ONE / 2;   // pulse width (Pulse shape only)

    // === VCO2 ===
    DsnWave vco2_wave   = DsnWave::Saw;
    int8_t  vco2_semi   = 0;     // -24..+24 semitones
    int8_t  vco2_detune = 0;     // -50..+50 cents
    uint8_t vco2_sync   = 0;     // 1 = hard sync to VCO1

    // === BALANCE === 0 = VCO1 only, Q15_ONE = VCO2 only
    fx::q15 balance = fx::Q15_ONE / 2;

    // === VCF ===
    uint8_t vcf_type  = 0;               // dsp::FilterType (0=LPF 1=HPF 2=BPF 3=Notch 4=Off)
    fx::q15 cutoff    = fx::Q15_ONE;
    fx::q15 resonance = 0;

    // === EG1 (primary; drives VCA when vca_mode=EG) ===
    uint32_t eg1_attack  = 100;
    uint32_t eg1_decay   = 5000;
    fx::q15  eg1_sustain = fx::Q15_ONE * 3 / 4;
    uint32_t eg1_release = 8000;

    // === EG2 (secondary - DSN-12 doesn't have this) ===
    uint32_t eg2_attack  = 100;
    uint32_t eg2_decay   = 8000;
    fx::q15  eg2_sustain = 0;
    uint32_t eg2_release = 4000;

    // === hardwired mod depths (signed q15; negative = inverted) ===
    // full scale for pitch destinations = +/-24 semitones.
    int16_t eg1_to_pitch  = 0;
    int16_t eg1_to_cutoff = 0;
    int16_t eg2_to_pitch  = 0;
    int16_t eg2_to_cutoff = 0;
    int16_t mg1_to_pitch  = 0;
    int16_t mg1_to_cutoff = 0;
    int16_t mg2_to_pw     = 0;
    int16_t mg2_to_vca    = 0;

    // === MG1 / MG2 ===
    uint8_t mg1_wave = 0;                // 0=TRI 1=SAW 2=SQR 3=S&H
    fx::q15 mg1_rate = fx::Q15_ONE / 8;  // set_rate_norm: 0.1..20 Hz
    uint8_t mg2_wave = 0;
    fx::q15 mg2_rate = fx::Q15_ONE / 16;

    // === VCA ===
    uint8_t vca_mode  = 1;               // 0 = GATE, 1 = EG1
    fx::q15 vca_level = fx::Q15_ONE;
    fx::q15 drive     = 0;               // 0 = clean, Q15_ONE = full overdrive (~4x + soft clip)
};

// simple ADSR envelope (q31 core, linear release-to-zero like wavsynth)
struct DsnEg {
    enum class Stage : uint8_t { Idle, Attack, Decay, Sustain, Release };
    Stage    stage = Stage::Idle;
    fx::q31  env   = 0;
    fx::q31  release_start = 0;
    uint32_t pos   = 0;

    inline void gate_on() { stage = Stage::Attack; pos = 0; }   // retrigs from current env (no click)
    inline void gate_off() {
        if (stage != Stage::Idle && stage != Stage::Release) {
            stage = Stage::Release; pos = 0; release_start = env;
        }
    }
    inline void reset() { stage = Stage::Idle; env = 0; pos = 0; }
    inline bool idle() const { return stage == Stage::Idle; }
    inline fx::q15 q15() const { return (fx::q15)(env >> 16); }

    // advance one sample. a/d/r in frames, s in q15.
    void step(uint32_t a, uint32_t d, fx::q15 s, uint32_t r);
};

class DsnSynth : public audio::Voice {
public:
    void note_on(int note, int velocity) override;
    void note_off() override;
    void cut() override;
    bool render(fx::q15* out, std::size_t frames) override;

    int     ui_env_stage(int idx) const override { return (int)(idx ? eg2_ : eg1_).stage; }
    fx::q15 ui_env_level(int idx) const override { return (idx ? eg2_ : eg1_).q15(); }

    DsnSynthParams params;

private:
    static constexpr int CTRL_BLOCK = 16;   // control-rate: pitch/cutoff recompute period

    // oscillator state (16-bit phase like wavsynth, q16 increment)
    uint32_t ph1_ = 0, ph2_ = 0;
    uint32_t inc1_ = 0, inc2_ = 0;
    uint32_t noise_state_ = 0xCAFEBABE;

    // pitch in semitones q16 (glide source of truth)
    int32_t pitch_cur_q16_    = 60 << 16;
    int32_t pitch_target_q16_ = 60 << 16;
    int32_t glide_step_q16_   = 0;          // per control block

    // envelopes / MGs
    DsnEg   eg1_, eg2_;
    dsp::Mg mg1_, mg2_;
    fx::q15 mg1_val_ = 0, mg2_val_ = 0;     // selected shape, control-rate

    // control-rate cached values
    int      ctrl_count_ = 0;
    uint32_t pw_thr_     = 32768;           // pulse threshold in q16 phase units
    int32_t  vca_mod_q15_ = fx::Q15_ONE;    // tremolo factor

    // filter (mono - voice is mono)
    dsp::Svf2 filter_;

    fx::q15 velocity_ = fx::Q15_ONE;

    // GATE-mode anti-click ramp
    bool    gated_ = false;
    int32_t gate_amp_q15_ = 0;              // ramps 0<->ONE

    // KIL hard-cut ramp (like wavsynth)
    int32_t cut_remaining_ = 0;
    fx::q15 cut_amp_q15_   = fx::Q15_ONE;
};

} // namespace trackr::synth
