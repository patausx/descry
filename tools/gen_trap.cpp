// host-side generator: TRAP demo track -> slot 03.
// build: g++ -std=c++17 -I. tools/gen_trap.cpp -o /tmp/gentrap
// halftime 140: snare on beat 3 only, RTG hat rolls (the trap signature),
// FM bell melody in D minor, long 808 glides, EVN-gated vox stabs.
#include "core/sequencer/types.h"
#include "core/sequencer/project.h"
#include "core/sequencer/serialize.h"
#include <cstdio>
#include <cstring>

using namespace trackr;
using namespace trackr::seq;
using namespace trackr::synth;

static Project* P;
static PhraseStep& step(int ph, int st) { return P->phrases[ph].steps[st]; }
static void note(int ph, int st, int n, int inst, int vel = 0x60) {
    auto& s = step(ph, st);
    s.note = (uint8_t)n; s.instrument = (uint8_t)inst; s.velocity = (uint8_t)vel;
}
static void setfx(int ph, int st, int slot, char cmd, uint8_t val) {
    step(ph, st).fx[slot] = { (uint8_t)cmd, val };
}

int main() {
    static Project proj;
    P = &proj;
    std::snprintf(proj.name, sizeof(proj.name), "COLDCHAIN");

    auto& song = proj.song;
    song.bpm = 140;
    song.scale_root = 2;    // D
    song.scale_type = 2;    // natural minor
    song.dly_time = 214;    // dotted-ish echo
    song.dly_fb   = 100;
    song.dly_wet  = 110;
    song.rev_wet  = 135;
    song.duck_src = 0;
    song.duck_rel = 80;     // slower, deeper pump - halftime has room
    uint8_t ducks[8] = { 0, 150, 90, 0, 0, 70, 110, 0 };
    std::memcpy(song.track_duck, ducks, 8);
    // trimmed ~-6dB total: master was slamming the soft-clip knee
    uint8_t vols[8] = { 125, 118, 101, 96, 78, 91, 83, 0 };
    std::memcpy(song.track_vol, vols, 8);

    // === INSTRUMENTS ===
    // I00 KICK - boomy, longer than phonk
    {
        auto& I = proj.instruments[0];
        I.type = InstrumentType::DsnSynth;
        std::snprintf(I.name, sizeof(I.name), "SLAM");
        I.dsn = DsnSynthParams{};
        auto& d = I.dsn;
        d.vco1_wave = DsnWave::Sine; d.balance = 0;
        d.eg1_attack = 3; d.eg1_decay = 6500; d.eg1_sustain = 0; d.eg1_release = 500;
        d.eg2_attack = 1; d.eg2_decay = 1600; d.eg2_sustain = 0;
        d.eg2_to_pitch = (int16_t)(fx::Q15_ONE / 2);
        d.vca_mode = 1;
        d.drive = (fx::q15)(fx::Q15_ONE * 35 / 100);
    }
    // I01 808 - long boomy sub, heavy glide
    {
        auto& I = proj.instruments[1];
        I.type = InstrumentType::DsnSynth;
        std::snprintf(I.name, sizeof(I.name), "TRUNK");
        I.dsn = DsnSynthParams{};
        auto& d = I.dsn;
        d.vco1_wave = DsnWave::Sine; d.balance = 0;
        d.portamento = 40;                              // slow dramatic glides
        d.eg1_attack = 2; d.eg1_decay = 20000;
        d.eg1_sustain = (fx::q15)(fx::Q15_ONE * 60 / 100);
        d.eg1_release = 3500;
        d.vca_mode = 1;
        d.drive = (fx::q15)(fx::Q15_ONE * 45 / 100);
    }
    // I02 BELL - FM PAIR: op1(r3.0)->op2(r1.0) - the trap bell
    {
        auto& I = proj.instruments[2];
        I.type = InstrumentType::FmSynth;
        std::snprintf(I.name, sizeof(I.name), "ICEBELL");
        I.fm = FmSynthParams{};
        auto& m = I.fm;
        m.algorithm = 1;    // PAIR: 1->2, 3->4
        m.feedback = 2;
        m.master_volume = 105;
        // pair A: mod r3.0 -> carrier r1.0 (bell partials)
        m.ops[0] = { 5, 72, 2, 3500, 0, 0, 1200 };      // idx5=r3.0 modulator, decays
        m.ops[1] = { 1, 110, 2, 9000, 0, 0, 2600 };     // carrier r1.0, long ring
        // pair B: shimmer an octave up, quieter
        m.ops[2] = { 10, 45, 2, 2600, 0, 0, 900 };      // idx10=r8.0 glint mod
        m.ops[3] = { 3, 58, 2, 7000, 0, 0, 2200 };      // carrier r2.0 (octave)
        I.fx_send_del = 105;                             // echo tail = the melody breathes
        I.fx_send_rev = 60;
    }
    // I03 SNARE - tight crack + reverb
    {
        auto& I = proj.instruments[3];
        I.type = InstrumentType::Wavsynth;
        std::snprintf(I.name, sizeof(I.name), "CRACK");
        I.wavsynth = WavsynthParams{};
        auto& w = I.wavsynth;
        w.shape = WaveShape::Noise;
        w.attack = 1; w.decay = 2400; w.sustain = 0; w.release = 400;
        I.fx_filter_type = 3;
        I.fx_cutoff = 155;
        I.fx_send_rev = 100;
    }
    // I04 HAT - the roll machine
    {
        auto& I = proj.instruments[4];
        I.type = InstrumentType::Wavsynth;
        std::snprintf(I.name, sizeof(I.name), "SIZZLE");
        I.wavsynth = WavsynthParams{};
        auto& w = I.wavsynth;
        w.shape = WaveShape::Noise;
        w.attack = 1; w.decay = 480; w.sustain = 0; w.release = 120;
        I.fx_filter_type = 2;
        I.fx_cutoff = 150;
        I.fx_volume = 185;
    }
    // I05 VOX STAB - formant-ish square blip (adlib substitute)
    {
        auto& I = proj.instruments[5];
        I.type = InstrumentType::Wavsynth;
        std::snprintf(I.name, sizeof(I.name), "HUH");
        I.wavsynth = WavsynthParams{};
        auto& w = I.wavsynth;
        w.shape = WaveShape::Square;
        w.attack = 4; w.decay = 1800; w.sustain = 0; w.release = 600;
        w.unison = 2; w.detune_cents = 20;
        I.fx_filter_type = 3;    // BPF = talky
        I.fx_cutoff = 120;
        I.fx_send_del = 90;
        I.fx_send_rev = 70;
    }
    // I06 PAD - minor wash behind everything
    {
        auto& I = proj.instruments[6];
        I.type = InstrumentType::Wavsynth;
        std::snprintf(I.name, sizeof(I.name), "FOG");
        I.wavsynth = WavsynthParams{};
        auto& w = I.wavsynth;
        w.shape = WaveShape::Triangle;
        w.attack = 8000; w.decay = 9000;
        w.sustain = (fx::q15)(fx::Q15_ONE * 50 / 100);
        w.release = 18000;
        w.unison = 3; w.detune_cents = 14;
        w.spread = (fx::q15)(fx::Q15_ONE * 80 / 100);
        I.fx_send_rev = 120;
        I.fx_volume = 190;
    }

    // === PHRASES ===
    // D minor: D1=26 F1=29 A1=33 C2=36 | bell: D5=74 F5=77 A5=81 E5=76 C5=72 A4=69
    // P01 KICK halftime: 0 + syncopated 6, 11 pattern (2-bar feel in one phrase)
    note(1, 0, 26, 0, 0x7F);
    note(1, 6, 26, 0, 0x6E);
    note(1, 11, 26, 0, 0x74);
    // P02 SNARE - halftime: beat 3 ONLY (step 8)
    note(2, 8, 60, 3, 0x7A);
    // P03 snare var - beat 3 + late ghost
    note(2 + 1, 8, 60, 3, 0x7A);          // P03
    note(3, 15, 60, 3, 0x34); setfx(3, 15, 0, 'O', 0x60);   // 38% late ghost
    // P04 808 - roots following kick, glide walk D->F->C
    note(4, 0, 26, 1, 0x72);              // D1
    note(4, 6, 29, 1, 0x62);              // F1 (glide)
    note(4, 11, 24, 1, 0x66);             // C1 (glide down)
    // P05 HATS - the trap grid: 8ths with ROLLS
    for (int s = 0; s < 16; s += 2) note(5, s, 60, 4, (s % 4 == 0) ? 0x52 : 0x3E);
    setfx(5, 6, 0, 'R', 0x02);            // roll (period 2 = triplet feel)
    setfx(5, 14, 0, 'R', 0x01);           // dense roll into the bar
    note(5, 13, 60, 4, 0x30); setfx(5, 13, 0, 'O', 0x66);   // 40% ghost 16th
    // P06 HATS var - heavier rolls, EVN-gated opener
    for (int s = 0; s < 16; s += 2) note(6, s, 60, 4, (s % 4 == 0) ? 0x52 : 0x3E);
    setfx(6, 2, 0, 'R', 0x01);
    setfx(6, 10, 0, 'R', 0x02);
    note(6, 15, 72, 4, 0x48); setfx(6, 15, 0, 'C', 0x24);   // pitch-up tick, 2nd of 4
    // P07 BELL melody A - sparse, echoes fill the space
    note(7, 0, 74, 2, 0x60);              // D5
    note(7, 5, 77, 2, 0x52);              // F5
    note(7, 10, 76, 2, 0x58);             // E5
    note(7, 14, 69, 2, 0x4C);             // A4
    // P08 BELL melody B - answer, higher
    note(8, 0, 81, 2, 0x5C);              // A5
    note(8, 5, 77, 2, 0x50);              // F5
    note(8, 8, 74, 2, 0x54);              // D5
    note(8, 13, 72, 2, 0x48);             // C5
    // P09 VOX - one stab, conditional variety
    note(9, 4, 62, 5, 0x56); setfx(9, 4, 0, 'C', 0x12);     // 1st of 2 passes
    note(9, 12, 65, 5, 0x4E); setfx(9, 12, 0, 'C', 0x22);   // 2nd of 2 - call/answer
    // P0A PAD - Dm hold
    note(0x0A, 0, 50, 6, 0x50);           // D3
    // P0B PAD - Bb (bVI - the trap sadness)
    note(0x0B, 0, 46, 6, 0x50);           // Bb2

    // === CHAINS ===
    auto set_chain = [&](int c, std::initializer_list<int> phs) {
        int r = 0;
        for (int p : phs) P->chains[c].rows[r++].phrase = (uint8_t)p;
    };
    set_chain(0, {1});                    // kick
    set_chain(1, {4});                    // 808
    set_chain(2, {7, 7, 8, 7});           // bell AABA
    set_chain(3, {2, 2, 2, 3});           // snare, ghost on 4th
    set_chain(4, {5, 5, 5, 6});           // hats, heavy rolls on 4th
    set_chain(5, {9});                    // vox
    set_chain(6, {0x0A, 0x0A, 0x0B, 0x0A}); // pad Dm Dm Bb Dm

    // === SONG ===
    auto put = [&](int row, int track, int chain) {
        song.rows[row].chain[track] = (uint8_t)chain;
    };
    // r0: bell + pad intro (trap opens with the melody)
    put(0, 2, 2); put(0, 6, 6);
    // r1: + hats
    put(1, 2, 2); put(1, 4, 4); put(1, 6, 6);
    // r2: DROP - kick + 808 + snare in
    put(2, 0, 0); put(2, 1, 1); put(2, 2, 2); put(2, 3, 3); put(2, 4, 4);
    // r3: full + vox
    put(3, 0, 0); put(3, 1, 1); put(3, 2, 2); put(3, 3, 3); put(3, 4, 4); put(3, 5, 5);
    // r4: full + pad back = peak
    put(4, 0, 0); put(4, 1, 1); put(4, 2, 2); put(4, 3, 3); put(4, 4, 4); put(4, 5, 5); put(4, 6, 6);
    // r5: strip - 808 + hats + vox (verse energy)
    put(5, 1, 1); put(5, 4, 4); put(5, 5, 5); put(5, 6, 6);
    // r6: second drop, everything
    put(6, 0, 0); put(6, 1, 1); put(6, 2, 2); put(6, 3, 3); put(6, 4, 4); put(6, 5, 5); put(6, 6, 6);

    // === WRITE ===
    std::printf("sizeof(Project) = %zu, version %u\n", sizeof(Project), PROJECT_VERSION);
    FILE* f = std::fopen("/tmp/coldchain.tr3d", "wb");
    if (!f) { std::perror("open"); return 1; }
    ProjectFileHeader h{};
    h.magic = PROJECT_MAGIC; h.version = PROJECT_VERSION; h.project_size = sizeof(Project);
    std::fwrite(&h, sizeof(h), 1, f);
    std::fwrite(&proj, sizeof(Project), 1, f);
    std::fclose(f);
    std::printf("wrote /tmp/coldchain.tr3d\n");
    return 0;
}
