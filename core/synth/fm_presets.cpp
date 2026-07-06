#include "fm_presets.h"

namespace trackr::synth {

// helper: build operators in one line
static FmOpParams op(uint8_t r, uint8_t lvl, uint16_t a, uint16_t d, uint8_t s, uint16_t rel) {
    FmOpParams o;
    o.ratio_idx = r;
    o.level     = lvl;
    o.attack    = a;
    o.decay     = d;
    o.sustain   = s;
    o.release   = rel;
    return o;
}

// === presets ===
// ratio_idx -> table: 0=0.5, 1=1.0, 2=1.5, 3=2.0, 4=2.5, 5=3.0, 6=4.0, 7=5.0,
//                      8=6.0, 9=7.0, 10=8.0, 11=9.0, 12=10, 13=12, 14=14, 15=16

static void preset_ep(FmSynthParams& p) {
    // algo 1 PAIR: 1→2, 3→4 (carriers 2,4)
    // op1 = bell-modulator, op2 = main carrier
    // op3 = high tine modulator, op4 = body carrier
    p.algorithm = 1;
    p.feedback  = 2;
    p.master_volume = 110;
    p.ops[0] = op(1,  80, 0,  5000, 0,  2000);   // op1 mod, q decay
    p.ops[1] = op(1, 100, 0,  8000, 0,  3000);   // op2 carrier
    p.ops[2] = op(13, 30, 0,  1500, 0,   500);   // op3 high tine
    p.ops[3] = op(1,  90, 0, 12000, 80, 4000);   // op4 body
}

static void preset_bass(FmSynthParams& p) {
    // algo 2 Y-FAN: 1 modulates 2,3,4
    p.algorithm = 2;
    p.feedback  = 4;
    p.master_volume = 120;
    p.ops[0] = op(1,  90, 0, 3000, 70, 2000);   // op1 modulator (sub-octave undertone)
    p.ops[1] = op(1, 100, 0,10000, 90, 1000);   // op2 carrier — fundamental
    p.ops[2] = op(0,  70, 0,10000, 90, 1000);   // op3 carrier — sub
    p.ops[3] = op(3,  35, 0, 5000, 50, 1000);   // op4 carrier — overtone bite
}

static void preset_lead(FmSynthParams& p) {
    // algo 3 3-into-1: 1,2,3 → 4
    p.algorithm = 3;
    p.feedback  = 3;
    p.master_volume = 110;
    p.ops[0] = op(1,  80, 0, 3000, 80, 1000);
    p.ops[1] = op(5,  60, 0, 4000, 40, 1000);
    p.ops[2] = op(7,  40, 0, 2000, 30,  800);
    p.ops[3] = op(1, 100, 0, 8000, 90, 2000);   // carrier
}

static void preset_bell(FmSynthParams& p) {
    // algo 0 STACK: 1→2→3→4 (carrier=4)
    p.algorithm = 0;
    p.feedback  = 0;
    p.master_volume = 100;
    p.ops[0] = op(9,  60, 0,  8000, 0, 4000);
    p.ops[1] = op(5,  70, 0, 10000, 0, 4000);
    p.ops[2] = op(1,  80, 0, 12000, 0, 5000);
    p.ops[3] = op(1, 100, 0, 16000, 0, 8000);   // carrier
}

static void preset_brass(FmSynthParams& p) {
    // algo 4 TREE: 1→2,3, 2→4 (carriers 3,4)
    p.algorithm = 4;
    p.feedback  = 5;
    p.master_volume = 105;
    p.ops[0] = op(1,  60, 200, 4000, 70, 1500);
    p.ops[1] = op(3,  50, 100, 4000, 60, 1500);
    p.ops[2] = op(1, 100, 200, 6000, 90, 2000);   // carrier
    p.ops[3] = op(1,  90, 150, 6000, 85, 2000);   // carrier
}

static void preset_pluck(FmSynthParams& p) {
    // algo 1 PAIR with a fast decay
    p.algorithm = 1;
    p.feedback  = 1;
    p.master_volume = 115;
    p.ops[0] = op(3,  80, 0, 1500, 0, 200);
    p.ops[1] = op(1, 100, 0, 3000, 0, 500);   // carrier
    p.ops[2] = op(6,  60, 0, 1000, 0, 200);
    p.ops[3] = op(1,  80, 0, 4000, 0, 500);   // carrier
}

static void preset_organ(FmSynthParams& p) {
    // algo 6 ADD: all 4 carriers as additive
    p.algorithm = 6;
    p.feedback  = 0;
    p.master_volume = 90;
    p.ops[0] = op(1, 100, 50, 1000, 127, 800);   // fundamental
    p.ops[1] = op(3,  80, 50, 1000, 127, 800);   // 2nd harmonic
    p.ops[2] = op(5,  60, 50, 1000, 127, 800);   // 3rd
    p.ops[3] = op(6,  50, 50, 1000, 127, 800);   // 4th
}

static void preset_pad(FmSynthParams& p) {
    // algo 1 PAIR with a slow attack
    p.algorithm = 1;
    p.feedback  = 2;
    p.master_volume = 95;
    p.ops[0] = op(1,  50,  8000,  8000, 80, 12000);
    p.ops[1] = op(1, 100,  8000, 10000,100, 15000);   // carrier
    p.ops[2] = op(2,  40, 12000,  8000, 70, 12000);
    p.ops[3] = op(1,  90, 12000, 10000,100, 15000);   // carrier
}

static void preset_stab(FmSynthParams& p) {
    // algo 4 TREE — chord stab
    p.algorithm = 4;
    p.feedback  = 4;
    p.master_volume = 110;
    p.ops[0] = op(1,  70, 0, 1500, 60, 500);
    p.ops[1] = op(7,  40, 0,  800,  0, 200);
    p.ops[2] = op(1, 100, 0, 4000, 70, 800);   // carrier
    p.ops[3] = op(1,  90, 0, 4000, 70, 800);   // carrier
}

static void preset_wood(FmSynthParams& p) {
    // algo 0 STACK with a very fast decay -> marimba
    p.algorithm = 0;
    p.feedback  = 0;
    p.master_volume = 110;
    p.ops[0] = op(6,  70, 0,  800, 0, 100);
    p.ops[1] = op(1,  80, 0, 1500, 0, 200);
    p.ops[2] = op(3,  60, 0, 1500, 0, 200);
    p.ops[3] = op(1, 100, 0, 2000, 0, 300);   // carrier
}

static void preset_wah(FmSynthParams& p) {
    // algo 5 STACK+aux: 1→2→3, 4 standalone
    p.algorithm = 5;
    p.feedback  = 6;     // high feedback = talkative wah
    p.master_volume = 105;
    p.ops[0] = op(1,  50, 200, 3000, 80, 1500);
    p.ops[1] = op(5,  80, 100, 3000, 70, 1500);
    p.ops[2] = op(1, 100,  50, 5000, 80, 1500);   // carrier
    p.ops[3] = op(0,  80,  50, 5000, 80, 1500);   // standalone carrier
}

static void preset_ice(FmSynthParams& p) {
    // algo 7 BRANCH: 1->3, 2->3, 4 standalone - glassy texture
    p.algorithm = 7;
    p.feedback  = 0;
    p.master_volume = 100;
    p.ops[0] = op(9,  50, 0, 4000,  0, 2000);
    p.ops[1] = op(13, 40, 0, 3000,  0, 1500);
    p.ops[2] = op(1, 100, 0, 8000, 30, 4000);   // carrier
    p.ops[3] = op(3,  50,100, 6000, 40, 3000);   // standalone carrier
}

// === dispatcher ===
const char* fm_preset_name(FmPreset p) {
    switch (p) {
        case FmPreset::EP:    return "EP";
        case FmPreset::Bass:  return "BASS";
        case FmPreset::Lead:  return "LEAD";
        case FmPreset::Bell:  return "BELL";
        case FmPreset::Brass: return "BRASS";
        case FmPreset::Pluck: return "PLUCK";
        case FmPreset::Organ: return "ORGAN";
        case FmPreset::Pad:   return "PAD";
        case FmPreset::Stab:  return "STAB";
        case FmPreset::Wood:  return "WOOD";
        case FmPreset::Wah:   return "WAH";
        case FmPreset::Ice:   return "ICE";
        default: return "?";
    }
}

void fm_load_preset(FmSynthParams& dst, FmPreset p) {
    switch (p) {
        case FmPreset::EP:    preset_ep(dst);    break;
        case FmPreset::Bass:  preset_bass(dst);  break;
        case FmPreset::Lead:  preset_lead(dst);  break;
        case FmPreset::Bell:  preset_bell(dst);  break;
        case FmPreset::Brass: preset_brass(dst); break;
        case FmPreset::Pluck: preset_pluck(dst); break;
        case FmPreset::Organ: preset_organ(dst); break;
        case FmPreset::Pad:   preset_pad(dst);   break;
        case FmPreset::Stab:  preset_stab(dst);  break;
        case FmPreset::Wood:  preset_wood(dst);  break;
        case FmPreset::Wah:   preset_wah(dst);   break;
        case FmPreset::Ice:   preset_ice(dst);   break;
        default: break;
    }
}

} // namespace trackr::synth
