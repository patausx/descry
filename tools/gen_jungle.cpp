// host-side generator: JUNGLE / breakbeat demo -> slot 00 "IRONLUNG".
//
// this is the flagship demo for descry: 174bpm jungle built the way jungle is
// actually made - ONE break, sliced to 16ths, rearranged step by step. the
// break itself is synthesized here (procedural kick/snare/hats mixed into one
// 2-bar loop at exactly the project tempo) so the demo ships without needing
// any copyrighted amen.
//
// Song playback has a shared row clock: chains may have different nominal
// lengths, short chains loop inside the row, and EMPTY cells wait silently until
// the longest chain reaches the common boundary. The explicit 4-phrase rests
// below are kept as arrangement data, not as a synchronization workaround.
//
// build: see docs/ENGINE_NOTES.md ("host-сборка генератора")
// writes: <outdir>/project_00.tr3d + <outdir>/sample_63.s16 + sample_63.name
#include "core/sequencer/project.h"
#include "core/sequencer/serialize.h"
#include "core/synth/drum_gen.h"
#include "core/synth/dsn_presets.h"
#include "core/synth/fm_presets.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

using namespace trackr;
using namespace trackr::seq;
using namespace trackr::synth;

// ============================================================================
// tempo / grid
// ============================================================================
constexpr int   BPM        = 174;              // jungle
constexpr int   SR         = 32000;
constexpr int   BREAK_SLOT = 63;               // reserved flagship-demo slot
constexpr int   BREAK_16THS = 32;              // 2 bars of 16ths = 32 slices
// frames per 16th note. kept in double so the 32 chop markers stay on grid.
static const double SIXTEENTH = (double)SR * 60.0 / ((double)BPM * 4.0);

// ============================================================================
// the break: procedural 2-bar amen-flavoured loop
// ============================================================================
static void build_break(Sample& dst) {
    const std::size_t total = (std::size_t)(SIXTEENTH * BREAK_16THS + 0.5);

    // render the drum voices once, then stamp them onto the timeline
    Sample kick, snare, chat, ohat, rim;
    generate_drum(kick,  DrumType::Kick);
    generate_drum(snare, DrumType::Snare);
    generate_drum(chat,  DrumType::ClosedHat);
    generate_drum(ohat,  DrumType::OpenHat);
    generate_drum(rim,   DrumType::Rim);

    std::vector<double> mix(total, 0.0);
    auto stamp = [&](const Sample& src, double step, double gain) {
        std::size_t at = (std::size_t)(step * SIXTEENTH + 0.5);
        for (std::size_t i = 0; i < src.data.size(); ++i) {
            std::size_t p = at + i;
            if (p >= total) break;             // don't wrap: the tail is the loop seam
            mix[p] += (src.data[i] / 32768.0) * gain;
        }
    };

    // --- the pattern (16ths, 0..31) -------------------------------------
    // bar 1: the "statement". bar 2: the classic displaced-snare answer.
    // kick
    stamp(kick, 0,  1.00); stamp(kick, 10, 0.92);
    stamp(kick, 16, 1.00); stamp(kick, 26, 0.88); stamp(kick, 27, 0.70);
    // snare (backbeat + ghosts)
    stamp(snare, 4,  0.95); stamp(snare, 12, 0.98);
    stamp(snare, 20, 0.95); stamp(snare, 30, 0.90);
    stamp(snare, 14, 0.30); stamp(snare, 22, 0.28);   // ghosts = the jungle swing
    stamp(rim,   19, 0.35); stamp(rim,   7,  0.30);
    // ride/hats on 8ths, accent on the beat
    for (int s = 0; s < BREAK_16THS; s += 2)
        stamp(chat, s, (s % 4 == 0) ? 0.55 : 0.34);
    stamp(ohat, 15, 0.42); stamp(ohat, 31, 0.45);     // open hat into the loop point

    // --- normalize + soft clip ------------------------------------------
    double peak = 0.0;
    for (double v : mix) { double a = std::fabs(v); if (a > peak) peak = a; }
    double norm = (peak > 0.0) ? (0.78 / peak) : 1.0;

    dst.data.assign(total, 0);
    for (std::size_t i = 0; i < total; ++i) {
        double v = mix[i] * norm;
        v = std::tanh(v * 1.05);                       // glue, a touch of drive
        int32_t q = (int32_t)(v * 32767.0);
        if (q >  32767) q =  32767;
        if (q < -32768) q = -32768;
        dst.data[i] = (fx::q15)q;
    }
    dst.channels  = 1;
    dst.root_note = 48;                                // C-3 = slice 1
    dst.loop_start = 0;
    dst.loop_end   = 0;
    dst.reversed   = false;
    dst.slice_rev_mask = 0;
    std::snprintf(dst.name, sizeof(dst.name), "IRONLUNG BREAK 174");

    // 32 chop markers, one per 16th - the whole point of a break machine
    for (int i = 0; i < Sample::MAX_CHOPS; ++i) dst.chops[i] = 0xFFFFFFFFu;
    for (int i = 0; i < BREAK_16THS; ++i)
        dst.chops[i] = (uint32_t)(i * SIXTEENTH + 0.5);
}

// .s16 v3 writer - must match load_sample_from_sd() in platform/3ds/main.cpp
static bool write_s16(const Sample& s, const std::string& dir, int slot) {
    char path[256];
    std::snprintf(path, sizeof(path), "%s/sample_%02d.s16", dir.c_str(), slot);
    FILE* f = std::fopen(path, "wb");
    if (!f) { std::perror(path); return false; }
    struct Hdr {
        uint32_t magic; uint8_t version, channels, root_note, flags;
        uint32_t loop_start, loop_end;
    } h{};
    h.magic = 0x53335254u;  // 'TR3S'
    h.version = 3;
    h.channels = s.channels;
    h.root_note = (uint8_t)s.root_note;
    h.flags = s.reversed ? 1 : 0;
    h.loop_start = s.loop_start;
    h.loop_end = s.loop_end;
    bool ok = std::fwrite(&h, sizeof(h), 1, f) == 1
           && std::fwrite(s.chops, sizeof(s.chops), 1, f) == 1
           && std::fwrite(&s.slice_rev_mask, sizeof(s.slice_rev_mask), 1, f) == 1
           && std::fwrite(s.data.data(), sizeof(int16_t), s.data.size(), f) == s.data.size();
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) return false;

    std::snprintf(path, sizeof(path), "%s/sample_%02d.name", dir.c_str(), slot);
    FILE* nf = std::fopen(path, "wb");
    if (nf) { std::fwrite(s.name, 1, std::strlen(s.name), nf); std::fclose(nf); }
    return true;
}

// ============================================================================
// project authoring helpers
// ============================================================================
static Project* P;
static PhraseStep& step(int ph, int st) { return P->phrases[ph].steps[st]; }
static void note(int ph, int st, int n, int inst, int vel = 0x60) {
    auto& s = step(ph, st);
    s.note = (uint8_t)n; s.instrument = (uint8_t)inst; s.velocity = (uint8_t)vel;
}
static void setfx(int ph, int st, int slot, char cmd, uint8_t val) {
    auto& s = step(ph, st);
    s.fx[slot].cmd = (uint8_t)cmd; s.fx[slot].value = val;
}
// play slice `sl` of the break (chromatic slices: note = root + slice index)
static void slice(int ph, int st, int sl, int inst = 0, int vel = 0x68) {
    note(ph, st, 48 + sl, inst, vel);
}

int main(int argc, char** argv) {
    const std::string outdir = (argc > 1) ? argv[1] : "/tmp";

    // === the break audio ===
    Sample brk;
    build_break(brk);
    if (!write_s16(brk, outdir, BREAK_SLOT)) return 1;

    static Project proj;
    P = &proj;
    std::snprintf(proj.name, sizeof(proj.name), "IRONLUNG");

    auto& song = proj.song;
    song.bpm    = BPM;
    song.groove = 6;            // straight 16ths; jungle swings inside the break
    song.swing  = 0;
    song.scale_root = 9;        // A
    song.scale_type = 2;        // natural minor
    // mix
    // headroom: the break is already a dense full-range mix, so everything
    // stacked on top of it has to stay well under unity or the master soft-clips
    // (measured: naive levels sat at 20% knee = permanent saturation).
    song.master_vol = 190;
    song.dly_time = 150; song.dly_fb = 100; song.dly_wet = 72;
    song.rev_wet  = 80;
    // sidechain: the reinforcement kick (track 3) pumps the basses
    song.duck_src = 3;
    song.duck_rel = 70;
    song.track_duck[1] = 90;    // sub
    song.track_duck[2] = 64;    // reese
    song.track_vol[0] = 205;    // break
    song.track_vol[1] = 180;    // sub
    song.track_vol[2] = 105;    // reese
    song.track_vol[3] = 120;    // kick (reinforcement only - the break has one)
    song.track_vol[4] = 105;    // snare (ditto)
    song.track_vol[5] = 88;     // pad
    song.track_vol[6] = 100;    // stab

    // ======================= INSTRUMENTS =======================
    // I00 BREAK - the star. chromatic slices: note picks the 16th.
    {
        auto& I = proj.instruments[0];
        I.type = InstrumentType::Sampler;
        std::snprintf(I.name, sizeof(I.name), "IRONLUNG");
        I.sampler = SamplerParams{};
        auto& s = I.sampler;
        s.sample_slot = BREAK_SLOT;
        s.chromatic_slices = true;
        s.play_mode = PlayMode::Fwd;
        s.attack = 0; s.decay = 0; s.sustain = fx::Q15_ONE; s.release = 900;
        s.gain = fx::Q15_ONE;
        I.fx_volume = 255;
    }
    // I01 BREAK REV - same slices backwards (fills / turnarounds)
    {
        auto& I = proj.instruments[1];
        I.type = InstrumentType::Sampler;
        std::snprintf(I.name, sizeof(I.name), "GNULNORI");
        I.sampler = SamplerParams{};
        auto& s = I.sampler;
        s.sample_slot = BREAK_SLOT;
        s.chromatic_slices = true;
        s.play_mode = PlayMode::Rev;
        s.reverse = true;
        s.attack = 0; s.decay = 0; s.sustain = fx::Q15_ONE; s.release = 900;
        I.fx_send_rev = 60;
    }
    // I02 SUB - the jungle backbone. pure sine, long, glides.
    {
        auto& I = proj.instruments[2];
        I.type = InstrumentType::DsnSynth;
        std::snprintf(I.name, sizeof(I.name), "TECTONIC");
        I.dsn = DsnSynthParams{};
        auto& d = I.dsn;
        d.vco1_wave = DsnWave::Sine;
        d.balance = 0;                                   // VCO1 only
        d.portamento = 18;                               // slides between roots
        d.eg1_attack = 40; d.eg1_decay = 20000;
        d.eg1_sustain = (fx::q15)(fx::Q15_ONE * 82 / 100);
        d.eg1_release = 3000;
        d.vca_mode = 1;
        d.drive = (fx::q15)(fx::Q15_ONE * 18 / 100);     // harmonics so it reads on phone speakers
        I.fx_volume = 225;
    }
    // I03 REESE - detuned saws beating against each other. THE jungle bass.
    {
        auto& I = proj.instruments[3];
        I.type = InstrumentType::DsnSynth;
        std::snprintf(I.name, sizeof(I.name), "REESE");
        I.dsn = DsnSynthParams{};
        auto& d = I.dsn;
        d.vco1_wave = DsnWave::Saw;
        d.vco2_wave = DsnWave::Saw;
        d.vco2_semi = 0;
        d.vco2_detune = 22;                              // the beating = the reese
        d.balance = fx::Q15_ONE / 2;
        d.vcf_type = 0;                                  // LPF
        d.cutoff = (fx::q15)(fx::Q15_ONE * 34 / 100);
        d.resonance = (fx::q15)(fx::Q15_ONE * 30 / 100);
        d.eg1_attack = 300; d.eg1_decay = 16000;
        d.eg1_sustain = (fx::q15)(fx::Q15_ONE * 70 / 100);
        d.eg1_release = 4000;
        // MG1 -> cutoff: slow movement, the classic reese wobble
        d.mg1_wave = 0;                                  // TRI
        d.mg1_rate = (fx::q15)(fx::Q15_ONE * 6 / 100);
        d.mg1_to_cutoff = (int16_t)(fx::Q15_ONE * 30 / 100);
        d.vca_mode = 1;
        d.drive = (fx::q15)(fx::Q15_ONE * 28 / 100);
        I.fx_send_del = 40;
        I.fx_volume = 200;
    }
    // I04 KICK - reinforcement under the break
    {
        auto& I = proj.instruments[4];
        I.type = InstrumentType::DsnSynth;
        std::snprintf(I.name, sizeof(I.name), "PILEDRIVE");
        dsn_load_preset(I.dsn, DsnPreset::Kick);
        I.type = InstrumentType::DsnSynth;
        I.fx_volume = 235;
    }
    // I05 SNARE - reinforcement on the backbeat
    {
        auto& I = proj.instruments[5];
        I.type = InstrumentType::DsnSynth;
        std::snprintf(I.name, sizeof(I.name), "CRACK");
        dsn_load_preset(I.dsn, DsnPreset::Snare);
        I.fx_send_rev = 90;
        I.fx_volume = 205;
    }
    // I06 PAD - the melancholy jungle wash
    {
        auto& I = proj.instruments[6];
        I.type = InstrumentType::Wavsynth;
        std::snprintf(I.name, sizeof(I.name), "RAINGLASS");
        I.wavsynth = WavsynthParams{};
        auto& w = I.wavsynth;
        w.shape = WaveShape::Saw;
        w.attack = 14000; w.decay = 12000;
        w.sustain = (fx::q15)(fx::Q15_ONE * 55 / 100);
        w.release = 26000;
        w.unison = 3; w.detune_cents = 14;
        w.spread = (fx::q15)(fx::Q15_ONE * 85 / 100);
        I.fx_filter_type = 1;    // LPF
        I.fx_cutoff = 88;
        I.fx_send_rev = 150;
        I.fx_volume = 175;
    }
    // I07 STAB - ragga/hoover chord stab
    {
        auto& I = proj.instruments[7];
        I.type = InstrumentType::FmSynth;
        std::snprintf(I.name, sizeof(I.name), "DREADSTAB");
        fm_load_preset(I.fm, FmPreset::Stab);
        I.fx_send_del = 110;
        I.fx_send_rev = 70;
        I.fx_volume = 190;
    }

    // ======================= PHRASES =======================
    // --- break phrases. slice N = the Nth 16th of the 2-bar loop -------
    // P01 bar 1 straight
    for (int s = 0; s < 16; ++s) slice(1, s, s, 0, (s % 4 == 0) ? 0x74 : 0x66);
    // P02 bar 2 straight
    for (int s = 0; s < 16; ++s) slice(2, s, 16 + s, 0, (s % 4 == 0) ? 0x74 : 0x66);
    // P03 CHOPPED - the classic rearrangement: hold the kick, jump the snare
    {
        const int order[16] = { 0, 1, 10, 11, 4, 5, 2, 3,
                                8, 9, 26, 27, 12, 13, 6, 7 };
        for (int s = 0; s < 16; ++s)
            slice(3, s, order[s], 0, (s % 4 == 0) ? 0x74 : 0x64);
    }
    // P04 ROLLS - stutter the last beat (RTG) = the jungle fill
    {
        const int order[16] = { 0, 1, 2, 3, 4, 5, 6, 7,
                                8, 9, 10, 11, 4, 4, 4, 4 };
        for (int s = 0; s < 16; ++s)
            slice(4, s, order[s], 0, (s % 4 == 0) ? 0x74 : 0x64);
        setfx(4, 12, 0, 'R', 0x03);      // retrig every 3 ticks
        setfx(4, 13, 0, 'R', 0x02);
        setfx(4, 14, 0, 'R', 0x02);
        setfx(4, 15, 0, 'R', 0x01);      // machine-gun into the drop
        setfx(4, 15, 1, 'V', 0xFF);
    }
    // P05 INTRO - filtered, half-time feel (only the strong slices)
    {
        slice(5, 0, 0, 0, 0x70); slice(5, 4, 4, 0, 0x68);
        slice(5, 8, 8, 0, 0x64); slice(5, 12, 12, 0, 0x6C);
        setfx(5, 0, 0, 'Y', 0x00);       // LPF
        setfx(5, 0, 1, 'F', 0x48);       // cutoff down = "coming from another room"
        setfx(5, 0, 2, 'Q', 0x30);
    }
    // P06 REVERSE FILL - back half runs backwards, then a reverse snare lift
    {
        for (int s = 0; s < 8; ++s) slice(6, s, 16 + s, 0, 0x66);
        for (int s = 8; s < 16; ++s) slice(6, s, 31 - (s - 8), 1, 0x62);
        setfx(6, 0, 0, 'F', 0xFF);       // make sure the filter is open again
    }
    // --- sub bass: A minor roots, one note per bar mostly ---------------
    // A1=33 G1=31 F1=29 C2=36 E1=28
    note(7, 0,  33, 2, 0x76); setfx(7, 0, 0, 'F', 0xFF);
    note(7, 10, 33, 2, 0x5E);
    note(8, 0,  29, 2, 0x76);            // F1
    note(8, 10, 31, 2, 0x60);            // G1 pickup (glides)
    // --- reese: same roots an octave up, long sustained ------------------
    note(9,  0, 45, 3, 0x60);            // A2
    note(9, 12, 45, 3, 0x50);
    note(10, 0, 41, 3, 0x60);            // F2
    note(10, 8, 43, 3, 0x58);            // G2
    // --- kick reinforcement: lock to the break's kicks -------------------
    note(11, 0, 36, 4, 0x78);
    note(11, 10, 36, 4, 0x66);
    // --- snare reinforcement: the backbeat -------------------------------
    note(12, 4,  60, 5, 0x6E);
    note(12, 12, 60, 5, 0x72);
    // --- pad: Am / F ------------------------------------------------------
    note(13, 0, 57, 6, 0x54);            // A3
    note(14, 0, 53, 6, 0x54);            // F3
    // --- stab: syncopated ragga hits --------------------------------------
    note(15, 2,  69, 7, 0x64);           // A4
    note(15, 7,  72, 7, 0x5C);           // C5
    note(15, 11, 69, 7, 0x60);
    note(16, 2,  67, 7, 0x64);           // G4
    note(16, 6,  72, 7, 0x5C);
    note(16, 13, 76, 7, 0x62);           // E5
    // P17 = intentionally empty (a rest that keeps the grid)

    // ======================= CHAINS =======================
    // All musical chains happen to span four phrases; the engine no longer
    // requires this for synchronization.
    auto set_chain = [&](int c, std::initializer_list<int> phs) {
        int r = 0;
        for (int p : phs) proj.chains[c].rows[r++].phrase = (uint8_t)p;
    };
    set_chain(0, {1, 2, 1, 3});          // break: statement, answer, statement, chop
    set_chain(1, {5, 5, 5, 5});          // break intro (filtered)
    set_chain(2, {1, 2, 3, 4});          // break build -> rolls
    set_chain(3, {1, 2, 3, 6});          // break + reverse fill
    set_chain(4, {7, 7, 7, 8});          // sub
    set_chain(5, {9, 9, 10, 9});         // reese
    set_chain(6, {11, 11, 11, 11});      // kick
    set_chain(7, {12, 12, 12, 12});      // snare
    set_chain(8, {13, 13, 14, 13});      // pad Am Am F Am
    set_chain(9, {15, 15, 16, 15});      // stab
    // Explicit rest chain: useful arrangement data and editable in the tracker.
    set_chain(15, {17, 17, 17, 17});

    // ======================= SONG =======================
    auto put = [&](int row, int track, int chain) {
        song.rows[row].chain[track] = (uint8_t)chain;
    };
    constexpr int REST = 15;
    // t0 break | t1 sub | t2 reese | t3 kick | t4 snare | t5 pad | t6 stab
    // start every used track on a rest, then overwrite where it plays.
    for (int r = 0; r < 8; ++r)
        for (int t = 0; t < 7; ++t) put(r, t, REST);
    // r0 intro: distant break + pad
    put(0, 0, 1); put(0, 5, 8);
    // r1: sub arrives
    put(1, 0, 0); put(1, 1, 4); put(1, 5, 8);
    // r2 DROP: full weight
    put(2, 0, 0); put(2, 1, 4); put(2, 2, 5); put(2, 3, 6); put(2, 4, 7); put(2, 5, 8);
    // r3: reverse fill + stabs
    put(3, 0, 3); put(3, 1, 4); put(3, 2, 5); put(3, 3, 6); put(3, 4, 7); put(3, 6, 9);
    // r4 breakdown: bass + pad + stabs, drums gone
    put(4, 1, 4); put(4, 5, 8); put(4, 6, 9);
    // r5 build: break returns, rolls at the end
    put(5, 0, 2); put(5, 1, 4); put(5, 5, 8);
    // r6 DROP 2: everything
    put(6, 0, 0); put(6, 1, 4); put(6, 2, 5); put(6, 3, 6); put(6, 4, 7); put(6, 6, 9);
    // r7 outro
    put(7, 0, 3); put(7, 1, 4); put(7, 5, 8);

    // ======================= WRITE =======================
    char path[256];
    std::snprintf(path, sizeof(path), "%s/project_00.tr3d", outdir.c_str());
    FILE* f = std::fopen(path, "wb");
    if (!f) { std::perror(path); return 1; }
    ProjectFileHeader h{};
    h.magic = PROJECT_MAGIC;
    h.version = PROJECT_VERSION;
    h.project_size = sizeof(Project);
    std::fwrite(&h, sizeof(h), 1, f);
    std::fwrite(&proj, sizeof(Project), 1, f);
    std::fclose(f);

    std::printf("IRONLUNG: %d bpm, break %zu frames (%.2fs), %d slices\n",
                BPM, brk.data.size(), brk.data.size() / (double)SR, BREAK_16THS);
    std::printf("wrote %s\n", path);
    std::printf("wrote %s/sample_%02d.s16\n", outdir.c_str(), BREAK_SLOT);
    return 0;
}
