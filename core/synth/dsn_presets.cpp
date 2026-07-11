// 12 DSN presets — each one a carefully tuned analog voice.
// values follow DsnSynthParams units: q15 for levels/cutoff, frames for EG times
// (32kHz: 320 = 10ms, 3200 = 100ms, 16000 = 500ms), signed q15 for mod depths.
#include "dsn_presets.h"

namespace trackr::synth {

namespace {
    constexpr fx::q15 Q = fx::Q15_ONE;
    constexpr fx::q15 q(int pct) { return (fx::q15)((int32_t)Q * pct / 100); }
    constexpr int16_t s(int pct) { return (int16_t)((int32_t)Q * pct / 100); }
}

const char* dsn_preset_name(DsnPreset p) {
    switch (p) {
        case DsnPreset::Init:    return "INIT";
        case DsnPreset::Acid:    return "ACID";
        case DsnPreset::Hoover:  return "HOOVER";
        case DsnPreset::Sync:    return "SYNC";
        case DsnPreset::Sub:     return "SUB";
        case DsnPreset::Pwm:     return "PWM";
        case DsnPreset::Strings: return "STRNGS";
        case DsnPreset::Lead:    return "LEAD";
        case DsnPreset::Pluck:   return "PLUCK";
        case DsnPreset::Wobble:  return "WOBBLE";
        case DsnPreset::Trem:    return "TREM";
        case DsnPreset::Zap:     return "ZAP";
        case DsnPreset::Wind:    return "WIND";
        case DsnPreset::Kick:    return "KICK";
        case DsnPreset::Snare:   return "SNARE";
        case DsnPreset::Hat:     return "HAT";
        case DsnPreset::Tom:     return "TOM";
        default: return "?";
    }
}

void dsn_load_preset(DsnSynthParams& dst, DsnPreset p) {
    dst = DsnSynthParams{};   // reset to defaults, then override
    switch (p) {
        case DsnPreset::Init:
            // blank patch: VCO1 saw only, filter wide open, plain EG1 -> VCA.
            // everything else stays at struct defaults - build from here.
            dst.balance = 0;
            break;
        case DsnPreset::Acid:
            // 303: single saw, tight filter env, high res, slight glide
            dst.octave = -1;
            dst.vco1_wave = DsnWave::Saw;
            dst.vco2_wave = DsnWave::Saw;
            dst.balance = 0;                     // VCO1 only
            dst.cutoff = q(18); dst.resonance = q(72);
            dst.portamento = 24;
            dst.eg1_attack = 160; dst.eg1_decay = 6400;
            dst.eg1_sustain = 0; dst.eg1_release = 2400;
            dst.eg1_to_cutoff = s(55);
            dst.drive = q(25);
            break;
        case DsnPreset::Hoover:
            // rave hoover: detuned saws + pwm, eg2 pitch drop-in, chorusy mg
            dst.vco1_wave = DsnWave::Pulse; dst.vco1_pw = q(35);
            dst.vco2_wave = DsnWave::Saw;
            dst.vco2_detune = 18; dst.balance = q(55);
            dst.cutoff = q(85); dst.resonance = q(15);
            dst.eg1_attack = 640; dst.eg1_sustain = q(85); dst.eg1_release = 9600;
            dst.eg2_attack = 0; dst.eg2_decay = 4800; dst.eg2_sustain = 0;
            dst.eg2_to_pitch = s(-10);           // pitch swoops UP into the note
            dst.mg2_rate = q(14); dst.mg2_to_pw = s(45);
            dst.drive = q(20);
            break;
        case DsnPreset::Sync:
            // hard-sync lead: vco2 sync'd +7semi, eg2 sweeps vco2 pitch = sync scream
            dst.vco1_wave = DsnWave::Saw;
            dst.vco2_wave = DsnWave::Saw;
            dst.vco2_sync = 1; dst.vco2_semi = 7;
            dst.balance = Q;                     // listen to the sync'd osc
            dst.cutoff = q(90);
            dst.eg1_sustain = q(80); dst.eg1_release = 4800;
            dst.eg2_attack = 0; dst.eg2_decay = 12800; dst.eg2_sustain = 0;
            dst.eg2_to_pitch = s(30);            // sweep = the classic sync wow
            break;
        case DsnPreset::Sub:
            dst.octave = -2;
            dst.vco1_wave = DsnWave::Sine;
            dst.balance = 0;
            dst.cutoff = Q; dst.vcf_type = 4;    // filter off - pure sine
            dst.eg1_attack = 160; dst.eg1_sustain = Q; dst.eg1_release = 3200;
            dst.vca_level = Q;
            break;
        case DsnPreset::Pwm:
            dst.vco1_wave = DsnWave::Pulse; dst.vco1_pw = q(50);
            dst.vco2_wave = DsnWave::Pulse; dst.vco2_detune = 8;
            dst.balance = q(45);
            dst.cutoff = q(70);
            dst.eg1_attack = 2400; dst.eg1_sustain = q(90); dst.eg1_release = 12800;
            dst.mg2_rate = q(8); dst.mg2_to_pw = s(60);   // the lush drift
            break;
        case DsnPreset::Strings:
            dst.vco1_wave = DsnWave::Saw;
            dst.vco2_wave = DsnWave::Saw; dst.vco2_detune = 14;
            dst.balance = q(50);
            dst.cutoff = q(55); dst.resonance = q(8);
            dst.eg1_attack = 9600; dst.eg1_sustain = q(85); dst.eg1_release = 16000;
            dst.mg1_rate = q(10); dst.mg1_to_cutoff = s(12);   // slow shimmer
            break;
        case DsnPreset::Lead:
            dst.vco1_wave = DsnWave::Pulse; dst.vco1_pw = q(30);
            dst.vco2_wave = DsnWave::Saw; dst.vco2_detune = -6;
            dst.balance = q(40);
            dst.cutoff = q(75); dst.resonance = q(20);
            dst.portamento = 12;
            dst.eg1_sustain = q(90); dst.eg1_release = 3200;
            dst.mg1_rate = q(22); dst.mg1_to_pitch = s(3);     // gentle vibrato
            break;
        case DsnPreset::Pluck:
            dst.vco1_wave = DsnWave::Tri;
            dst.vco2_wave = DsnWave::Pulse; dst.vco2_semi = 12; dst.vco2_detune = 4;
            dst.balance = q(30);
            dst.cutoff = q(10); dst.resonance = q(35);
            dst.eg1_attack = 0; dst.eg1_decay = 4800;
            dst.eg1_sustain = 0; dst.eg1_release = 4800;
            dst.eg1_to_cutoff = s(65);           // the pluck IS the filter env
            break;
        case DsnPreset::Wobble:
            dst.octave = -1;
            dst.vco1_wave = DsnWave::Saw;
            dst.vco2_wave = DsnWave::Saw; dst.vco2_semi = -12;
            dst.balance = q(40);
            dst.cutoff = q(20); dst.resonance = q(45);
            dst.eg1_sustain = Q; dst.eg1_release = 2400;
            dst.mg1_wave = 0; dst.mg1_rate = q(30);
            dst.mg1_to_cutoff = s(50);           // the wobble
            dst.drive = q(30);
            break;
        case DsnPreset::Trem:
            dst.vco1_wave = DsnWave::Tri;
            dst.vco2_wave = DsnWave::Tri; dst.vco2_detune = 6;
            dst.balance = q(50);
            dst.cutoff = q(60);
            dst.eg1_attack = 4800; dst.eg1_sustain = q(90); dst.eg1_release = 12800;
            dst.mg2_wave = 0; dst.mg2_rate = q(25);
            dst.mg2_to_vca = s(-60);             // tremolo
            break;
        case DsnPreset::Zap:
            // laser zap / synth perc: eg2 yanks pitch down fast, no sustain
            dst.vco1_wave = DsnWave::Tri;
            dst.balance = 0;
            dst.cutoff = q(80);
            dst.eg1_attack = 0; dst.eg1_decay = 3200;
            dst.eg1_sustain = 0; dst.eg1_release = 1600;
            dst.eg2_attack = 0; dst.eg2_decay = 2400; dst.eg2_sustain = 0;
            dst.eg2_to_pitch = s(60);            // start high, drop fast
            dst.vca_mode = 1;
            break;
        case DsnPreset::Wind:
            dst.vco1_wave = DsnWave::Noise;
            dst.balance = 0;
            dst.vcf_type = 2;                    // BPF
            dst.cutoff = q(40); dst.resonance = q(55);
            dst.eg1_attack = 12800; dst.eg1_sustain = q(70); dst.eg1_release = 16000;
            dst.mg1_rate = q(6); dst.mg1_to_cutoff = s(25);   // slow sweep
            break;
        // === drum presets (v1.0.3, requested on discord) ===
        case DsnPreset::Kick:
            dst.octave = -2;
            dst.vco1_wave = DsnWave::Sine; dst.balance = 0;
            dst.cutoff = q(60);
            dst.eg1_attack = 0; dst.eg1_decay = 3200;  dst.eg1_sustain = 0; dst.eg1_release = 800;
            dst.eg2_attack = 0; dst.eg2_decay = 1600;  dst.eg2_sustain = 0;
            dst.eg2_to_pitch = s(60);                  // click + body drop
            dst.drive = q(30);
            break;
        case DsnPreset::Snare:
            dst.vco1_wave = DsnWave::Tri;
            dst.vco2_wave = DsnWave::Noise; dst.balance = q(65);
            dst.vcf_type = 2;                          // BPF = snare body
            dst.cutoff = q(45); dst.resonance = q(30);
            dst.eg1_attack = 0; dst.eg1_decay = 2400;  dst.eg1_sustain = 0; dst.eg1_release = 1200;
            dst.eg2_attack = 0; dst.eg2_decay = 800;   dst.eg2_sustain = 0;
            dst.eg2_to_pitch = s(25);
            break;
        case DsnPreset::Hat:
            dst.vco1_wave = DsnWave::Noise; dst.balance = 0;
            dst.vcf_type = 1;                          // HPF = sizzle only
            dst.cutoff = q(80);
            dst.eg1_attack = 0; dst.eg1_decay = 800;   dst.eg1_sustain = 0; dst.eg1_release = 320;
            break;
        case DsnPreset::Tom:
            dst.octave = -1;
            dst.vco1_wave = DsnWave::Sine; dst.balance = 0;
            dst.cutoff = q(55);
            dst.eg1_attack = 0; dst.eg1_decay = 4800;  dst.eg1_sustain = 0; dst.eg1_release = 1600;
            dst.eg2_attack = 0; dst.eg2_decay = 2400;  dst.eg2_sustain = 0;
            dst.eg2_to_pitch = s(35);
            break;
        default: break;
    }
}

} // namespace trackr::synth
