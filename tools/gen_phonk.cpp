// host-side generator: PHONK demo track -> slot 02.
// build: g++ -std=c++17 -I. tools/gen_phonk.cpp -o /tmp/genphonk
// memphis phonk: cowbell lead (DSN 2x pulse ratio 1.5 + BPF - the 808 cowbell
// recipe), 808 sub with portamento glides, roomy snare, dark saw pad.
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
    std::snprintf(proj.name, sizeof(proj.name), "GRAVEHILL");

    auto& song = proj.song;
    song.bpm = 132;
    song.scale_root = 9;    // A
    song.scale_type = 3;    // harmonic minor - the memphis darkness
    song.dly_time = 200;
    song.dly_fb   = 90;
    song.dly_wet  = 100;
    song.rev_wet  = 150;
    song.duck_src = 0;      // kick pumps
    song.duck_rel = 55;
    uint8_t ducks[8] = { 0, 200, 60, 0, 40, 130, 0, 0 };
    std::memcpy(song.track_duck, ducks, 8);
    // trimmed ~-7dB: knee% was 20-32 (hot master), keep phonk grit in the drums not the bus
    uint8_t vols[8] = { 115, 107, 95, 90, 68, 75, 0, 0 };
    std::memcpy(song.track_vol, vols, 8);

    // === INSTRUMENTS ===
    // I00 KICK - short punchy DSN
    {
        auto& I = proj.instruments[0];
        I.type = InstrumentType::DsnSynth;
        std::snprintf(I.name, sizeof(I.name), "DIRT");
        I.dsn = DsnSynthParams{};
        auto& d = I.dsn;
        d.vco1_wave = DsnWave::Sine; d.balance = 0;
        d.eg1_attack = 3; d.eg1_decay = 4200; d.eg1_sustain = 0; d.eg1_release = 300;
        d.eg2_attack = 1; d.eg2_decay = 1100; d.eg2_sustain = 0;
        d.eg2_to_pitch = (int16_t)(fx::Q15_ONE * 55 / 100);
        d.vca_mode = 1;
        d.drive = (fx::q15)(fx::Q15_ONE * 55 / 100);   // dirtier than techno kick
    }
    // I01 808 SUB - sine + porta glide + drive (the phonk backbone)
    {
        auto& I = proj.instruments[1];
        I.type = InstrumentType::DsnSynth;
        std::snprintf(I.name, sizeof(I.name), "EIGHT08");
        I.dsn = DsnSynthParams{};
        auto& d = I.dsn;
        d.vco1_wave = DsnWave::Sine; d.balance = 0;
        d.portamento = 28;                              // the glide
        d.eg1_attack = 2; d.eg1_decay = 14000;
        d.eg1_sustain = (fx::q15)(fx::Q15_ONE * 55 / 100);
        d.eg1_release = 2500;
        d.vca_mode = 1;
        d.drive = (fx::q15)(fx::Q15_ONE * 40 / 100);   // harmonics so it reads on small speakers
    }
    // I02 COWBELL - the star. 2x pulse, ratio 1.5 (+7 semi), BPF, short decay
    {
        auto& I = proj.instruments[2];
        I.type = InstrumentType::DsnSynth;
        std::snprintf(I.name, sizeof(I.name), "BELLGRAVE");
        I.dsn = DsnSynthParams{};
        auto& d = I.dsn;
        d.vco1_wave = DsnWave::Pulse; d.vco1_pw = (fx::q15)(fx::Q15_ONE * 48 / 100);
        d.vco2_wave = DsnWave::Pulse; d.vco2_semi = 7;  // ~1.498 ratio = 808 cowbell
        d.balance = fx::Q15_ONE / 2;
        d.vcf_type = 2;                                  // BPF - the "clonk" body
        d.cutoff = (fx::q15)(fx::Q15_ONE * 52 / 100);
        d.resonance = (fx::q15)(fx::Q15_ONE * 38 / 100);
        d.eg1_attack = 2; d.eg1_decay = 2100; d.eg1_sustain = 0; d.eg1_release = 350;
        d.vca_mode = 1;
        d.drive = (fx::q15)(fx::Q15_ONE * 25 / 100);
        I.fx_send_rev = 70;
    }
    // I03 SNARE - roomy noise burst (memphis snares swim in reverb)
    {
        auto& I = proj.instruments[3];
        I.type = InstrumentType::Wavsynth;
        std::snprintf(I.name, sizeof(I.name), "CRYPT");
        I.wavsynth = WavsynthParams{};
        auto& w = I.wavsynth;
        w.shape = WaveShape::Noise;
        w.attack = 2; w.decay = 3200; w.sustain = 0; w.release = 500;
        I.fx_filter_type = 3;    // BPF body
        I.fx_cutoff = 135;
        I.fx_send_rev = 135;     // swim
    }
    // I04 HAT - tick
    {
        auto& I = proj.instruments[4];
        I.type = InstrumentType::Wavsynth;
        std::snprintf(I.name, sizeof(I.name), "TSS");
        I.wavsynth = WavsynthParams{};
        auto& w = I.wavsynth;
        w.shape = WaveShape::Noise;
        w.attack = 1; w.decay = 550; w.sustain = 0; w.release = 150;
        I.fx_filter_type = 2;    // HPF
        I.fx_cutoff = 140;
        I.fx_volume = 190;
    }
    // I05 PAD - dark detuned saw, low
    {
        auto& I = proj.instruments[5];
        I.type = InstrumentType::Wavsynth;
        std::snprintf(I.name, sizeof(I.name), "MIASMA");
        I.wavsynth = WavsynthParams{};
        auto& w = I.wavsynth;
        w.shape = WaveShape::Saw;
        w.attack = 6000; w.decay = 8000;
        w.sustain = (fx::q15)(fx::Q15_ONE * 50 / 100);
        w.release = 14000;
        w.unison = 3; w.detune_cents = 12;
        w.spread = (fx::q15)(fx::Q15_ONE * 75 / 100);
        I.fx_filter_type = 1;    // LPF - keep it murky
        I.fx_cutoff = 95;
        I.fx_send_rev = 110;
        I.fx_volume = 200;
    }

    // === PHRASES ===
    // A1=33 C2=36 E2=40 G1=31 | cowbell zone: A4=69 C5=72 D5=74 E5=76 G4=67 G#4=68
    // P01 KICK - memphis bounce: 0, 10 + ghost on 7
    note(1, 0, 33, 0, 0x7F);
    note(1, 10, 33, 0, 0x78);
    note(1, 7, 33, 0, 0x48); setfx(1, 7, 0, 'O', 0x78);   // ghost ~47%
    // P02 SNARE - 4 and 12 (beats 2/4)
    note(2, 4, 60, 3, 0x72);
    note(2, 12, 60, 3, 0x76);
    // P03 808 - follows the kick, glide into the offbeat
    note(3, 0, 33, 1, 0x70);                               // A1
    note(3, 10, 33, 1, 0x68);
    note(3, 14, 31, 1, 0x5C);                              // G1 pickup (glides)
    // P04 808 var - walks the harmonic minor
    note(4, 0, 33, 1, 0x70);
    note(4, 6, 36, 1, 0x60);                               // C2
    note(4, 10, 40, 1, 0x64);                              // E2
    note(4, 14, 32, 1, 0x58);                              // G#1 - harmonic minor sting
    // P05 COWBELL riff A - the hook (8th notes)
    {
        const int riff[8] = {69, 72, 69, 67, 69, 72, 74, 72};   // A C A G A C D C
        for (int i = 0; i < 8; ++i)
            note(5, i * 2, riff[i], 2, (i % 4 == 0) ? 0x6E : 0x58);
    }
    // P06 COWBELL riff B - descending answer + harmonic minor G#
    {
        const int riff[8] = {76, 74, 72, 74, 72, 69, 68, 69};   // E D C D C A G# A
        for (int i = 0; i < 8; ++i)
            note(6, i * 2, riff[i], 2, (i % 4 == 0) ? 0x6E : 0x58);
    }
    // P07 HATS - 8ths + ghosts, roll into the bar end
    for (int s = 0; s < 16; s += 2) note(7, s, 60, 4, (s % 8 == 0) ? 0x5A : 0x40);
    note(7, 15, 60, 4, 0x36); setfx(7, 15, 0, 'R', 0x02);  // 3-hit roll
    note(7, 9, 60, 4, 0x2C);  setfx(7, 9, 0, 'O', 0x70);   // ghost 16th, 44%
    // P08 PAD - Am drone with a slow filter open via table? keep static: Am
    note(8, 0, 57, 5, 0x54);                                // A3, rings whole phrase
    // P09 PAD var - F (the phonk iv-VI darkness)
    note(9, 0, 53, 5, 0x54);                                // F3

    // === CHAINS ===
    auto set_chain = [&](int c, std::initializer_list<int> phs) {
        int r = 0;
        for (int p : phs) P->chains[c].rows[r++].phrase = (uint8_t)p;
    };
    set_chain(0, {1});                    // kick
    set_chain(1, {3, 3, 3, 4});           // 808 with a walk on the 4th
    set_chain(2, {5, 5, 6, 6});           // cowbell A A B B
    set_chain(3, {2});                    // snare
    set_chain(4, {7});                    // hats
    set_chain(5, {8, 8, 9, 8});           // pad Am Am F Am

    // === SONG ===
    auto put = [&](int row, int track, int chain) {
        song.rows[row].chain[track] = (uint8_t)chain;
    };
    // r0: intro - cowbell + hats (the hook first, memphis style)
    put(0, 2, 2); put(0, 4, 4);
    // r1: + kick + 808
    put(1, 0, 0); put(1, 1, 1); put(1, 2, 2); put(1, 4, 4);
    // r2: + snare = full ride
    put(2, 0, 0); put(2, 1, 1); put(2, 2, 2); put(2, 3, 3); put(2, 4, 4);
    // r3: full + pad murk
    put(3, 0, 0); put(3, 1, 1); put(3, 2, 2); put(3, 3, 3); put(3, 4, 4); put(3, 5, 5);
    // r4: breakdown - 808 + pad + hats (cowbell rests, tension)
    put(4, 1, 1); put(4, 4, 4); put(4, 5, 5);
    // r5: full return
    put(5, 0, 0); put(5, 1, 1); put(5, 2, 2); put(5, 3, 3); put(5, 4, 4); put(5, 5, 5);

    // === WRITE ===
    std::printf("sizeof(Project) = %zu, version %u\n", sizeof(Project), PROJECT_VERSION);
    FILE* f = std::fopen("/tmp/gravehill.tr3d", "wb");
    if (!f) { std::perror("open"); return 1; }
    ProjectFileHeader h{};
    h.magic = PROJECT_MAGIC; h.version = PROJECT_VERSION; h.project_size = sizeof(Project);
    std::fwrite(&h, sizeof(h), 1, f);
    std::fwrite(&proj, sizeof(Project), 1, f);
    std::fclose(f);
    std::printf("wrote /tmp/gravehill.tr3d\n");
    return 0;
}
