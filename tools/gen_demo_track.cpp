// host-side demo track generator: builds a Project in memory and writes a
// .tr3d slot file byte-identical to what the device saves.
// build:  g++ -std=c++17 -I. tools/gen_demo_track.cpp -o /tmp/gendemo
// layout safety: same headers, all-POD Project, no 64-bit members ->
// x86-64 and arm32 agree (verified via static_assert cross-compile).
//
// the track: "FIRSTLIGHT" - melodic techno, A minor, 124 bpm.
// showcases: sidechain duck (kick pumps bass/pad/stab), phrase polymetry
// (hats LEN=12 drift against the 16-grid), EVN conditions (fills), scale,
// per-instrument FX sends, DSN kick/bass/zap + wavsynth pad/stab/arp.
#include "core/sequencer/types.h"
#include "core/sequencer/project.h"
#include "core/sequencer/serialize.h"
#include <cstdio>
#include <cstring>

using namespace trackr;
using namespace trackr::seq;
using namespace trackr::synth;

static Project* P;

// --- tiny helpers -----------------------------------------------------------
static PhraseStep& step(int ph, int st) { return P->phrases[ph].steps[st]; }

static void note(int ph, int st, int n, int inst, int vel = 0x60) {
    auto& s = step(ph, st);
    s.note = (uint8_t)n; s.instrument = (uint8_t)inst; s.velocity = (uint8_t)vel;
}
static void setfx(int ph, int st, int slot, char cmd, uint8_t val) {
    step(ph, st).fx[slot] = { (uint8_t)cmd, val };
}

int main() {
    static Project proj;   // ~62KB, static to keep it off the stack
    P = &proj;
    std::snprintf(proj.name, sizeof(proj.name), "FIRSTLIGHT");

    auto& song = proj.song;
    song.bpm = 124;
    song.scale_root = 9;    // A
    song.scale_type = 2;    // natural minor
    // global fx
    song.dly_time = 242;    // ~242ms ~= 8th @124
    song.dly_fb   = 110;
    song.dly_wet  = 140;
    song.rev_wet  = 125;
    // sidechain: kick on T0 pumps the mix
    song.duck_src = 0;
    song.duck_rel = 70;     // ~340ms release - musical pump at 124
    uint8_t ducks[8] = { 0, 175, 120, 90, 0, 60, 145, 80 };
    std::memcpy(song.track_duck, ducks, 8);
    // trimmed ~-6dB total (was 255-peak): master sat in soft-clip knee 20%+ of samples -> mud on device
    uint8_t vols[8] = { 125, 111, 93, 83, 71, 93, 86, 74 };
    std::memcpy(song.track_vol, vols, 8);

    // === INSTRUMENTS =========================================================
    // I00 KICK - DSN: sine body + fast pitch drop via EG2
    {
        auto& I = proj.instruments[0];
        I.type = InstrumentType::DsnSynth;
        std::snprintf(I.name, sizeof(I.name), "KICK");
        I.dsn = DsnSynthParams{};
        auto& d = I.dsn;
        d.vco1_wave = DsnWave::Sine;
        d.balance = 0;                       // VCO1 only
        d.vcf_type = 0; d.cutoff = fx::Q15_ONE; d.resonance = 0;
        d.eg1_attack = 5;  d.eg1_decay = 5500; d.eg1_sustain = 0; d.eg1_release = 400;
        d.eg2_attack = 1;  d.eg2_decay = 1400; d.eg2_sustain = 0; d.eg2_release = 200;
        d.eg2_to_pitch = (int16_t)(fx::Q15_ONE / 2);   // ~12 semi drop
        d.vca_mode = 1; d.vca_level = fx::Q15_ONE;
        d.drive = (fx::q15)(fx::Q15_ONE * 45 / 100);
    }
    // I01 BASS - DSN: acid-ish saw, EG2 opens the filter per note
    {
        auto& I = proj.instruments[1];
        I.type = InstrumentType::DsnSynth;
        std::snprintf(I.name, sizeof(I.name), "RUBBER");
        I.dsn = DsnSynthParams{};
        auto& d = I.dsn;
        d.vco1_wave = DsnWave::Saw;
        d.vco2_wave = DsnWave::Pulse; d.vco2_semi = -12; d.vco2_detune = 6;
        d.balance = (fx::q15)(fx::Q15_ONE * 35 / 100);
        d.vcf_type = 0;                       // LPF
        d.cutoff = (fx::q15)(fx::Q15_ONE * 30 / 100);
        d.resonance = (fx::q15)(fx::Q15_ONE * 45 / 100);
        d.eg1_attack = 8; d.eg1_decay = 4200; d.eg1_sustain = fx::Q15_ONE / 3; d.eg1_release = 1200;
        d.eg2_attack = 1; d.eg2_decay = 2600; d.eg2_sustain = 0; d.eg2_release = 400;
        d.eg2_to_cutoff = (int16_t)(fx::Q15_ONE * 40 / 100);
        d.vca_mode = 1; d.drive = (fx::q15)(fx::Q15_ONE * 30 / 100);
    }
    // I02 STAB - wavsynth square, tracker-chord via ARP in the phrase
    {
        auto& I = proj.instruments[2];
        I.type = InstrumentType::Wavsynth;
        std::snprintf(I.name, sizeof(I.name), "STAB");
        I.wavsynth = WavsynthParams{};
        auto& w = I.wavsynth;
        w.shape = WaveShape::Square;
        w.attack = 5; w.decay = 2000; w.sustain = 0; w.release = 900;
        w.unison = 3; w.detune_cents = 9; w.spread = (fx::q15)(fx::Q15_ONE * 70 / 100);
        I.fx_send_del = 95;
    }
    // I03 ARP - wavsynth triangle pluck, delay send does the space
    {
        auto& I = proj.instruments[3];
        I.type = InstrumentType::Wavsynth;
        std::snprintf(I.name, sizeof(I.name), "GLINT");
        I.wavsynth = WavsynthParams{};
        auto& w = I.wavsynth;
        w.shape = WaveShape::Triangle;
        w.attack = 3; w.decay = 1500; w.sustain = 0; w.release = 700;
        I.fx_send_del = 85;
        I.fx_send_rev = 30;
    }
    // I04 HAT - noise tick
    {
        auto& I = proj.instruments[4];
        I.type = InstrumentType::Wavsynth;
        std::snprintf(I.name, sizeof(I.name), "TICK");
        I.wavsynth = WavsynthParams{};
        auto& w = I.wavsynth;
        w.shape = WaveShape::Noise;
        w.attack = 2; w.decay = 700; w.sustain = 0; w.release = 200;
        I.fx_filter_type = 2;    // HPF - thin it out
        I.fx_cutoff = 120;
    }
    // I05 CLAP - noise burst + reverb tail
    {
        auto& I = proj.instruments[5];
        I.type = InstrumentType::Wavsynth;
        std::snprintf(I.name, sizeof(I.name), "SNAP");
        I.wavsynth = WavsynthParams{};
        auto& w = I.wavsynth;
        w.shape = WaveShape::Noise;
        w.attack = 4; w.decay = 2800; w.sustain = 0; w.release = 600;
        I.fx_send_rev = 115;
        I.fx_filter_type = 3;    // BPF - clap body
        I.fx_cutoff = 150;
    }
    // I06 PAD - detuned saw wash
    {
        auto& I = proj.instruments[6];
        I.type = InstrumentType::Wavsynth;
        std::snprintf(I.name, sizeof(I.name), "HAZE");
        I.wavsynth = WavsynthParams{};
        auto& w = I.wavsynth;
        w.shape = WaveShape::Saw;
        w.attack = 9000; w.decay = 9000;
        w.sustain = (fx::q15)(fx::Q15_ONE * 55 / 100);
        w.release = 22000;
        w.unison = 3; w.detune_cents = 16; w.spread = (fx::q15)(fx::Q15_ONE * 85 / 100);
        I.fx_send_rev = 95;
        I.fx_volume = 210;
    }
    // I07 ZAP - DSN pitch-drop blip for sparse ear candy
    {
        auto& I = proj.instruments[7];
        I.type = InstrumentType::DsnSynth;
        std::snprintf(I.name, sizeof(I.name), "COMET");
        I.dsn = DsnSynthParams{};
        auto& d = I.dsn;
        d.vco1_wave = DsnWave::Sine; d.balance = 0;
        d.eg1_attack = 4; d.eg1_decay = 6000; d.eg1_sustain = 0; d.eg1_release = 800;
        d.eg2_attack = 1; d.eg2_decay = 5200; d.eg2_sustain = 0;
        d.eg2_to_pitch = (int16_t)(fx::Q15_ONE * 60 / 100);
        d.vca_mode = 1;
        I.fx_send_del = 110;
        I.fx_send_rev = 90;
    }

    // === PHRASES ============================================================
    // notes: A1=33 C2=36 D2=38 E2=40 | A3=57 C4=60 D4=62 E4=64 G4=67 A4=69 | F3=53 G3=55 E3=52
    // P01 kick 4-on-floor
    for (int s : {0, 4, 8, 12}) note(1, s, 33, 0, 0x7F);
    // P02 kick + fill tail
    for (int s : {0, 4, 8, 12}) note(2, s, 33, 0, 0x7F);
    note(2, 14, 33, 0, 0x50); setfx(2, 14, 0, 'O', 0x90);           // ghost, 56% chance
    // P03 bass: driving offbeats
    for (int s : {2, 6, 10, 14}) note(3, s, 33, 1, 0x68);
    note(3, 15, 40, 1, 0x48); setfx(3, 15, 0, 'O', 0x70);           // E2 pickup, 44%
    // P04 bass var: walks up
    note(4, 2, 33, 1, 0x68); note(4, 6, 33, 1, 0x60);
    note(4, 10, 36, 1, 0x64); note(4, 14, 38, 1, 0x66);
    // P05 stabs: Am on 4, F on 12 (tracker chords via ARP)
    note(5, 4, 57, 2, 0x58);  setfx(5, 4, 0, 'J', 0x37);            // A C E
    note(5, 12, 53, 2, 0x52); setfx(5, 12, 0, 'J', 0x47);           // F A C
    // P06 arp: pentatonic run
    {
        const int seq16[16] = {57,60,62,64,67,64,62,60, 57,60,62,64,67,69,67,64};
        for (int s = 0; s < 16; ++s) {
            note(6, s, seq16[s], 3, (s & 1) ? 0x38 : 0x58);
        }
        setfx(6, 0, 0, 'C', 0x24);   // whole run only on pass 2 of 4? no - just step 0 gated
    }
    // (undo that: EVN belongs on sparse hits, not the run head)
    step(6, 0).fx[0] = {0, 0};
    // P07 arp high octave, every other step
    {
        const int hi[8] = {69, 72, 74, 76, 79, 76, 74, 72};
        for (int i = 0; i < 8; ++i) note(7, i * 2, hi[i], 3, 0x4A);
    }
    // P08 HATS - LEN 12 polymetry, drifts against the 16-grid
    P->phrases[8].length = 12;
    for (int s = 0; s < 12; s += 2) note(8, s, 60, 4, (s % 4 == 2) ? 0x60 : 0x38);
    note(8, 7, 60, 4, 0x28); setfx(8, 7, 0, 'O', 0x80);             // ghost 50%
    // P09 CLAP on 2 & 4 + rare pre-hit
    note(9, 4, 60, 5, 0x70); note(9, 12, 60, 5, 0x74);
    note(9, 11, 60, 5, 0x30); setfx(9, 11, 0, 'C', 0x44);           // 4th pass only
    // P0A..P0D PAD chords: Am F G Em (one sustained note each)
    note(0x0A, 0, 57, 6, 0x60);                                   // A3
    note(0x0B, 0, 53, 6, 0x60);                                   // F3
    note(0x0C, 0, 55, 6, 0x5A);                                   // G3
    note(0x0D, 0, 52, 6, 0x60);                                   // E3
    // P0E ZAP: sparse, conditional
    note(0x0E, 0, 69, 7, 0x50);  setfx(0x0E, 0, 0, 'C', 0x14);      // 1st of 4 passes
    note(0x0E, 8, 74, 7, 0x44);  setfx(0x0E, 8, 0, 'O', 0x60);      // 38% chance
    note(0x0E, 12, 72, 7, 0x40); setfx(0x0E, 12, 0, 'C', 0x34);     // 3rd of 4

    // === CHAINS =============================================================
    auto set_chain = [&](int c, std::initializer_list<int> phs) {
        int r = 0;
        for (int p : phs) P->chains[c].rows[r++].phrase = (uint8_t)p;
    };
    set_chain(0, {1, 1, 1, 2});          // kick, fill on 4th
    set_chain(1, {3, 3, 4, 3});          // bass with a walk
    set_chain(2, {5});                    // stabs
    set_chain(3, {6, 6, 6, 7});          // arp, lift on 4th
    set_chain(4, {8});                    // hats (12-step loop drifts)
    set_chain(5, {9});                    // clap
    set_chain(6, {0x0A, 0x0B, 0x0C, 0x0D}); // pad: Am F G Em
    set_chain(7, {0x0E});                 // zap

    // === SONG (rows 0..7, ~8x4 bars arrangement) ============================
    auto put = [&](int row, int track, int chain) {
        song.rows[row].chain[track] = (uint8_t)chain;
    };
    // r0: kick + hats
    put(0, 0, 0); put(0, 4, 4);
    // r1: + bass
    put(1, 0, 0); put(1, 1, 1); put(1, 4, 4);
    // r2: + stab + clap
    put(2, 0, 0); put(2, 1, 1); put(2, 2, 2); put(2, 4, 4); put(2, 5, 5);
    // r3: + arp = full groove
    put(3, 0, 0); put(3, 1, 1); put(3, 2, 2); put(3, 3, 3); put(3, 4, 4); put(3, 5, 5);
    // r4: full + zap candy
    put(4, 0, 0); put(4, 1, 1); put(4, 2, 2); put(4, 3, 3); put(4, 4, 4); put(4, 5, 5); put(4, 7, 7);
    // r5: BREAKDOWN - pad + arp + hats, no kick (duck releases, air opens)
    put(5, 3, 3); put(5, 4, 4); put(5, 6, 6);
    // r6: build - kick returns over the pad
    put(6, 0, 0); put(6, 3, 3); put(6, 4, 4); put(6, 5, 5); put(6, 6, 6);
    // r7: everything
    put(7, 0, 0); put(7, 1, 1); put(7, 2, 2); put(7, 3, 3);
    put(7, 4, 4); put(7, 5, 5); put(7, 6, 6); put(7, 7, 7);

    // === WRITE ==============================================================
    std::printf("sizeof(Project)  = %zu\n", sizeof(Project));
    std::printf("sizeof(Instrument)= %zu\n", sizeof(Instrument));
    std::printf("PROJECT_VERSION  = %u\n", PROJECT_VERSION);

    FILE* f = std::fopen("/tmp/firstlight.tr3d", "wb");
    if (!f) { std::perror("open"); return 1; }
    ProjectFileHeader h{};
    h.magic = PROJECT_MAGIC;
    h.version = PROJECT_VERSION;
    h.project_size = sizeof(Project);
    std::fwrite(&h, sizeof(h), 1, f);
    std::fwrite(&proj, sizeof(Project), 1, f);
    std::fclose(f);
    std::printf("wrote /tmp/firstlight.tr3d\n");
    return 0;
}
