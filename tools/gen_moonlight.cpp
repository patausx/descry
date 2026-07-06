// host-side generator: MOONLIGHT SONATA (Beethoven op.27 no.2, 1st movt,
// bars 1-8) -> slot 05.
// build: g++ -std=c++17 -I. tools/gen_moonlight.cpp -o /tmp/genmoon
//
// the whole movement is triplets: song.groove=8 makes one step = 8 ticks
// = a triplet eighth (quarter = 24 ticks = 3 steps). every phrase LEN=12
// = exactly one 4/4 bar of triplets. adagio ~54 bpm.
// senza sordini (pedal throughout) = poly instruments + fat reverb, no delay.
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
static void note(int ph, int st, int n, int inst, int vel = 0x40) {
    auto& s = step(ph, st);
    s.note = (uint8_t)n; s.instrument = (uint8_t)inst; s.velocity = (uint8_t)vel;
}
// one bar of triplet arpeggio: 4 beats x 3 notes. tri[beat][i] = midi note.
static void arp_bar(int ph, const int tri[4][3], int inst) {
    P->phrases[ph].length = 12;
    for (int b = 0; b < 4; ++b)
        for (int i = 0; i < 3; ++i)
            note(ph, b * 3 + i, tri[b][i], inst, (i == 0) ? 0x36 : 0x2C);
}

int main() {
    static Project proj;
    P = &proj;
    std::snprintf(proj.name, sizeof(proj.name), "MOONLIGHT");

    auto& song = proj.song;
    song.bpm = 54;          // adagio sostenuto
    song.groove = 8;        // 8 ticks/step -> step = triplet eighth
    song.scale_root = 1;    // C#
    song.scale_type = 3;    // harmonic minor (the B# is real)
    song.dly_wet = 30;      // barely any delay - pedal, not echo
    song.dly_fb  = 60;
    song.rev_wet = 135;     // senza sordini (170 overloaded the comb loop - 12 ringing
                            // poly voices with long decays saturate the tail)
    song.duck_src = 0xFF;
    uint8_t vols[8] = { 200, 190, 210, 0, 0, 0, 0, 0 };
    std::memcpy(song.track_vol, vols, 8);

    // === INSTRUMENTS ===
    // I00 PEARL - the triplet arp. soft FM piano, poly = pedal blur.
    {
        auto& I = proj.instruments[0];
        I.type = InstrumentType::FmSynth;
        std::snprintf(I.name, sizeof(I.name), "PEARL");
        I.poly = true;                                   // notes ring into each other
        I.fm = FmSynthParams{};
        auto& m = I.fm;
        m.algorithm = 1;    // PAIR: 1->2, 3->4
        m.feedback = 1;
        m.master_volume = 95;
        m.ops[0] = { 5, 34, 60, 4200, 0, 0, 1800 };      // r3.0 mod - hammer warmth, fades
        m.ops[1] = { 1, 112, 90, 15000, 0, 0, 6500 };    // r1.0 carrier (2.8ms attack - a hammer,
                                                         // not a wall: 10-frame attacks read as
                                                         // clicks on the scope and the ear)
        m.ops[2] = { 3, 18, 60, 2600, 0, 0, 1200 };      // r2.0 faint octave sheen
        m.ops[3] = { 0, 42, 110, 16000, 0, 0, 7000 };    // r0.5 sub warmth
        I.fx_send_rev = 70;
    }
    // I01 UNDERTOW - bass octaves in one voice (DSN vco2 an octave DOWN)
    {
        auto& I = proj.instruments[1];
        I.type = InstrumentType::DsnSynth;
        std::snprintf(I.name, sizeof(I.name), "UNDERTOW");
        I.dsn = DsnSynthParams{};
        auto& d = I.dsn;
        d.vco1_wave = DsnWave::Tri;
        d.vco2_wave = DsnWave::Tri; d.vco2_semi = -12;   // the left-hand octave pair
        d.balance = fx::Q15_ONE / 2;
        d.vcf_type = 0;
        d.cutoff = (fx::q15)(fx::Q15_ONE * 35 / 100);
        d.eg1_attack = 60; d.eg1_decay = 22000;
        d.eg1_sustain = (fx::q15)(fx::Q15_ONE * 55 / 100);
        d.eg1_release = 14000;
        d.vca_mode = 1;
        I.fx_send_rev = 70;
    }
    // I02 LUNA - the melody voice, slightly brighter, sings above
    {
        auto& I = proj.instruments[2];
        I.type = InstrumentType::FmSynth;
        std::snprintf(I.name, sizeof(I.name), "LUNA");
        I.poly = true;                                   // held G# rings under the dotted repeat
        I.fm = FmSynthParams{};
        auto& m = I.fm;
        m.algorithm = 1;
        m.feedback = 1;
        m.master_volume = 110;
        m.ops[0] = { 3, 46, 70, 6000, 0, 0, 2400 };      // r2.0 mod - presence
        m.ops[1] = { 1, 115, 100, 19000, 0, 0, 9000 };   // r1.0 carrier, 3ms hammer (decay-only)
        m.ops[2] = { 8, 14, 70, 2000, 0, 0, 1000 };      // r6.0 faint air
        m.ops[3] = { 1, 30, 100, 17000, 0, 0, 8000 };    // unison reinforcement
        I.fx_send_rev = 85;
    }

    // === NOTES ===
    // G#2=44 B#2=48 C#3=49 D3=50 D#3=51 E3=52 F#3=54 A2=45
    // bass: C#2=37 B1=35 A1=33 G#1=32 B#1=36 (each doubled -12 by the DSN)
    // melody: G#4=68 A4=69

    // --- arp phrases (one bar each) ---
    const int BAR_CSM[4][3]  = {{44,49,52},{44,49,52},{44,49,52},{44,49,52}};   // C#m
    const int BAR_3[4][3]    = {{45,49,52},{45,49,52},{45,50,54},{45,50,54}};   // A | D/F# colour
    const int BAR_4[4][3]    = {{44,48,54},{44,48,54},{44,49,52},{44,48,51}};   // G#7 | C#m/G# | G#7
    const int BAR_6[4][3]    = {{44,48,54},{44,48,54},{44,48,54},{44,48,54}};   // G#7/B#
    const int BAR_7[4][3]    = {{45,49,52},{45,49,52},{45,49,52},{45,49,52}};   // A
    const int BAR_8[4][3]    = {{44,48,51},{44,48,54},{44,48,51},{44,48,54}};   // G#7 vamp -> loop
    arp_bar(1, BAR_CSM, 0);
    arp_bar(2, BAR_3,   0);
    arp_bar(3, BAR_4,   0);
    arp_bar(4, BAR_6,   0);
    arp_bar(5, BAR_7,   0);
    arp_bar(6, BAR_8,   0);

    // --- bass phrases: one octave-pair per bar, rings the whole bar ---
    auto bass_bar = [&](int ph, int n) {
        P->phrases[ph].length = 12;
        note(ph, 0, n, 1, 0x46);
    };
    bass_bar(7, 37);   // C#
    bass_bar(8, 35);   // B
    bass_bar(9, 33);   // A
    bass_bar(10, 32);  // G#
    bass_bar(11, 36);  // B# (bar 6 - the dominant lean)

    // --- melody phrases ---
    // bar 5/6 figure: G# on beat 1, dotted-8th+16th G#-G# on beat 4
    // (in triplet steps: dotted = 2 steps, 16th = 1 -> hits at 9 and 11)
    {
        P->phrases[12].length = 12;
        note(12, 0, 68, 2, 0x52);
        note(12, 9, 68, 2, 0x3E);
        note(12, 11, 68, 2, 0x48);
    }
    // bar 7: A sings, answers down to G#
    {
        P->phrases[13].length = 12;
        note(13, 0, 69, 2, 0x54);
        note(13, 6, 68, 2, 0x4A);
    }
    // bar 8: G# held over the dominant - hangs, loop resolves it to bar 1
    {
        P->phrases[14].length = 12;
        note(14, 0, 68, 2, 0x50);
    }
    // empty 12-step bar for the melody's tacet bars 1-4 (keeps the 12-grid sync;
    // a truly empty song cell would tick 16 steps and drift the track)
    P->phrases[15].length = 12;

    // === CHAINS ===
    auto set_chain = [&](int c, std::initializer_list<int> phs) {
        int r = 0;
        for (int p : phs) P->chains[c].rows[r++].phrase = (uint8_t)p;
    };
    set_chain(0, {1, 1, 2, 3});           // arp bars 1-4
    set_chain(1, {1, 4, 5, 6});           // arp bars 5-8
    set_chain(2, {7, 8, 9, 10});          // bass bars 1-4: C# B A G#
    set_chain(3, {7, 11, 9, 10});         // bass bars 5-8: C# B# A G#
    set_chain(4, {15, 15, 15, 15});       // melody tacet (bars 1-4)
    set_chain(5, {12, 12, 13, 14});       // melody bars 5-8

    // === SONG: two rows = bars 1-4, 5-8; loops seamlessly on the G#7 hang ===
    auto put = [&](int row, int track, int chain) {
        song.rows[row].chain[track] = (uint8_t)chain;
    };
    put(0, 0, 2); put(0, 1, 0); put(0, 2, 4);
    put(1, 0, 3); put(1, 1, 1); put(1, 2, 5);

    // === WRITE ===
    std::printf("sizeof(Project) = %zu, version %u\n", sizeof(Project), PROJECT_VERSION);
    FILE* f = std::fopen("/tmp/moonlight.tr3d", "wb");
    if (!f) { std::perror("open"); return 1; }
    ProjectFileHeader h{};
    h.magic = PROJECT_MAGIC; h.version = PROJECT_VERSION; h.project_size = sizeof(Project);
    std::fwrite(&h, sizeof(h), 1, f);
    std::fwrite(&proj, sizeof(Project), 1, f);
    std::fclose(f);
    std::printf("wrote /tmp/moonlight.tr3d\n");
    return 0;
}
