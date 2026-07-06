#include "drum_gen.h"
#include <cmath>
#include <cstdint>

namespace trackr::synth {

constexpr int SR = SAMPLER_SR;   // 32000
constexpr double PI = 3.14159265358979;

// === xorshift noise (per-gen state) ===
struct Rng {
    uint32_t s = 0xCAFEBABE;
    int16_t next_q15() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (int16_t)((int32_t)(s & 0xFFFF) - 32768);
    }
    // normalized [-1, +1]
    double next_norm() {
        return next_q15() / 32768.0;
    }
};

// === q15 saturate ===
static inline fx::q15 sat(double x) {
    int32_t v = (int32_t)(x * 32767.0);
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return (fx::q15)v;
}

// add with saturation
static inline fx::q15 mix(fx::q15 a, double v) {
    int32_t r = (int32_t)a + (int32_t)(v * 32767.0);
    if (r >  32767) r =  32767;
    if (r < -32768) r = -32768;
    return (fx::q15)r;
}

// === init_sample helper ===
static void init_sample(Sample& s, std::size_t frames, int root_note) {
    s.data.assign(frames, 0);
    s.channels   = 1;
    s.root_note  = root_note;
    s.loop_start = 0;
    s.loop_end   = 0;
    s.reversed   = false;
    for (int i = 0; i < Sample::MAX_CHOPS; ++i) s.chops[i] = 0xFFFFFFFFu;
}

// === KICK: 808-style sine sweep + click ===
static void gen_kick(Sample& s) {
    constexpr std::size_t N = 4096;     // 128ms @ 32kHz
    init_sample(s, N, 36);              // C-2

    Rng rng;
    double phase = 0.0;
    // sweep: f_start=140Hz -> f_end=45Hz exponentially over 50ms
    double sweep_frames = SR * 0.050;

    for (std::size_t i = 0; i < N; ++i) {
        // pitch sweep (exponential)
        double t_sweep = (i < (std::size_t)sweep_frames) ? (i / sweep_frames) : 1.0;
        double freq = 140.0 * std::pow(45.0 / 140.0, t_sweep);
        phase += 2.0 * PI * freq / SR;
        if (phase > 2.0 * PI) phase -= 2.0 * PI;

        // amp envelope: instant attack, exp decay tau ~80ms
        double env = std::exp(-(double)i / (SR * 0.080));

        double sine = std::sin(phase) * env;
        // click - add a first 80 frames noise burst
        double click = 0.0;
        if (i < 80) {
            double click_env = std::exp(-(double)i / 30.0);
            click = rng.next_norm() * click_env * 0.4;
        }
        s.data[i] = sat((sine + click) * 0.95);
    }
}

// === SNARE: noise + 200Hz tone ===
static void gen_snare(Sample& s) {
    constexpr std::size_t N = 5120;     // 160ms
    init_sample(s, N, 60);

    Rng rng;
    double phase = 0.0;
    double freq = 220.0;

    for (std::size_t i = 0; i < N; ++i) {
        phase += 2.0 * PI * freq / SR;
        if (phase > 2.0 * PI) phase -= 2.0 * PI;

        // tone decay tau ~30ms
        double tone_env  = std::exp(-(double)i / (SR * 0.030));
        // noise decay tau ~80ms
        double noise_env = std::exp(-(double)i / (SR * 0.080));

        double tone  = std::sin(phase) * tone_env * 0.4;
        double noise = rng.next_norm() * noise_env * 0.7;
        s.data[i] = sat((tone + noise) * 0.85);
    }
}

// === CLOSED HAT: short noise burst with highpass-ish ===
static void gen_closed_hat(Sample& s) {
    constexpr std::size_t N = 1280;     // 40ms
    init_sample(s, N, 60);

    Rng rng;
    double prev = 0.0;     // for the naive highpass (subtract running avg)

    for (std::size_t i = 0; i < N; ++i) {
        double n = rng.next_norm();
        // highpass: y = x - lowpass(x). lowpass with tau=4 frames
        prev = prev * 0.7 + n * 0.3;
        double hp = n - prev;

        double env = std::exp(-(double)i / (SR * 0.020));   // tau 20ms
        s.data[i] = sat(hp * env * 0.8);
    }
}

// === OPEN HAT: longer noise burst ===
static void gen_open_hat(Sample& s) {
    constexpr std::size_t N = 8192;     // 256ms
    init_sample(s, N, 60);

    Rng rng;
    double prev = 0.0;

    for (std::size_t i = 0; i < N; ++i) {
        double n = rng.next_norm();
        prev = prev * 0.7 + n * 0.3;
        double hp = n - prev;

        // longer exponential decay tau ~120ms
        double env = std::exp(-(double)i / (SR * 0.120));
        s.data[i] = sat(hp * env * 0.7);
    }
}

// === CLAP: 4 quick noise bursts with short gaps ===
static void gen_clap(Sample& s) {
    constexpr std::size_t N = 6400;     // 200ms
    init_sample(s, N, 60);

    Rng rng;
    // burst centers @ 0, 8ms, 16ms, 24ms (256, 512, 768 frames)
    const std::size_t bursts[4] = { 0, 256, 512, 768 };
    constexpr std::size_t BURST_LEN = 200;

    for (std::size_t i = 0; i < N; ++i) {
        double signal = 0.0;
        // quick 4 transients
        for (int b = 0; b < 4; ++b) {
            if (i >= bursts[b] && i < bursts[b] + BURST_LEN) {
                double t = (i - bursts[b]) / (double)BURST_LEN;
                double burst_env = std::exp(-t * 8.0);
                signal += rng.next_norm() * burst_env * 0.5;
            }
        }
        // tail noise after the last burst (reverb-like)
        if (i >= 968) {
            double tail_env = std::exp(-(double)(i - 968) / (SR * 0.060));
            signal += rng.next_norm() * tail_env * 0.25;
        }
        s.data[i] = sat(signal);
    }
}

// === TOM: low sine sweep ===
static void gen_tom(Sample& s) {
    constexpr std::size_t N = 6144;     // 192ms
    init_sample(s, N, 48);              // C-3

    double phase = 0.0;
    double sweep_frames = SR * 0.040;

    for (std::size_t i = 0; i < N; ++i) {
        double t_sweep = (i < (std::size_t)sweep_frames) ? (i / sweep_frames) : 1.0;
        double freq = 130.0 * std::pow(80.0 / 130.0, t_sweep);
        phase += 2.0 * PI * freq / SR;
        if (phase > 2.0 * PI) phase -= 2.0 * PI;

        double env = std::exp(-(double)i / (SR * 0.120));
        s.data[i] = sat(std::sin(phase) * env * 0.9);
    }
}

// === RIM: short noise + 1500Hz click ===
static void gen_rim(Sample& s) {
    constexpr std::size_t N = 1280;     // 40ms
    init_sample(s, N, 72);              // C-5

    Rng rng;
    double phase = 0.0;
    double freq = 1500.0;

    for (std::size_t i = 0; i < N; ++i) {
        phase += 2.0 * PI * freq / SR;
        if (phase > 2.0 * PI) phase -= 2.0 * PI;

        double tone_env  = std::exp(-(double)i / (SR * 0.008));   // 8ms click
        double noise_env = std::exp(-(double)i / (SR * 0.015));   // 15ms noise

        double tone  = std::sin(phase) * tone_env * 0.5;
        double noise = rng.next_norm() * noise_env * 0.4;
        s.data[i] = sat((tone + noise) * 0.85);
    }
}

// === COWBELL: 2 sine partials ===
static void gen_cowbell(Sample& s) {
    constexpr std::size_t N = 3072;     // 96ms
    init_sample(s, N, 72);

    double phase1 = 0.0, phase2 = 0.0;
    double f1 = 560.0, f2 = 845.0;     // 5:7.55 ratio

    for (std::size_t i = 0; i < N; ++i) {
        phase1 += 2.0 * PI * f1 / SR;
        phase2 += 2.0 * PI * f2 / SR;
        if (phase1 > 2.0 * PI) phase1 -= 2.0 * PI;
        if (phase2 > 2.0 * PI) phase2 -= 2.0 * PI;

        // 2-stage env: fast attack, decay tau ~50ms
        double env = std::exp(-(double)i / (SR * 0.050));
        double signal = (std::sin(phase1) * 0.5 + std::sin(phase2) * 0.5) * env;
        s.data[i] = sat(signal * 0.85);
    }
}

// === BASS808: long sine with pitch sweep, sustain ===
static void gen_bass808(Sample& s) {
    constexpr std::size_t N = 16384;    // 512ms — loopable
    init_sample(s, N, 36);              // C-2 root

    double phase = 0.0;
    double sweep_frames = SR * 0.020;

    for (std::size_t i = 0; i < N; ++i) {
        double t_sweep = (i < (std::size_t)sweep_frames) ? (i / sweep_frames) : 1.0;
        double freq = 100.0 * std::pow(65.0 / 100.0, t_sweep);
        phase += 2.0 * PI * freq / SR;
        if (phase > 2.0 * PI) phase -= 2.0 * PI;

        // long decay tau ~300ms
        double env = std::exp(-(double)i / (SR * 0.300));
        // light sub harmonics for fatness
        double sub = std::sin(phase * 0.5) * 0.3;
        double signal = (std::sin(phase) + sub) * env;
        s.data[i] = sat(signal * 0.8);
    }
    // loop the last 2k frames so long notes can be played
    s.loop_start = N - 2048;
    s.loop_end   = N;
}

// === LEAD: bright square with decay ===
static void gen_lead(Sample& s) {
    constexpr std::size_t N = 8192;     // 256ms
    init_sample(s, N, 60);

    double phase = 0.0;
    double freq = 261.6;     // C4

    for (std::size_t i = 0; i < N; ++i) {
        phase += 2.0 * PI * freq / SR;
        if (phase > 2.0 * PI) phase -= 2.0 * PI;

        double square = (std::sin(phase) > 0) ? 0.7 : -0.7;
        double env = std::exp(-(double)i / (SR * 0.250));
        s.data[i] = sat(square * env * 0.7);
    }
}

// === PLUCK: sine + filter sweep + decay ===
static void gen_pluck(Sample& s) {
    constexpr std::size_t N = 6144;     // 192ms
    init_sample(s, N, 60);

    double phase = 0.0;
    double freq = 261.6;     // C4
    double lp = 0.0;          // 1-pole lowpass state

    for (std::size_t i = 0; i < N; ++i) {
        phase += 2.0 * PI * freq / SR;
        if (phase > 2.0 * PI) phase -= 2.0 * PI;

        // fundamental + 2x harmonic for bite
        double raw = std::sin(phase) * 0.6 + std::sin(phase * 2.0) * 0.3;

        // filter cutoff sweep: open at the start, closes over ~100ms
        double t_sweep = std::min(1.0, (double)i / (SR * 0.080));
        double alpha = 0.5 - t_sweep * 0.4;       // 0.5 → 0.1
        lp = lp * (1.0 - alpha) + raw * alpha;

        double env = std::exp(-(double)i / (SR * 0.100));
        s.data[i] = sat(lp * env * 0.9);
    }
}

// === PAD: slow attack + loop sustain ===
static void gen_pad(Sample& s) {
    constexpr std::size_t N = 16384;    // 512ms — loopable
    init_sample(s, N, 60);

    double phase  = 0.0;
    double phase2 = 0.0;
    double phase3 = 0.0;
    double freq = 261.6;     // C4

    constexpr std::size_t ATTACK = 4096;     // 128ms attack ramp

    for (std::size_t i = 0; i < N; ++i) {
        phase  += 2.0 * PI * freq         / SR;
        phase2 += 2.0 * PI * (freq * 1.005) / SR;   // detune for chorus-like
        phase3 += 2.0 * PI * (freq * 0.995) / SR;
        if (phase  > 2.0 * PI) phase  -= 2.0 * PI;
        if (phase2 > 2.0 * PI) phase2 -= 2.0 * PI;
        if (phase3 > 2.0 * PI) phase3 -= 2.0 * PI;

        // saw approximation via a limited fourier series
        auto saw = [](double p) {
            return (p / PI) - 1.0;
        };
        double signal = (saw(phase) + saw(phase2) + saw(phase3)) / 3.0;

        // attack ramp
        double env = (i < ATTACK) ? ((double)i / ATTACK) : 1.0;
        s.data[i] = sat(signal * env * 0.5);
    }
    // loop at the end
    s.loop_start = ATTACK;
    s.loop_end   = N;
}

// === dispatcher ===
const char* drum_type_name(DrumType t) {
    switch (t) {
        case DrumType::Kick:       return "KICK";
        case DrumType::Snare:      return "SNARE";
        case DrumType::ClosedHat:  return "CL HAT";
        case DrumType::OpenHat:    return "OP HAT";
        case DrumType::Clap:       return "CLAP";
        case DrumType::Tom:        return "TOM";
        case DrumType::Rim:        return "RIM";
        case DrumType::Cowbell:    return "COWBELL";
        case DrumType::Bass808:    return "BASS 808";
        case DrumType::Lead:       return "LEAD";
        case DrumType::Pluck:      return "PLUCK";
        case DrumType::Pad:        return "PAD";
        default: return "?";
    }
}

void generate_drum(Sample& dst, DrumType type) {
    switch (type) {
        case DrumType::Kick:      gen_kick(dst);       break;
        case DrumType::Snare:     gen_snare(dst);      break;
        case DrumType::ClosedHat: gen_closed_hat(dst); break;
        case DrumType::OpenHat:   gen_open_hat(dst);   break;
        case DrumType::Clap:      gen_clap(dst);       break;
        case DrumType::Tom:       gen_tom(dst);        break;
        case DrumType::Rim:       gen_rim(dst);        break;
        case DrumType::Cowbell:   gen_cowbell(dst);    break;
        case DrumType::Bass808:   gen_bass808(dst);    break;
        case DrumType::Lead:      gen_lead(dst);       break;
        case DrumType::Pluck:     gen_pluck(dst);      break;
        case DrumType::Pad:       gen_pad(dst);        break;
        default: break;
    }
}

} // namespace trackr::synth
