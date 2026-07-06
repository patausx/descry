// host-side generator: AMBIENT demo track -> slot 04.
// build: g++ -std=c++17 -I. tools/gen_ambient.cpp -o /tmp/genambient
// beatless drift in C lydian: three polymetric layers (LEN 16/12/10) that
// realign only every 240 steps, user wavetables (vox/fold from the SD pack),
// no kick, no duck - just slow envelopes, delay and reverb doing the work.
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
    std::snprintf(proj.name, sizeof(proj.name), "SLOWLIGHT");

    auto& song = proj.song;
    song.bpm = 72;
    song.scale_root = 0;    // C
    song.scale_type = 7;    // lydian - the floating #4
    song.dly_time = 255;    // max echo
    song.dly_fb   = 160;    // long repeats
    song.dly_wet  = 150;
    song.rev_wet  = 190;    // swim
    song.duck_src = 0xFF;   // no duck - nothing pumps here
    uint8_t vols[8] = { 200, 185, 170, 160, 150, 0, 0, 0 };
    std::memcpy(song.track_vol, vols, 8);

    // === INSTRUMENTS ===
    // I00 DRONE - DSN: two detuned saws through a dark LPF, MG1 slow cutoff wobble
    {
        auto& I = proj.instruments[0];
        I.type = InstrumentType::DsnSynth;
        std::snprintf(I.name, sizeof(I.name), "BEDROCK");
        I.dsn = DsnSynthParams{};
        auto& d = I.dsn;
        d.vco1_wave = DsnWave::Saw;
        d.vco2_wave = DsnWave::Saw; d.vco2_detune = 9;
        d.balance = fx::Q15_ONE / 2;
        d.vcf_type = 0;
        d.cutoff = (fx::q15)(fx::Q15_ONE * 22 / 100);
        d.resonance = (fx::q15)(fx::Q15_ONE * 20 / 100);
        d.eg1_attack = 28000; d.eg1_decay = 10000;
        d.eg1_sustain = (fx::q15)(fx::Q15_ONE * 70 / 100);
        d.eg1_release = 32000;
        d.mg1_wave = 0;                                   // TRI
        d.mg1_rate = (fx::q15)(fx::Q15_ONE / 64);         // very slow
        d.mg1_to_cutoff = (int16_t)(fx::Q15_ONE * 18 / 100); // breathing filter
        d.vca_mode = 1;
        I.fx_send_rev = 100;
    }
    // I01 VOX - user wavetable 03_vox_ah (falls back to slot 2 if pack order shifts)
    {
        auto& I = proj.instruments[1];
        I.type = InstrumentType::Wavsynth;
        std::snprintf(I.name, sizeof(I.name), "CHOIR");
        I.wavsynth = WavsynthParams{};
        auto& w = I.wavsynth;
        w.shape = WaveShape::User;
        w.user_slot = 2;                                  // 03_vox_ah (alphabetical)
        w.attack = 14000; w.decay = 12000;
        w.sustain = (fx::q15)(fx::Q15_ONE * 60 / 100);
        w.release = 26000;
        w.unison = 3; w.detune_cents = 10;
        w.spread = (fx::q15)(fx::Q15_ONE * 90 / 100);
        I.fx_send_rev = 140;
        I.fx_volume = 215;
    }
    // I02 GLINT - user wavetable 12_fold, plucky, delay does the rhythm
    {
        auto& I = proj.instruments[2];
        I.type = InstrumentType::Wavsynth;
        std::snprintf(I.name, sizeof(I.name), "MOTE");
        I.wavsynth = WavsynthParams{};
        auto& w = I.wavsynth;
        w.shape = WaveShape::User;
        w.user_slot = 11;                                 // 12_fold
        w.attack = 40; w.decay = 5500; w.sustain = 0; w.release = 3000;
        I.fx_send_del = 150;                              // echoes = the rhythm section
        I.fx_send_rev = 80;
        I.fx_volume = 200;
    }
    // I03 BELL - FM ice, very sparse
    {
        auto& I = proj.instruments[3];
        I.type = InstrumentType::FmSynth;
        std::snprintf(I.name, sizeof(I.name), "FARBELL");
        I.fm = FmSynthParams{};
        auto& m = I.fm;
        m.algorithm = 1;    // PAIR
        m.feedback = 1;
        m.master_volume = 90;
        m.ops[0] = { 8, 55, 3, 5000, 0, 0, 2000 };        // r6.0 mod - inharmonic sheen
        m.ops[1] = { 1, 105, 3, 16000, 0, 0, 9000 };      // r1.0 carrier, very long
        m.ops[2] = { 12, 30, 3, 3000, 0, 0, 1500 };       // r10 glint
        m.ops[3] = { 3, 45, 3, 12000, 0, 0, 7000 };       // r2.0 octave
        I.fx_send_del = 120;
        I.fx_send_rev = 110;
    }
    // I04 AIR - filtered noise wash, rises and falls
    {
        auto& I = proj.instruments[4];
        I.type = InstrumentType::Wavsynth;
        std::snprintf(I.name, sizeof(I.name), "STRATA");
        I.wavsynth = WavsynthParams{};
        auto& w = I.wavsynth;
        w.shape = WaveShape::Noise;
        w.attack = 30000; w.decay = 20000;
        w.sustain = (fx::q15)(fx::Q15_ONE * 35 / 100);
        w.release = 30000;
        I.fx_filter_type = 3;   // BPF - wind band
        I.fx_cutoff = 90;
        I.fx_send_rev = 150;
        I.fx_volume = 150;
    }

    // === PHRASES ===
    // C lydian: C2=36 G2=43 | C4=60 D4=62 E4=64 F#4=66 G4=67 A4=69 B4=71 C5=72 E5=76 G5=79
    // P01 DRONE - LEN 16: C2 root, G2 answer at half
    note(1, 0, 36, 0, 0x58);
    note(1, 8, 43, 0, 0x46); setfx(1, 8, 0, 'O', 0x90);   // G sometimes (56%)
    // P02 CHOIR - LEN 12 (polymeter layer 1): C4-E4-B3 triad drift
    P->phrases[2].length = 12;
    note(2, 0, 60, 1, 0x4E);                               // C4
    note(2, 4, 64, 1, 0x44);                               // E4
    note(2, 8, 59, 1, 0x48);                               // B3 (lydian colour)
    // P03 CHOIR var - suspends to F#/A
    P->phrases[3].length = 12;
    note(3, 0, 62, 1, 0x4A);                               // D4
    note(3, 4, 66, 1, 0x42);                               // F#4 - THE lydian note
    note(3, 8, 69, 1, 0x46);                               // A4
    // P04 MOTE - LEN 10 (polymeter layer 2): sparse pentatonic sparks
    P->phrases[4].length = 10;
    note(4, 0, 72, 2, 0x50);                               // C5
    note(4, 3, 79, 2, 0x3C); setfx(4, 3, 0, 'O', 0x78);    // G5, 47%
    note(4, 7, 76, 2, 0x44); setfx(4, 7, 0, 'O', 0x9A);    // E5, 60%
    // P05 MOTE var - climbs
    P->phrases[5].length = 10;
    note(5, 0, 76, 2, 0x48);                               // E5
    note(5, 4, 78, 2, 0x3E); setfx(5, 4, 0, 'O', 0x70);    // F#5 44%
    note(5, 8, 83, 2, 0x40); setfx(5, 8, 0, 'C', 0x23);    // B5 - 2nd of 3 passes
    // P06 FARBELL - LEN 16, one strike, EVN keeps it rare
    note(6, 0, 67, 3, 0x46); setfx(6, 0, 0, 'C', 0x13);    // G4 - 1st of 3 passes
    note(6, 10, 71, 3, 0x3A); setfx(6, 10, 0, 'C', 0x33);  // B4 - 3rd of 3
    // P07 AIR - one long swell per phrase
    note(7, 0, 60, 4, 0x40);
    // P08 DRONE lift - up a fourth (F lydian feel)
    note(8, 0, 41, 0, 0x52);                               // F2
    note(8, 8, 48, 0, 0x42); setfx(8, 8, 0, 'O', 0x80);    // C3 50%

    // === CHAINS ===
    auto set_chain = [&](int c, std::initializer_list<int> phs) {
        int r = 0;
        for (int p : phs) P->chains[c].rows[r++].phrase = (uint8_t)p;
    };
    set_chain(0, {1, 1, 1, 8});           // drone with the lift every 4th
    set_chain(1, {2, 2, 3, 2});           // choir - 12-step layer
    set_chain(2, {4, 4, 5});              // motes - 10-step layer, 3-row cycle
    set_chain(3, {6});                    // far bell
    set_chain(4, {7});                    // air

    // === SONG - very slow build, layers accrete like sediment ===
    auto put = [&](int row, int track, int chain) {
        song.rows[row].chain[track] = (uint8_t)chain;
    };
    // r0: drone alone
    put(0, 0, 0);
    // r1: + air
    put(1, 0, 0); put(1, 4, 4);
    // r2: + choir (12 vs 16 drift begins)
    put(2, 0, 0); put(2, 1, 1); put(2, 4, 4);
    // r3: + motes (10 vs 12 vs 16 - full polymetric web)
    put(3, 0, 0); put(3, 1, 1); put(3, 2, 2); put(3, 4, 4);
    // r4: + far bell = peak density (still barely anything - ambient)
    put(4, 0, 0); put(4, 1, 1); put(4, 2, 2); put(4, 3, 3); put(4, 4, 4);
    // r5: hold
    put(5, 0, 0); put(5, 1, 1); put(5, 2, 2); put(5, 3, 3); put(5, 4, 4);
    // r6: recede - drop the motes
    put(6, 0, 0); put(6, 1, 1); put(6, 3, 3); put(6, 4, 4);
    // r7: drone + air only - back to the start (loop feels seamless)
    put(7, 0, 0); put(7, 4, 4);

    // === WRITE ===
    std::printf("sizeof(Project) = %zu, version %u\n", sizeof(Project), PROJECT_VERSION);
    FILE* f = std::fopen("/tmp/slowlight.tr3d", "wb");
    if (!f) { std::perror("open"); return 1; }
    ProjectFileHeader h{};
    h.magic = PROJECT_MAGIC; h.version = PROJECT_VERSION; h.project_size = sizeof(Project);
    std::fwrite(&h, sizeof(h), 1, f);
    std::fwrite(&proj, sizeof(Project), 1, f);
    std::fclose(f);
    std::printf("wrote /tmp/slowlight.tr3d\n");
    return 0;
}
