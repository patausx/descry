// App: instrument editor screen (Wavsynth / Sampler / DrumKit / FmSynth).
// Split out of app.cpp. Owns update_instrument + draw_instrument.
#include "../app.h"
#include "../ui_internal.h"
#include "../../audio/fixed.h"
#include "../../synth/drum_gen.h"
#include "../../synth/fm_presets.h"
#include "../../synth/dsn_presets.h"
#include "../../synth/wave_presets.h"
#include "../../synth/wavetable.h"
#include "../../synth/wav_loader.h"
#include "../../synth/sample_utils.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>

namespace trackr::ui {

// helper shared by all TYPE rows: change instrument type + reinit the union.
static void set_inst_type(seq::Instrument& inst, int t) {
    if (t < 0) t = 0;
    if (t > seq::INSTRUMENT_TYPE_COUNT - 1) t = seq::INSTRUMENT_TYPE_COUNT - 1;
    if ((seq::InstrumentType)t == inst.type) return;
    inst.type = (seq::InstrumentType)t;
    switch (inst.type) {
        case seq::InstrumentType::Wavsynth: inst.wavsynth = synth::WavsynthParams{}; break;
        case seq::InstrumentType::Sampler:  inst.sampler  = synth::SamplerParams{};  break;
        case seq::InstrumentType::DrumKit:  inst.drumkit  = synth::DrumKitParams{};  break;
        case seq::InstrumentType::FmSynth:  inst.fm       = synth::FmSynthParams{};  break;
        case seq::InstrumentType::DsnSynth: inst.dsn      = synth::DsnSynthParams{}; break;
        default: break;
    }
}

// === DSN instrument layout (M8-style THREE-column param list) ===
// 37 rows in three columns (13/12/12): voice | envelopes | mod+amp.
enum DsnRow {
    // col 0 - voice (13)
    DR_TYPE = 0, DR_PRESET, DR_OCT, DR_VCO1_WAVE, DR_VCO1_PW, DR_VCO2_WAVE,
    DR_VCO2_SEMI, DR_VCO2_DET, DR_SYNC, DR_BALANCE, DR_CUTOFF, DR_RES, DR_FILT_TYPE,
    // col 1 - envelopes (12)
    DR_EG1_A, DR_EG1_D, DR_EG1_S, DR_EG1_R, DR_EG1_PIT, DR_EG1_CUT,
    DR_EG2_A, DR_EG2_D, DR_EG2_S, DR_EG2_R, DR_EG2_PIT, DR_EG2_CUT,
    // col 2 - mod + amp (12)
    DR_MG1_WAVE, DR_MG1_RATE, DR_MG1_PIT, DR_MG1_CUT,
    DR_MG2_WAVE, DR_MG2_RATE, DR_MG2_PW, DR_MG2_VCA,
    DR_PORTA, DR_VCA_MODE, DR_VCA_LVL, DR_DRIVE,
    DSN_ROWS
};
// column boundaries for nav + draw
static const int kDsnColStart[3] = { 0, 13, 25 };
static const int kDsnColLen[3]   = { 13, 12, 12 };
static inline int dsn_col_of(int r) { return r < 13 ? 0 : (r < 25 ? 1 : 2); }
static const char* const kDsnRowNames[DSN_ROWS] = {
    "TYPE  ", "PRESET", "OCT   ", "OSC1  ", "PW    ", "OSC2  ",
    "SEMI  ", "DETUN ", "SYNC  ", "BAL   ", "CUT   ", "RES   ", "FILT  ",
    "EG1-A ", "EG1-D ", "EG1-S ", "EG1-R ", "E1>PIT", "E1>CUT",
    "EG2-A ", "EG2-D ", "EG2-S ", "EG2-R ", "E2>PIT", "E2>CUT",
    "MG1   ", "MG1RT ", "M1>PIT", "M1>CUT",
    "MG2   ", "MG2RT ", "M2>PW ", "M2>VCA",
    "PORTA ", "VCA   ", "VCALVL", "DRIVE ",
};

// edit one row of the DSN instrument.
void App::edit_dsn_row(synth::DsnSynthParams& dp, seq::Instrument& inst, int delta) {
    const int32_t Q = fx::Q15_ONE;
    const int32_t STEP = Q / 32;              // ~3% per tick
    auto clampq = [](int v) { if (v < 0) v = 0; if (v > fx::Q15_ONE) v = fx::Q15_ONE; return (fx::q15)v; };
    auto clamps = [](int v) { if (v < -fx::Q15_ONE) v = -fx::Q15_ONE; if (v > fx::Q15_ONE) v = fx::Q15_ONE; return (int16_t)v; };
    switch (inst_row_) {
        case DR_TYPE:
            set_inst_type(inst, (int)inst.type + delta);
            inst_row_ = 0;
            break;
        case DR_PRESET: {
            int p = dsn_preset_idx_ + delta;
            while (p < 0) p += synth::DSN_PRESET_COUNT;
            p %= synth::DSN_PRESET_COUNT;
            dsn_preset_idx_ = p;
            synth::dsn_load_preset(dp, (synth::DsnPreset)p);
            std::snprintf(inst.name, sizeof(inst.name), "%s",
                          synth::dsn_preset_name((synth::DsnPreset)p));
            break;
        }
        case DR_OCT: {
            int v = dp.octave + (delta > 0 ? 1 : -1);
            if (v < -2) v = -2; if (v > 2) v = 2;
            dp.octave = (int8_t)v;
            break;
        }
        case DR_VCO1_WAVE: {
            int v = (int)dp.vco1_wave + (delta > 0 ? 1 : -1);
            if (v < 0) v = 4; if (v > 4) v = 0;
            dp.vco1_wave = (synth::DsnWave)v;
            break;
        }
        case DR_VCO1_PW:  dp.vco1_pw = clampq((int)dp.vco1_pw + delta * STEP); break;
        case DR_VCO2_WAVE: {
            int v = (int)dp.vco2_wave + (delta > 0 ? 1 : -1);
            if (v < 0) v = 4; if (v > 4) v = 0;
            dp.vco2_wave = (synth::DsnWave)v;
            break;
        }
        case DR_VCO2_SEMI: {
            int v = dp.vco2_semi + (delta > 0 ? 1 : -1);
            if (v < -24) v = -24; if (v > 24) v = 24;
            dp.vco2_semi = (int8_t)v;
            break;
        }
        case DR_VCO2_DET: {
            int v = dp.vco2_detune + (delta > 0 ? 1 : -1);
            if (v < -50) v = -50; if (v > 50) v = 50;
            dp.vco2_detune = (int8_t)v;
            break;
        }
        case DR_SYNC:    dp.vco2_sync = dp.vco2_sync ? 0 : 1; break;
        case DR_BALANCE: dp.balance = clampq((int)dp.balance + delta * STEP); break;
        case DR_CUTOFF:  dp.cutoff  = clampq((int)dp.cutoff + delta * STEP); break;
        case DR_RES:     dp.resonance = clampq((int)dp.resonance + delta * STEP); break;
        case DR_FILT_TYPE: {
            int v = (int)dp.vcf_type + (delta > 0 ? 1 : -1);
            if (v < 0) v = 4; if (v > 4) v = 0;
            dp.vcf_type = (uint8_t)v;
            break;
        }
        case DR_PORTA: {
            int v = dp.portamento + delta;
            if (v < 0) v = 0; if (v > 127) v = 127;
            dp.portamento = (uint8_t)v;
            break;
        }
        case DR_EG1_A: { int v = (int)dp.eg1_attack  + delta * 100; if (v < 0) v = 0; dp.eg1_attack  = (uint32_t)v; break; }
        case DR_EG1_D: { int v = (int)dp.eg1_decay   + delta * 200; if (v < 0) v = 0; dp.eg1_decay   = (uint32_t)v; break; }
        case DR_EG1_S: dp.eg1_sustain = clampq((int)dp.eg1_sustain + delta * STEP); break;
        case DR_EG1_R: { int v = (int)dp.eg1_release + delta * 200; if (v < 0) v = 0; dp.eg1_release = (uint32_t)v; break; }
        case DR_EG1_PIT: dp.eg1_to_pitch  = clamps((int)dp.eg1_to_pitch  + delta * STEP); break;
        case DR_EG1_CUT: dp.eg1_to_cutoff = clamps((int)dp.eg1_to_cutoff + delta * STEP); break;
        case DR_EG2_A: { int v = (int)dp.eg2_attack  + delta * 100; if (v < 0) v = 0; dp.eg2_attack  = (uint32_t)v; break; }
        case DR_EG2_D: { int v = (int)dp.eg2_decay   + delta * 200; if (v < 0) v = 0; dp.eg2_decay   = (uint32_t)v; break; }
        case DR_EG2_S: dp.eg2_sustain = clampq((int)dp.eg2_sustain + delta * STEP); break;
        case DR_EG2_R: { int v = (int)dp.eg2_release + delta * 200; if (v < 0) v = 0; dp.eg2_release = (uint32_t)v; break; }
        case DR_EG2_PIT: dp.eg2_to_pitch  = clamps((int)dp.eg2_to_pitch  + delta * STEP); break;
        case DR_EG2_CUT: dp.eg2_to_cutoff = clamps((int)dp.eg2_to_cutoff + delta * STEP); break;
        case DR_MG1_WAVE: {
            int v = (int)dp.mg1_wave + (delta > 0 ? 1 : -1);
            if (v < 0) v = 3; if (v > 3) v = 0;
            dp.mg1_wave = (uint8_t)v;
            break;
        }
        case DR_MG1_RATE: dp.mg1_rate = clampq((int)dp.mg1_rate + delta * STEP); break;
        case DR_MG1_PIT:  dp.mg1_to_pitch  = clamps((int)dp.mg1_to_pitch  + delta * STEP); break;
        case DR_MG1_CUT:  dp.mg1_to_cutoff = clamps((int)dp.mg1_to_cutoff + delta * STEP); break;
        case DR_MG2_WAVE: {
            int v = (int)dp.mg2_wave + (delta > 0 ? 1 : -1);
            if (v < 0) v = 3; if (v > 3) v = 0;
            dp.mg2_wave = (uint8_t)v;
            break;
        }
        case DR_MG2_RATE: dp.mg2_rate = clampq((int)dp.mg2_rate + delta * STEP); break;
        case DR_MG2_PW:   dp.mg2_to_pw  = clamps((int)dp.mg2_to_pw  + delta * STEP); break;
        case DR_MG2_VCA:  dp.mg2_to_vca = clamps((int)dp.mg2_to_vca + delta * STEP); break;
        case DR_VCA_MODE: dp.vca_mode = dp.vca_mode ? 0 : 1; break;
        case DR_VCA_LVL:  dp.vca_level = clampq((int)dp.vca_level + delta * STEP); break;
        case DR_DRIVE:    dp.drive = clampq((int)dp.drive + delta * STEP); break;
    }
}

// === Sampler instrument layout (M8-style param list) ===
// row indices for the Sampler instrument editor.
enum SamplerRow {
    SR_TYPE = 0, SR_SAMPLE, SR_PLAY, SR_SLICE, SR_START, SR_LENGTH,
    SR_LOOP_S, SR_LOOP_E, SR_DETUNE, SR_ATTACK, SR_RELEASE, SR_TABLE,
    SAMPLER_ROWS
};
static const char* const kSamplerRowNames[SAMPLER_ROWS] = {
    "TYPE  ", "SAMPLE", "PLAY  ", "SLICE ", "START ", "LENGTH",
    "LOOP-S", "LOOP-E", "DETUNE", "ATK   ", "REL   ", "TABLE ",
};


// edit one row of the Sampler instrument (M8-style param list).
void App::edit_sampler_row(synth::SamplerParams& sp, seq::Instrument& inst, int delta) {
    auto& s = synth::SampleBank::instance().slot(sp.sample_slot);
    const int32_t Q = fx::Q15_ONE;            // 32767
    const int32_t STEP = Q / 64;              // ~1.5% per A/B tick; X/Y = 16x
    switch (inst_row_) {
        case SR_TYPE: {
            set_inst_type(inst, (int)inst.type + delta);
            inst_row_ = 0;
            break;
        }
        case SR_SAMPLE: {
            int v = sp.sample_slot + delta;
            if (v < 0) v = 0;
            if (v >= synth::SAMPLE_BANK_SIZE) v = synth::SAMPLE_BANK_SIZE - 1;
            sp.sample_slot = v;
            cur_sample_ = (uint8_t)v;
            break;
        }
        case SR_PLAY: {
            int v = (int)sp.play_mode + (delta > 0 ? 1 : (delta < 0 ? -1 : 0));
            int n = (int)synth::PlayMode::Count;
            if (v < 0) v = n - 1;
            if (v >= n) v = 0;
            sp.play_mode = (synth::PlayMode)v;
            break;
        }
        case SR_SLICE: {
            // 0 = whole / chromatic toggle at the top; 1..MAX = fixed slice
            int v = (int)sp.slice + (delta > 0 ? 1 : (delta < 0 ? -1 : 0));
            if (v < 0) { v = 0; sp.chromatic_slices = !sp.chromatic_slices; }
            if (v > synth::Sample::MAX_CHOPS) v = synth::Sample::MAX_CHOPS;
            sp.slice = (uint8_t)v;
            break;
        }
        case SR_START: {
            int v = (int)sp.start + delta * STEP;
            if (v < 0) v = 0;
            if (v > Q) v = Q;
            sp.start = (fx::q15)v;
            break;
        }
        case SR_LENGTH: {
            int v = (int)sp.length + delta * STEP;
            if (v < 0) v = 0;
            if (v > Q) v = Q;
            sp.length = (fx::q15)v;
            break;
        }
        case SR_LOOP_S: {
            int v = (int)s.loop_start + delta * 64;
            if (v < 0) v = 0;
            if (v > (int)s.num_frames()) v = s.num_frames();
            s.loop_start = (uint32_t)v;
            break;
        }
        case SR_LOOP_E: {
            int v = (int)s.loop_end + delta * 64;
            if (v < 0) v = 0;
            if (v > (int)s.num_frames()) v = s.num_frames();
            s.loop_end = (uint32_t)v;
            break;
        }
        case SR_DETUNE: {
            int v = (int)sp.fine_cents + delta;
            if (v < -50) v = -50;
            if (v >  50) v =  50;
            sp.fine_cents = (int8_t)v;
            break;
        }
        case SR_ATTACK: {
            int v = (int)sp.attack + delta * 100;
            if (v < 0) v = 0;
            sp.attack = (uint32_t)v;
            break;
        }
        case SR_RELEASE: {
            int v = (int)sp.release + delta * 200;
            if (v < 0) v = 0;
            sp.release = (uint32_t)v;
            break;
        }
        case SR_TABLE: {
            int v = (int)inst.table_id + delta;
            if (v < -1) v = -1;
            if (v >= seq::MAX_TABLES) v = seq::MAX_TABLES - 1;
            inst.table_id = (v < 0) ? seq::EMPTY : (uint8_t)v;
            break;
        }
    }
}

// === bottom-screen M8-style slice editor panel ===
// drawn in the keyboard zone when inst_slice_panel_ is on (Sampler instrument).
// v2: fat action buttons + waveform with numbered chop tabs. all touch.
namespace {
    constexpr int SL_X = 4, SL_W = 312;
    constexpr int SLB_Y = 108, SLB_H = 18, SLB_W = 78;   // 4 action buttons
    constexpr int SL_Y = 134, SL_H = 76;                 // waveform box
    constexpr int SL_INFO = 218;
    static const char* const kSliceOps[4] = { "AUTO16", "DEL", "CLR", ">KIT" };
}

void App::draw_slice_panel(Draw& d, int slot) {
    auto& s = synth::SampleBank::instance().slot(slot);
    auto& sp = project_.instruments[cur_inst_].sampler;
    (void)sp;

    // === action buttons (fat, obviously tappable) ===
    for (int i = 0; i < 4; ++i) {
        int x = SL_X + i * SLB_W;
        ui_button(d, x, SLB_Y, SLB_W - 3, SLB_H, pal::BG_HI, pal::GRID,
                  kSliceOps[i], s.empty() ? pal::FG_DIM : pal::FG);
    }

    // === waveform box ===
    d.rect(SL_X - 2, SL_Y - 12, SL_W + 4, SL_H + 14, pal::BG_HI);
    d.rect(SL_X, SL_Y, SL_W, SL_H, pal::PANEL);
    d.rect(SL_X, SL_Y + SL_H / 2, SL_W, 1, pal::GRID);

    uint32_t total = s.empty() ? 0 : s.num_frames();
    auto f2x = [&](uint32_t f) -> int {
        if (total == 0) return SL_X;
        if (f > total) f = total;
        return SL_X + (int)((uint64_t)f * SL_W / total);
    };

    int cnt = 0;
    for (int i = 0; i < synth::Sample::MAX_CHOPS; ++i)
        if (s.chops[i] != 0xFFFFFFFFu) ++cnt;

    if (s.empty()) {
        d.text(SL_X + SL_W / 2 - 60, SL_Y + SL_H / 2 - 4, "EMPTY - REC WITH ZR", pal::FG_DIM);
    } else {
        // waveform (min/max per column, decimated)
        for (int x = 0; x < SL_W; ++x) {
            uint32_t a = ((uint64_t)x * total) / SL_W;
            uint32_t b = ((uint64_t)(x + 1) * total) / SL_W;
            if (b <= a) b = a + 1;
            if (b > total) b = total;
            int32_t mn = 32767, mx = -32768;
            std::size_t step = (b - a) / 4; if (step < 1) step = 1;
            for (std::size_t k = a; k < b; k += step) {
                int32_t v = s.data[k * s.channels];
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            int yt = SL_Y + SL_H / 2 - (mx * (SL_H / 2)) / 32768;
            int yb = SL_Y + SL_H / 2 - (mn * (SL_H / 2)) / 32768;
            if (yt < SL_Y) yt = SL_Y;
            if (yb >= SL_Y + SL_H) yb = SL_Y + SL_H - 1;
            if (yb < yt) { int t = yt; yt = yb; yb = t; }
            d.rect(SL_X + x, yt, 1, yb - yt + 1, pal::PLAY_BG);
        }

        // chop markers: numbered TAB at the top + full-height post.
        // selected = cursor color, breathing, fatter post.
        for (int i = 0; i < synth::Sample::MAX_CHOPS; ++i) {
            uint32_t cf = s.chops[i];
            if (cf == 0xFFFFFFFFu) continue;
            int cx = f2x(cf);
            bool sel = (i == smp_chop_sel_);
            ui::Color col;
            if (sel) {
                uint8_t br = breathe_pulse(frame_, 64);
                col = lerp_color(with_alpha(pal::CURSOR, 150), pal::CURSOR, br);
            } else col = pal::FG_HEX;
            // grab tab hanging from the top edge (9x10) with the hex number
            int tx = cx - 4; if (tx < SL_X) tx = SL_X;
            d.rect(tx, SL_Y - 11, 9, 10, sel ? col : pal::BG_HI);
            char nb[3]; std::snprintf(nb, sizeof(nb), "%X", i);
            d.text(tx + 2, SL_Y - 10, nb, sel ? pal::PANEL : col);
            // post
            d.rect(cx, SL_Y, 1, SL_H, col);
            if (sel) d.rect(cx + 1, SL_Y, 1, SL_H, with_alpha(col, 120));
        }

        // live playhead
        for (int t = 0; t < seq::NUM_TRACKS; ++t) {
            auto* v = mixer_.primary_voice(t);
            if (v && v->active() && v->current_sample_slot() == slot) {
                int pos = v->current_frame();
                if (pos >= 0 && pos < (int)total)
                    d.rect(f2x((uint32_t)pos), SL_Y - 2, 1, SL_H + 4, pal::FG);
            }
        }
    }

    // === info line: state on the left, gesture help on the right ===
    char ib[48];
    if (cnt > 0 && smp_chop_sel_ >= 0 && s.chops[smp_chop_sel_] != 0xFFFFFFFFu && total) {
        int pct = (int)((uint64_t)s.chops[smp_chop_sel_] * 100 / total);
        std::snprintf(ib, sizeof(ib), "%d CHOPS  SEL %X @%d%%", cnt, smp_chop_sel_, pct);
    } else {
        std::snprintf(ib, sizeof(ib), "%d CHOPS", cnt);
    }
    d.text(SL_X, SL_INFO, ib, pal::FG);
    d.text(SL_X + 150, SL_INFO, "TAP=ADD/SEL  DRAG=MOVE", pal::FG_DIM);
}

// touch handler for the slice panel. buttons row + tap/drag on the waveform.
void App::slice_panel_touch(int x, int y, int slot, bool is_move) {
    auto& s = synth::SampleBank::instance().slot(slot);

    // === action buttons (tap only) ===
    if (!is_move && y >= SLB_Y && y < SLB_Y + SLB_H) {
        int i = (x - SL_X) / SLB_W;
        if (i < 0 || i > 3 || s.empty()) return;
        uint32_t total = s.num_frames();
        switch (i) {
            case 0:   // AUTO16: even slice across the whole sample
                for (int k = 0; k < synth::Sample::MAX_CHOPS; ++k)
                    s.chops[k] = (uint32_t)((uint64_t)k * total / synth::Sample::MAX_CHOPS);
                smp_chop_sel_ = 0;
                break;
            case 1:   // DEL: remove the selected chop, select the next live one
                if (smp_chop_sel_ >= 0 && smp_chop_sel_ < synth::Sample::MAX_CHOPS) {
                    s.chops[smp_chop_sel_] = 0xFFFFFFFFu;
                    for (int k = 0; k < synth::Sample::MAX_CHOPS; ++k)
                        if (s.chops[k] != 0xFFFFFFFFu) { smp_chop_sel_ = k; break; }
                }
                break;
            case 2:   // CLR: wipe all chops
                for (int k = 0; k < synth::Sample::MAX_CHOPS; ++k) s.chops[k] = 0xFFFFFFFFu;
                smp_chop_sel_ = 0;
                break;
            case 3:   // >KIT: spread chops onto a DrumKit
                make_kit_from_sample(slot);
                break;
        }
        mark_dirty();
        return;
    }

    // === waveform zone ===
    if (y < SL_Y - 14 || y > SL_Y + SL_H + 8) return;
    if (s.empty()) return;
    uint32_t total = s.num_frames();
    if (total == 0) return;
    int cx = x - SL_X;
    if (cx < 0) cx = 0;
    if (cx > SL_W) cx = SL_W;
    uint32_t frame = (uint32_t)((uint64_t)cx * total / SL_W);

    if (is_move) {
        // drag the selected chop
        if (smp_chop_sel_ >= 0 && smp_chop_sel_ < synth::Sample::MAX_CHOPS &&
            s.chops[smp_chop_sel_] != 0xFFFFFFFFu) {
            s.chops[smp_chop_sel_] = frame;
            mark_dirty();
        }
        return;
    }

    // tap: find nearest existing chop within ~12px (or its top tab)
    int best = -1; uint32_t best_d = 0xFFFFFFFFu;
    for (int i = 0; i < synth::Sample::MAX_CHOPS; ++i) {
        uint32_t cf = s.chops[i];
        if (cf == 0xFFFFFFFFu) continue;
        int ckx = (int)((uint64_t)cf * SL_W / total);
        uint32_t dd = (uint32_t)std::abs(ckx - cx);
        if (dd < best_d) { best_d = dd; best = i; }
    }
    if (best >= 0 && best_d <= 12) {
        smp_chop_sel_ = best;            // select existing
    } else {
        // place into the first empty slot
        int put = -1;
        for (int i = 0; i < synth::Sample::MAX_CHOPS; ++i)
            if (s.chops[i] == 0xFFFFFFFFu) { put = i; break; }
        if (put >= 0) {
            s.chops[put] = frame;
            smp_chop_sel_ = put;
            mark_dirty();
        }
    }
}

// === bottom-screen WAVE panel: waveform + start/length drag + destructive ops ===
// v2: taller op buttons, marker GRAB HANDLES (shapes, not bare 1px lines):
//   start = green flag pointing right, end = terracotta flag pointing left,
//   loop points = ochre flags at the bottom. drag anywhere near a marker.
namespace {
    constexpr int WV_X = 4,  WV_W = 312;
    constexpr int WOP_Y = 108, WOP_H = 18, WOP_N = 8;
    constexpr int WOP_W = 39;   // 8*39=312
    constexpr int WV_Y = 134, WV_H = 76;
    constexpr int WV_INFO = 218;
    static const char* const kWaveOps[WOP_N] = {
        "NORM", "REV", "FAD<", "FAD>", "G+3", "G-3", "CROP", "COPY"
    };
}

void App::draw_wave_panel(Draw& d, int slot) {
    auto& s = synth::SampleBank::instance().slot(slot);
    auto& sp = project_.instruments[cur_inst_].sampler;

    // op buttons row (taller = tappable, tactile gradient)
    for (int i = 0; i < WOP_N; ++i) {
        int x = WV_X + i * WOP_W;
        ui_button(d, x, WOP_Y, WOP_W - 3, WOP_H, pal::BG_HI, pal::GRID,
                  kWaveOps[i], s.empty() ? pal::FG_DIM : pal::FG);
    }

    // waveform window
    d.rect(WV_X - 2, WV_Y - 2, WV_W + 4, WV_H + 4, pal::BG_HI);
    d.rect(WV_X, WV_Y, WV_W, WV_H, pal::PANEL);
    d.rect(WV_X, WV_Y + WV_H / 2, WV_W, 1, pal::GRID);

    uint32_t total = s.empty() ? 0 : s.num_frames();
    if (s.empty()) {
        d.text(WV_X + WV_W / 2 - 66, WV_Y + WV_H / 2 - 4, "EMPTY - LOAD OR REC", pal::FG_DIM);
        return;
    }

    // min/max column waveform (dim; the active window gets the bright pass)
    uint32_t sframe = ((uint64_t)sp.start * total) >> 15;
    uint32_t eframe = sframe + (((uint64_t)sp.length * total) >> 15);
    if (eframe > total) eframe = total;
    auto f2x = [&](uint32_t f) { return WV_X + (int)((uint64_t)f * WV_W / total); };
    int sx = f2x(sframe), ex = f2x(eframe);

    for (int x = 0; x < WV_W; ++x) {
        uint32_t a = ((uint64_t)x * total) / WV_W;
        uint32_t b = ((uint64_t)(x + 1) * total) / WV_W;
        if (b <= a) b = a + 1;
        if (b > total) b = total;
        int32_t mn = 32767, mx = -32768;
        std::size_t step = (b - a) / 4; if (step < 1) step = 1;
        for (std::size_t k = a; k < b; k += step) {
            int32_t v = s.data[k * s.channels];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        int yt = WV_Y + WV_H / 2 - (mx * (WV_H / 2)) / 32768;
        int yb = WV_Y + WV_H / 2 - (mn * (WV_H / 2)) / 32768;
        if (yt < WV_Y) yt = WV_Y;
        if (yb >= WV_Y + WV_H) yb = WV_Y + WV_H - 1;
        if (yb < yt) { int t = yt; yt = yb; yb = t; }
        // inside the play window = bright, outside = dim (no shading veils)
        bool inside = (WV_X + x >= sx && WV_X + x <= ex);
        d.rect(WV_X + x, yt, 1, yb - yt + 1, inside ? pal::PLAY : pal::PLAY_BG);
    }

    // === marker grab handles ===
    // start: green post + flag pointing INTO the window (right)
    d.rect(sx, WV_Y - 3, 1, WV_H + 6, pal::TRACK1);
    d.rect(sx, WV_Y - 3, 6, 5, pal::TRACK1);
    d.rect(sx + 1, WV_Y + 2, 4, 2, with_alpha(pal::TRACK1, 140));
    // end: terracotta post + flag pointing left
    d.rect(ex, WV_Y - 3, 1, WV_H + 6, pal::RECORD);
    d.rect(ex - 5, WV_Y - 3, 6, 5, pal::RECORD);
    d.rect(ex - 4, WV_Y + 2, 4, 2, with_alpha(pal::RECORD, 140));

    // loop markers: ochre posts with BOTTOM flags (visually distinct from s/e)
    const bool loops = (sp.play_mode == synth::PlayMode::FwdLoop ||
                        sp.play_mode == synth::PlayMode::RevLoop);
    if (s.loop_end > s.loop_start) {
        ui::Color lc = loops ? pal::HEADER : with_alpha(pal::HEADER, 110);
        int lsx = f2x(s.loop_start), lex = f2x(s.loop_end > total ? total : s.loop_end);
        d.rect(lsx, WV_Y, 1, WV_H, lc);
        d.rect(lex, WV_Y, 1, WV_H, lc);
        d.rect(lsx, WV_Y + WV_H - 4, 6, 4, lc);
        d.rect(lex - 5, WV_Y + WV_H - 4, 6, 4, lc);
    }

    // live playhead
    for (int t = 0; t < seq::NUM_TRACKS; ++t) {
        auto* v = mixer_.primary_voice(t);
        if (v && v->active() && v->current_sample_slot() == slot) {
            int pos = v->current_frame();
            if (pos >= 0 && pos < (int)total)
                d.rect(f2x((uint32_t)pos), WV_Y - 4, 1, WV_H + 8, pal::FG);
        }
    }

    // info line: seconds + root + window
    char ib[56];
    char rn[4];
    Draw::note_str((uint8_t)s.root_note, rn);
    int spct = (int)((uint64_t)sp.start * 100 / fx::Q15_ONE);
    int lpct = (int)((uint64_t)sp.length * 100 / fx::Q15_ONE);
    std::snprintf(ib, sizeof(ib), "%.2fs  ROOT %s (A/B)  WIN %d-%d%%",
                  total / 32000.0f, rn, spct, spct + lpct > 100 ? 100 : spct + lpct);
    d.text(WV_X, WV_INFO, ib, pal::FG);
    d.text(WV_X + 220, WV_INFO, "DRAG=MARKERS", pal::FG_DIM);
}

// touch: op buttons row OR drag nearest start/end marker.
void App::wave_panel_touch(int x, int y, int slot, bool is_move) {
    auto& s = synth::SampleBank::instance().slot(slot);
    if (s.empty()) return;
    auto& sp = project_.instruments[cur_inst_].sampler;
    uint32_t total = s.num_frames();
    if (total == 0) return;

    // op buttons (tap only, not drag)
    if (!is_move && y >= WOP_Y && y < WOP_Y + WOP_H) {
        int i = (x - WV_X) / WOP_W;
        if (i < 0 || i >= WOP_N) return;
        uint32_t sfr = ((uint64_t)sp.start * total) >> 15;
        uint32_t efr = sfr + (((uint64_t)sp.length * total) >> 15);
        // data-index range (frames * channels) for the fade ops
        uint32_t da = sfr * s.channels, db = efr * s.channels;
        switch (i) {
            case 0: synth::sample_normalize(s); break;
            case 1: synth::sample_reverse(s);   break;
            case 2: synth::sample_fade_in(s, da, db);  break;
            case 3: synth::sample_fade_out(s, da, db); break;
            case 4: synth::sample_gain_db(s, +3); break;
            case 5: synth::sample_gain_db(s, -3); break;
            case 6:   // CROP to the current start/length window, then reset window
                synth::sample_trim_norm(s, sp.start, sp.length);
                sp.start = 0;
                sp.length = fx::Q15_ONE;
                break;
            case 7: {  // COPY the window into the first free bank slot
                int free_slot = -1;
                for (int k = 0; k < synth::SAMPLE_BANK_SIZE; ++k) {
                    if (synth::SampleBank::instance().slot(k).data.empty()) { free_slot = k; break; }
                }
                if (free_slot >= 0 && efr > sfr) {
                    auto& dst = synth::SampleBank::instance().slot(free_slot);
                    dst.data.assign(s.data.begin() + (size_t)sfr * s.channels,
                                    s.data.begin() + (size_t)efr * s.channels);
                    dst.channels  = s.channels;
                    dst.root_note = s.root_note;
                    dst.loop_start = 0;
                    dst.loop_end   = 0;
                    for (int c = 0; c < synth::Sample::MAX_CHOPS; ++c) dst.chops[c] = 0xFFFFFFFFu;
                    std::snprintf(smp_status_, sizeof(smp_status_), "COPIED > SLOT %02d", free_slot);
                }
                break;
            }
        }
        mark_dirty();
        return;
    }

    // marker drag inside the waveform zone
    if (y < WV_Y - 8 || y > WV_Y + WV_H + 8) return;
    int cx = x - WV_X;
    if (cx < 0) cx = 0;
    if (cx > WV_W) cx = WV_W;
    uint32_t frame = (uint32_t)((uint64_t)cx * total / WV_W);
    // zero-crossing snap for click-free trims
    frame = synth::find_zero_crossing_near(s, frame);

    uint32_t sfr = ((uint64_t)sp.start * total) >> 15;
    uint32_t efr = sfr + (((uint64_t)sp.length * total) >> 15);
    // pick nearest of the 4 markers (start/end/loop_s/loop_e) on first touch;
    // during move keep dragging the same one via smp_drag_kind_ (1/2/3/4)
    if (!is_move) {
        const bool has_loop = (s.loop_end > s.loop_start);
        uint32_t ds = frame > sfr ? frame - sfr : sfr - frame;
        uint32_t de = frame > efr ? frame - efr : efr - frame;
        uint32_t dls = 0xFFFFFFFFu, dle = 0xFFFFFFFFu;
        if (has_loop) {
            dls = frame > s.loop_start ? frame - s.loop_start : s.loop_start - frame;
            dle = frame > s.loop_end   ? frame - s.loop_end   : s.loop_end - frame;
        }
        uint32_t best = ds; smp_drag_kind_ = 1;
        if (de  < best) { best = de;  smp_drag_kind_ = 2; }
        if (dls < best) { best = dls; smp_drag_kind_ = 3; }
        if (dle < best) { best = dle; smp_drag_kind_ = 4; }
    }
    switch (smp_drag_kind_) {
        case 1: {
            // move start, keep end fixed
            if (frame >= efr) frame = efr > 0 ? efr - 1 : 0;
            uint32_t new_len = efr - frame;
            sp.start  = (fx::q15)(((uint64_t)frame  << 15) / total);
            sp.length = (fx::q15)(((uint64_t)new_len << 15) / total);
            break;
        }
        case 2: {
            // move end, keep start fixed
            if (frame <= sfr) frame = sfr + 1;
            if (frame > total) frame = total;
            uint32_t new_len = frame - sfr;
            sp.length = (fx::q15)(((uint64_t)new_len << 15) / total);
            break;
        }
        case 3:
            // loop start (clamp below loop_end)
            if (frame >= s.loop_end && s.loop_end > 0) frame = s.loop_end - 1;
            s.loop_start = frame;
            break;
        case 4:
            // loop end (clamp above loop_start)
            if (frame <= s.loop_start) frame = s.loop_start + 1;
            if (frame > total) frame = total;
            s.loop_end = frame;
            break;
    }
    if (sp.length < 1) sp.length = 1;
    mark_dirty();
}

// === bottom-screen REC panel: bounce master output into this instrument's slot ===
// tap the big button (or press ZR as before) to arm/stop. reuses mixer resample.
void App::draw_rec_panel(Draw& d, int slot) {
    constexpr int PX = 4, PY = 116, PW = 312, PH = 122;
    d.rect(PX - 2, PY - 2, PW + 4, PH + 4, pal::BG_HI);
    d.rect(PX, PY, PW, PH, pal::BG);
    d.text(PX + 2, PY + 2, "REC TO SLOT", pal::HEADER);
    char hb[24];
    std::snprintf(hb, sizeof(hb), "%02d", slot);
    d.text(PX + 80, PY + 2, hb, pal::CURSOR);

    const bool rec = mixer_.is_resampling();
    // big arm/stop button
    const int BX = PX + 60, BY = PY + 28, BW = 192, BH = 56;
    ui::Color bg = rec ? pal::RECORD : pal::BG_HI;
    d.rect(BX, BY, BW, BH, bg);
    if (rec) {
        uint8_t br = breathe_pulse(frame_, 32);
        d.corner_brackets(BX, BY, BW, BH,
                          lerp_color(with_alpha(pal::FG, 130), pal::FG, br), 6, 2);
        float sec = mixer_.resample_frames() / 32000.0f;
        char tb[32];
        std::snprintf(tb, sizeof(tb), "REC %.1fs  TAP=STOP", sec);
        d.text(BX + (BW - (int)std::strlen(tb) * 6) / 2, BY + BH / 2 - 4, tb, pal::FG);
    } else {
        d.corner_brackets(BX, BY, BW, BH, pal::RECORD, 6, 2);
        d.text(BX + (BW - 14 * 6) / 2, BY + BH / 2 - 4, "TAP TO RECORD", pal::FG);
    }

    d.text(PX + 2, PY + PH - 24, "records MASTER out (resample/bounce)", pal::FG_DIM);
    d.text(PX + 2, PY + PH - 12, "starts playback if stopped - 15s max", pal::FG_DIM);
}

// === DrumKit bottom tabs (KB / GEN) + GEN panel ===
// GEN generates a procedural drum straight onto the pad under the cursor:
// - pad has a slot -> overwrite that slot's data
// - pad empty -> allocate the first free bank slot and assign it to the pad
namespace {
    constexpr int KTAB_Y = 90, KTAB_H = 16, KTAB_N = 2;
    constexpr int KTAB_W = 58, KTAB_X0 = 8, KTAB_GAP = 4;
    inline int ktab_x(int i) { return KTAB_X0 + i * (KTAB_W + KTAB_GAP); }
    // GEN grid: 4 cols x 3 rows of drum types
    constexpr int GEN_X = 4, GEN_Y = 116, GEN_CW = 78, GEN_CH = 34;
    constexpr int GEN_COLS = 4;
}

void App::draw_kit_tabs(Draw& d) {
    static const char* labels[KTAB_N] = { "KB", "GEN" };
    int active = (int)kit_panel_;
    for (int i = 0; i < KTAB_N; ++i) {
        int x = ktab_x(i);
        bool on = (i == active);
        ui::Color bg = on ? pal::HEADER : pal::BG_HI;
        ui::Color fg = on ? pal::BG : pal::FG_DIM;
        ui_button(d, x, KTAB_Y, KTAB_W, KTAB_H, bg, bg, labels[i], fg, on);
        if (on) {
            uint8_t br = breathe_pulse(frame_, 64);
            d.corner_brackets(x, KTAB_Y, KTAB_W, KTAB_H,
                              lerp_color(with_alpha(pal::CURSOR, 130), pal::CURSOR, br), 4, 1);
        }
    }
}

bool App::kit_tab_touch(int x, int y) {
    if (y < KTAB_Y || y >= KTAB_Y + KTAB_H) return false;
    for (int i = 0; i < KTAB_N; ++i) {
        int bx = ktab_x(i);
        if (x >= bx && x < bx + KTAB_W) {
            kit_panel_ = (KitPanel)i;
            mark_dirty();
            return true;
        }
    }
    return false;
}

int App::gen_drum_to_pad(int pad, int drum_type) {
    auto& inst = project_.instruments[cur_inst_];
    if (inst.type != seq::InstrumentType::DrumKit) return -1;
    if (pad < 0 || pad >= synth::DRUMKIT_PADS) return -1;

    int slot = inst.drumkit.slots[pad];
    if (slot == 0xFF || slot >= synth::SAMPLE_BANK_SIZE) {
        // pad empty: find the first free bank slot
        slot = -1;
        for (int i = 0; i < synth::SAMPLE_BANK_SIZE; ++i) {
            if (synth::SampleBank::instance().slot(i).empty()) { slot = i; break; }
        }
        if (slot < 0) return -1;   // bank full
        inst.drumkit.slots[pad] = (uint8_t)slot;
    }
    auto& s = synth::SampleBank::instance().slot(slot);
    synth::generate_drum(s, (synth::DrumType)drum_type);
    mark_dirty();

    // instant preview: hear what landed on the pad
    auto* v = project_.make_voice(cur_inst_);
    mixer_.replace_voice(0, v);
    if (v) v->note_on(inst.drumkit.base_note + pad, 110);
    return slot;
}

void App::draw_gen_panel(Draw& d) {
    const auto& inst = project_.instruments[cur_inst_];
    // header: which pad receives the sound
    int pad = (inst_row_ >= 3 && inst_row_ < 19) ? inst_row_ - 3 : 0;
    char hb[48];
    uint8_t slot = inst.drumkit.slots[pad];
    if (slot == 0xFF)
        std::snprintf(hb, sizeof(hb), "GEN > PAD %X (new slot)", pad);
    else
        std::snprintf(hb, sizeof(hb), "GEN > PAD %X (slot %02d)", pad, slot);
    d.text(GEN_X, GEN_Y - 10, hb, pal::HEADER);

    for (int i = 0; i < synth::DRUM_TYPE_COUNT; ++i) {
        int col = i % GEN_COLS, row = i / GEN_COLS;
        int x = GEN_X + col * GEN_CW;
        int y = GEN_Y + row * GEN_CH;
        ui_button(d, x, y, GEN_CW - 4, GEN_CH - 4, pal::BG_HI, pal::FG_DIM);
        d.corner_brackets(x, y, GEN_CW - 4, GEN_CH - 4, pal::FG_DIM, 4, 1);
        const char* nm = synth::drum_type_name((synth::DrumType)i);
        int len = 0; while (nm[len]) ++len;
        d.text(x + (GEN_CW - 4 - len * 6) / 2, y + (GEN_CH - 4) / 2 - 4, nm, pal::FG);
    }
}

bool App::gen_panel_touch(int x, int y) {
    if (y < GEN_Y || x < GEN_X) return false;
    int col = (x - GEN_X) / GEN_CW;
    int row = (y - GEN_Y) / GEN_CH;
    if (col < 0 || col >= GEN_COLS) return false;
    int i = row * GEN_COLS + col;
    if (i < 0 || i >= synth::DRUM_TYPE_COUNT) return false;
    int pad = (inst_row_ >= 3 && inst_row_ < 19) ? inst_row_ - 3 : 0;
    gen_drum_to_pad(pad, i);
    return true;
}

// === bottom-screen WAV loader/browser panel ===
// drawn in the keyboard zone when inst_load_panel_ is on (Sampler instrument).
// shows a scrollable list of folders + .wav files; navigated by up/down/A.
void App::draw_load_panel(Draw& d, int slot) {
    (void)slot;
    constexpr int PX = 4, PY = 116, PW = 312, PH = 122;   // y 116..238

    // panel background + header
    d.rect(PX - 2, PY - 2, PW + 4, PH + 4, pal::BG_HI);
    d.rect(PX, PY, PW, PH, pal::BG);
    d.text(PX + 2, PY + 2, "LOAD WAV", pal::HEADER);
    {
        char hb[48];
        std::snprintf(hb, sizeof(hb), "/%s", wav_subpath_[0] ? wav_subpath_ : "");
        d.text(PX + 70, PY + 2, hb, pal::FG_DIM);
    }

    constexpr int LIST_Y  = PY + 16;
    constexpr int ROW_H   = 11;
    constexpr int VISIBLE  = 9;          // visible list rows
    constexpr int HINT_Y  = PY + PH - 10;

    if (wav_count_ == 0) {
        d.text(PX + 6, LIST_Y + 4, "NO WAV FILES", pal::FG_DIM);
        d.text(PX + 6, LIST_Y + 16, "(put in sdmc:/3ds/descry/wav)", pal::FG_DIM);
        d.text(PX + 2, HINT_Y, "R+SELECT=RESCAN  (tab KB to close)", pal::FG_DIM);
        return;
    }

    // scroll so wav_sel_ stays visible (centered window)
    int top = wav_sel_ - VISIBLE / 2;
    if (top > wav_count_ - VISIBLE) top = wav_count_ - VISIBLE;
    if (top < 0) top = 0;

    for (int row = 0; row < VISIBLE; ++row) {
        int i = top + row;
        if (i >= wav_count_) break;
        int ry = LIST_Y + row * ROW_H;
        bool sel = (i == wav_sel_);
        if (sel) {
            d.rect(PX + 2, ry - 1, PW - 4, ROW_H, pal::BG_HI);
            uint8_t br = breathe_pulse(frame_, 64);
            ui::Color cc = lerp_color(with_alpha(pal::CURSOR, 130), pal::CURSOR, br);
            d.corner_brackets(PX + 2, ry - 1, PW - 4, ROW_H, cc, 4, 1);
        }
        ui::Color col = sel ? pal::FG : pal::FG_DIM;
        if (wav_is_dir_[i]) {
            d.text(PX + 6, ry, "[]", pal::HEADER);
            d.text(PX + 24, ry, wav_files_[i], sel ? pal::FG : pal::HEADER);
        } else {
            d.text(PX + 24, ry, wav_files_[i], col);
        }
    }

    // scroll indicators
    if (top > 0)                    d.text(PX + PW - 12, LIST_Y, "^", pal::FG_DIM);
    if (top + VISIBLE < wav_count_) d.text(PX + PW - 12, LIST_Y + (VISIBLE - 1) * ROW_H, "v", pal::FG_DIM);

    d.text(PX + 2, HINT_Y, "A=OPEN/LOAD  UP/DN=SEL  (tab KB to close)", pal::FG_DIM);
}

// input handler for the WAV loader panel. ports the Screen::Sample LOAD logic:
// scans wav/ on first open, up/down navigates, A enters folder / loads file.
void App::load_panel_input(const InputState& in, int slot) {
    auto& s = synth::SampleBank::instance().slot(slot);
    constexpr const char* WAV_ROOT = "sdmc:/3ds/descry/wav";
    char curdir[160];
    if (wav_subpath_[0])
        std::snprintf(curdir, sizeof(curdir), "%s/%s", WAV_ROOT, wav_subpath_);
    else
        std::snprintf(curdir, sizeof(curdir), "%s", WAV_ROOT);

    if (!wav_scanned_) {
        ::mkdir("sdmc:/3ds", 0777);
        ::mkdir("sdmc:/3ds/descry", 0777);
        ::mkdir(WAV_ROOT, 0777);
        wav_count_ = 0;
        // in a subfolder - the first item is ".." (go back)
        if (wav_subpath_[0]) {
            std::strcpy(wav_files_[wav_count_], "..");
            wav_is_dir_[wav_count_] = true;
            ++wav_count_;
        }
        DIR* dir = ::opendir(curdir);
        if (dir) {
            struct dirent* ent;
            while ((ent = ::readdir(dir)) && wav_count_ < MAX_WAV_FILES) {
                const char* name = ent->d_name;
                if (name[0] == '.') continue;   // hidden and ./..
                std::size_t len = std::strlen(name);
                bool is_dir = (ent->d_type == DT_DIR);
                if (is_dir) {
                    std::strncpy(wav_files_[wav_count_], name, 39);
                    wav_files_[wav_count_][39] = 0;
                    wav_is_dir_[wav_count_] = true;
                    ++wav_count_;
                } else if (len >= 5) {
                    const char* ext = name + len - 4;
                    if ((ext[0] == '.') &&
                        (ext[1] == 'w' || ext[1] == 'W') &&
                        (ext[2] == 'a' || ext[2] == 'A') &&
                        (ext[3] == 'v' || ext[3] == 'V')) {
                        std::strncpy(wav_files_[wav_count_], name, 39);
                        wav_files_[wav_count_][39] = 0;
                        wav_is_dir_[wav_count_] = false;
                        ++wav_count_;
                    }
                }
            }
            ::closedir(dir);
        }
        wav_scanned_ = true;
        if (wav_sel_ >= wav_count_) wav_sel_ = 0;
    }

    // up/down - select
    if (wav_count_ > 0) {
        if (in.up)   wav_sel_ = (wav_sel_ - 1 + wav_count_) % wav_count_;
        if (in.down) wav_sel_ = (wav_sel_ + 1) % wav_count_;
    }
    // R+SELECT = rescan
    if (in.select_ && in.held_r) {
        wav_scanned_ = false;
        return;
    }
    // A = enter folder / load file
    if (in.a && wav_count_ > 0) {
        if (wav_is_dir_[wav_sel_]) {
            if (std::strcmp(wav_files_[wav_sel_], "..") == 0) {
                char* sl = std::strrchr(wav_subpath_, '/');
                if (sl) *sl = 0;
                else wav_subpath_[0] = 0;
            } else {
                if (wav_subpath_[0]) {
                    std::size_t l = std::strlen(wav_subpath_);
                    std::snprintf(wav_subpath_ + l, sizeof(wav_subpath_) - l, "/%s", wav_files_[wav_sel_]);
                } else {
                    std::strncpy(wav_subpath_, wav_files_[wav_sel_], sizeof(wav_subpath_) - 1);
                }
            }
            wav_sel_ = 0;
            wav_scanned_ = false;   // rescan the new folder
            return;
        }
        // load the .wav into this sampler's slot
        char path[200];
        std::snprintf(path, sizeof(path), "%s/%s", curdir, wav_files_[wav_sel_]);
        constexpr int MAX_FRAMES = 32000 * 15;     // 15 sec hard cap
        auto r = synth::load_wav_to_sample(path, s, 32000, MAX_FRAMES);
        if ((int)r >= 0) {
            mark_dirty();
            inst_panel_ = InstPanel::Kb;   // close panel on success
        }
        return;
    }
}

// tab buttons (KB / WAVE / SLICE / LOAD / REC) shown on the bottom screen when
// editing a Sampler instrument. lets the user switch the bottom panel without
// stealing START (global play/stop). geometry shared with inst_tab_touch().
namespace {
    constexpr int TAB_Y = 90, TAB_H = 16, TAB_N = 5;
    constexpr int TAB_W = 58, TAB_X0 = 8, TAB_GAP = 4;   // 5*58+4*4=306 (<320)
    inline int tab_x(int i) { return TAB_X0 + i * (TAB_W + TAB_GAP); }
}

void App::draw_inst_tabs(Draw& d) {
    static const char* labels[TAB_N] = { "KB", "WAVE", "SLICE", "LOAD", "REC" };
    int active = (int)inst_panel_;
    for (int i = 0; i < TAB_N; ++i) {
        int x = tab_x(i);
        bool on = (i == active);
        // REC tab glows red while resampling is armed/active
        bool rec_hot = (i == (int)InstPanel::Rec) && mixer_.is_resampling();
        ui::Color bg = rec_hot ? pal::RECORD : (on ? pal::HEADER : pal::BG_HI);
        ui::Color fg = (on || rec_hot) ? pal::BG : pal::FG_DIM;
        ui_button(d, x, TAB_Y, TAB_W, TAB_H, bg, bg, labels[i], fg, on || rec_hot);
        if (on) {
            uint8_t br = breathe_pulse(frame_, 64);
            d.corner_brackets(x, TAB_Y, TAB_W, TAB_H,
                              lerp_color(with_alpha(pal::CURSOR, 130), pal::CURSOR, br), 4, 1);
        }
    }
}

// returns true if a tab button was hit (and switches the panel).
bool App::inst_tab_touch(int x, int y) {
    if (y < TAB_Y || y >= TAB_Y + TAB_H) return false;
    for (int i = 0; i < TAB_N; ++i) {
        int bx = tab_x(i);
        if (x >= bx && x < bx + TAB_W) {
            inst_panel_ = (InstPanel)i;
            if (inst_panel_ == InstPanel::Load) wav_scanned_ = false;   // rescan on opening LOAD
            mark_dirty();
            return true;
        }
    }
    return false;
}
// === per-instrument FX defaults section (shared by all instrument types) ===
// bottom status-bar strip of 8 cells: FLT CUT RES DEL REV VOL PAN CRS, mapped to
// inst.fx_filter_type / fx_cutoff / fx_resonance / fx_send_del / fx_send_rev / fx_volume / fx_pan / fx_bits.
namespace {
    constexpr int FX_CELLS = 8;
    // 3-char labels - single-line layout (label + value side by side)
    static const char* const kFxLabels[FX_CELLS] = {
        "FLT", "CUT", "RES", "DEL", "REV", "VOL", "PAN", "CRS"
    };
    static const char* const kFiltNames[5] = { "OFF", "LP", "HP", "BP", "NTC" };
}

// handle FX-section focus toggle (ZL+SELECT) and, while focused, all navigation
// and value editing. returns true if the FX section consumed this frame's input
// (caller must early-return so the normal row nav/edit does not also fire).
bool App::update_fx_section(const InputState& in, seq::Instrument& inst) {
    // toggle focus with ZL+SELECT (free chord: L+SELECT is the global scope toggle,
    // R+SELECT is the sample-editor jump, plain SELECT is note preview).
    if (in.held_zl && in.select_) {
        inst_fx_col_ = (inst_fx_col_ < 0) ? 0 : -1;
        return true;
    }
    if (inst_fx_col_ < 0) return false;   // section not focused - normal nav runs

    // up exits the section back to the normal rows (it sits at the bottom).
    if (in.up) { inst_fx_col_ = -1; return true; }

    // left/right move between FX cells.
    if (in.left)  inst_fx_col_ = (inst_fx_col_ - 1 + FX_CELLS) % FX_CELLS;
    if (in.right) inst_fx_col_ = (inst_fx_col_ + 1) % FX_CELLS;

    // A/B = +/-1, X/Y = +/-16.
    int delta = 0;
    if (in.a) delta = +1;
    if (in.b) delta = -1;
    if (in.x) delta = +16;
    if (in.y) delta = -16;

    if (delta) {
        switch (inst_fx_col_) {
            case 0: {   // FILT type 0..4
                int v = (int)inst.fx_filter_type + (delta > 0 ? 1 : -1);
                if (v < 0) v = 0; if (v > 4) v = 4;
                inst.fx_filter_type = (uint8_t)v;
                break;
            }
            case 1: {   // CUT 0..255
                int v = (int)inst.fx_cutoff + delta;
                if (v < 0) v = 0; if (v > 255) v = 255;
                inst.fx_cutoff = (uint8_t)v;
                break;
            }
            case 2: {   // RES 0..255
                int v = (int)inst.fx_resonance + delta;
                if (v < 0) v = 0; if (v > 255) v = 255;
                inst.fx_resonance = (uint8_t)v;
                break;
            }
            case 3: {   // DEL send 0..255
                int v = (int)inst.fx_send_del + delta;
                if (v < 0) v = 0; if (v > 255) v = 255;
                inst.fx_send_del = (uint8_t)v;
                break;
            }
            case 4: {   // REV send 0..255
                int v = (int)inst.fx_send_rev + delta;
                if (v < 0) v = 0; if (v > 255) v = 255;
                inst.fx_send_rev = (uint8_t)v;
                break;
            }
            case 5: {   // VOL 0..255
                int v = (int)inst.fx_volume + delta;
                if (v < 0) v = 0; if (v > 255) v = 255;
                inst.fx_volume = (uint8_t)v;
                break;
            }
            case 6: {   // PAN -128..127 (signed)
                int v = (int)inst.fx_pan + delta;
                if (v < -128) v = -128; if (v > 127) v = 127;
                inst.fx_pan = (int8_t)v;
                break;
            }
            case 7: {   // CRUSH bits 1..16 (16 = clean)
                int v = (int)inst.fx_bits + delta;
                if (v < 1) v = 1; if (v > 16) v = 16;
                inst.fx_bits = (uint8_t)v;
                break;
            }
        }
        mark_dirty();
    }
    return true;   // FX focused: consume the frame, suppress normal nav/edit
}

// live param push: copy the edited instrument's params into every voice that
// is currently sounding from it - held preview notes AND playing sequencer
// notes react to edits immediately instead of waiting for the next trigger
// (DoubleSprattt: "cutoff doesn't change while I'm holding the note").
// envelopes/phase stay untouched, so this is click-free.
void App::push_live_inst_params(uint8_t inst_id) {
    audio::Mixer::LockGuard lg(mixer_);
    for (int t = 0; t < audio::NUM_TRACKS; ++t) {
        auto& tr = mixer_.track(t);
        for (int k = 0; k < audio::TRACK_POLY; ++k)
            if (tr.voices[k]) project_.refresh_voice_params(tr.voices[k], inst_id);
    }
}

void App::update_instrument(const InputState& in) {
    auto& inst = project_.instruments[cur_inst_];
    const bool is_drum = (inst.type == seq::InstrumentType::DrumKit);

    // FX defaults section: ZL+SELECT toggles focus; while focused it owns the input.
    // skip on None (no instrument). returns true -> early-out, normal nav suppressed.
    if (inst.type != seq::InstrumentType::None) {
        if (update_fx_section(in, inst)) return;
    }
    const bool is_fm   = (inst.type == seq::InstrumentType::FmSynth);
    const bool is_sampler = (inst.type == seq::InstrumentType::Sampler);
    const bool is_dsn  = (inst.type == seq::InstrumentType::DsnSynth);

    // layout:
    //  normal: 7 rows (type, osc/sl, attack, decay, sustain, release, table)
    //  drumkit: 3 top rows (type, base_note, table) + 16 pad cells 4x4 below
    //  fm: 5 top rows (type, algo, fb, master_vol, table) + 4 op rows
    //      inst_row_ 0..4 = top, 5..8 = op index (5+i), inst_col_ 0..5 = sub-field (R/L/A/D/S/R)
    //  dsn: 24 rows in two columns of 12 (like the sampler layout)
    int max_rows = 8;
    if (is_drum) max_rows = 20;
    else if (is_fm) max_rows = 11;
    else if (is_sampler) max_rows = SAMPLER_ROWS;
    else if (is_dsn) max_rows = DSN_ROWS;
    if (inst_row_ >= max_rows) inst_row_ = max_rows - 1;

    if (is_drum && inst_row_ >= 3 && inst_row_ < 19) {
        // grid navigation across pads (16 cells)
        int p = inst_row_ - 3;
        int px = p & 3, py = p >> 2;
        if (in.up)    { if (py > 0) py--; else { inst_row_ = 2; goto post_nav; } }
        if (in.down)  { if (py < 3) py++; else { inst_row_ = 19; goto post_nav; } }
        if (in.left)  { if (px > 0) px--; }
        if (in.right) { if (px < 3) px++; }
        inst_row_ = 3 + py * 4 + px;
    } else if (is_drum && inst_row_ == 19) {
        // drum POLY row
        if (in.up) inst_row_ = 18;
    } else if (is_fm && inst_row_ >= 6 && inst_row_ < 10) {
        // op rows: up/down switch op, left/right - sub-field
        if (in.up)   { if (inst_row_ > 6) inst_row_--; else inst_row_ = 5; goto post_nav; }
        if (in.down) { if (inst_row_ < 9) inst_row_++; else { inst_row_ = 10; goto post_nav; } }
        if (in.left)  { if (inst_col_ > 0) inst_col_--; }
        if (in.right) { if (inst_col_ < 6) inst_col_++; }
    } else if (is_fm && inst_row_ == 10) {
        // fm POLY row
        if (in.up) inst_row_ = 9;
    } else if (is_sampler) {
        // two-column grid: left col = first half, right col = second half.
        // up/down move within a column (wrap), left/right swap columns keeping the
        // vertical position. row 0 (TYPE) is shared.
        const int HALF = max_rows / 2;
        int col = inst_row_ / HALF;              // 0 or 1
        int pos = inst_row_ % HALF;
        if (in.up)    pos = (pos - 1 + HALF) % HALF;
        if (in.down)  pos = (pos + 1) % HALF;
        if (in.left)  col = 0;
        if (in.right) col = 1;
        inst_row_ = col * HALF + pos;
    } else if (is_dsn) {
        // three-column grid (13/12/12): up/down wrap within a column,
        // left/right hop columns keeping the vertical position (clamped).
        int col = dsn_col_of(inst_row_);
        int pos = inst_row_ - kDsnColStart[col];
        if (in.up)    pos = (pos - 1 + kDsnColLen[col]) % kDsnColLen[col];
        if (in.down)  pos = (pos + 1) % kDsnColLen[col];
        if (in.left  && col > 0) col--;
        if (in.right && col < 2) col++;
        if (pos >= kDsnColLen[col]) pos = kDsnColLen[col] - 1;
        inst_row_ = kDsnColStart[col] + pos;
    } else {
        if (in.up)   inst_row_ = (inst_row_ - 1 + max_rows) % max_rows;
        if (in.down) inst_row_ = (inst_row_ + 1) % max_rows;
    }
post_nav:

    // (panel switching is done via the KB/SLICE/LOAD tab buttons on the bottom
    //  screen - see instrument_tab_touch(). START stays as global play/stop.)

    // when the load panel is active, up/down/A drive the browser, not row editing.
    if (is_sampler && inst_panel_ == InstPanel::Load) {
        load_panel_input(in, inst.sampler.sample_slot);
        return;
    }
    int delta = 0;
    // when a bottom panel is open, A/B drive panel actions (handled below),
    // not row editing - so suppress delta entirely in that mode.
    if (!(is_sampler && inst_panel_ != InstPanel::Kb)) {
        if (in.a) delta = +1;
        if (in.b) delta = -1;
        if (in.x) delta = +16;
        if (in.y) delta = -16;
    }

    if (delta) {
        if (is_drum && inst_row_ >= 3 && inst_row_ < 19) {
            // edit sample slot for the pad
            int pad = inst_row_ - 3;
            uint8_t cur = inst.drumkit.slots[pad];
            int v = (cur == 0xFF) ? 0 : ((int)cur + delta);
            if (v < 0) inst.drumkit.slots[pad] = 0xFF;
            else if (v >= synth::SAMPLE_BANK_SIZE)
                inst.drumkit.slots[pad] = synth::SAMPLE_BANK_SIZE - 1;
            else inst.drumkit.slots[pad] = (uint8_t)v;
        } else if (is_drum && inst_row_ == 19) {
            // drum POLY toggle
            inst.poly = !inst.poly;
        } else if (is_fm && inst_row_ >= 6 && inst_row_ < 10) {
            // edit op param: row gives op_idx, col gives sub-field
            // cols: 0=RAT 1=WAV 2=LVL 3=ATK 4=DEC 5=SUS 6=REL
            int oi = inst_row_ - 6;
            auto& op = inst.fm.ops[oi];
            switch (inst_col_) {
                case 0: {  // ratio_idx
                    int v = (int)op.ratio_idx + delta;
                    if (v < 0) v = 0;
                    if (v > 15) v = 15;
                    op.ratio_idx = (uint8_t)v;
                    break;
                }
                case 1: {  // wave (SIN/TRI/SAW/SQR, wraps)
                    int v = (int)op.wave + (delta > 0 ? 1 : -1);
                    if (v < 0) v = 3;
                    if (v > 3) v = 0;
                    op.wave = (uint8_t)v;
                    break;
                }
                case 2: {  // level
                    int step = (delta == 16 ? 16 : delta == -16 ? -16 : delta);
                    int v = (int)op.level + step;
                    if (v < 0) v = 0; if (v > 127) v = 127;
                    op.level = (uint8_t)v;
                    break;
                }
                case 3: {  // attack
                    int step = delta * 50;
                    int v = (int)op.attack + step;
                    if (v < 0) v = 0; if (v > 65535) v = 65535;
                    op.attack = (uint16_t)v;
                    break;
                }
                case 4: {  // decay
                    int step = delta * 100;
                    int v = (int)op.decay + step;
                    if (v < 0) v = 0; if (v > 65535) v = 65535;
                    op.decay = (uint16_t)v;
                    break;
                }
                case 5: {  // sustain
                    int v = (int)op.sustain + delta;
                    if (v < 0) v = 0; if (v > 127) v = 127;
                    op.sustain = (uint8_t)v;
                    break;
                }
                case 6: {  // release
                    int step = delta * 100;
                    int v = (int)op.release + step;
                    if (v < 0) v = 0; if (v > 65535) v = 65535;
                    op.release = (uint16_t)v;
                    break;
                }
            }
        } else if (is_fm) {
            // top FM fields: 0=type 1=algo 2=fb 3=master_vol 4=table
            switch (inst_row_) {
                case 0: {
                    set_inst_type(inst, (int)inst.type + delta);
                    if (inst_row_ >= 6 && inst.type != seq::InstrumentType::FmSynth) inst_row_ = 0;
                    break;
                }
                case 1: {
                    int v = (int)inst.fm.algorithm + delta;
                    if (v < 0) v = 0; if (v >= synth::FM_NUM_ALGOS) v = synth::FM_NUM_ALGOS - 1;
                    inst.fm.algorithm = (uint8_t)v;
                    break;
                }
                case 2: {
                    int v = (int)inst.fm.feedback + delta;
                    if (v < 0) v = 0; if (v > 7) v = 7;
                    inst.fm.feedback = (uint8_t)v;
                    break;
                }
                case 3: {
                    int v = (int)inst.fm.master_volume + delta;
                    if (v < 0) v = 0; if (v > 127) v = 127;
                    inst.fm.master_volume = (uint8_t)v;
                    break;
                }
                case 4: {
                    int v = (int)inst.table_id + delta;
                    if (v < -1) v = -1; if (v >= seq::MAX_TABLES) v = seq::MAX_TABLES - 1;
                    inst.table_id = (v < 0) ? seq::EMPTY : (uint8_t)v;
                    break;
                }
                case 5: {
                    // PRESET: A/B cycles + loads
                    int p = fm_preset_idx_ + delta;
                    while (p < 0) p += synth::FM_PRESET_COUNT;
                    p %= synth::FM_PRESET_COUNT;
                    fm_preset_idx_ = p;
                    synth::fm_load_preset(inst.fm, (synth::FmPreset)p);
                    // rename instrument
                    std::snprintf(inst.name, sizeof(inst.name), "%s", synth::fm_preset_name((synth::FmPreset)p));
                    break;
                }
                case 10: {
                    // FM POLY toggle
                    inst.poly = !inst.poly;
                    break;
                }
            }
        } else if (is_sampler) {
            edit_sampler_row(inst.sampler, inst, delta);
        } else if (is_dsn) {
            edit_dsn_row(inst.dsn, inst, delta);
        } else switch (inst_row_) {
            case 0: {
                set_inst_type(inst, (int)inst.type + delta);
                // when entering/leaving drum mode - clamp the row
                if (inst_row_ >= 3 && inst.type != seq::InstrumentType::DrumKit) inst_row_ = 0;
                break;
            }
            case 1: {
                if (inst.type == seq::InstrumentType::Wavsynth) {
                    // wavsynth row 1 = PRESET. cycle: built-ins, then USER wavetables
                    // from the SD bank (shape=User + slot). one knob, whole palette.
                    const int user_n = synth::WavetableBank::instance().count();
                    const int total = synth::WAVE_PRESET_COUNT + user_n;
                    int p = wav_preset_idx_ + delta;
                    while (p < 0) p += total;
                    p %= total;
                    wav_preset_idx_ = p;
                    if (p < synth::WAVE_PRESET_COUNT) {
                        synth::wave_load_preset(inst.wavsynth, (synth::WavePreset)p);
                        std::snprintf(inst.name, sizeof(inst.name), "%s", synth::wave_preset_name((synth::WavePreset)p));
                    } else {
                        // keep the current envelope - just swap the oscillator table
                        inst.wavsynth.shape = synth::WaveShape::User;
                        inst.wavsynth.user_slot = (uint8_t)(p - synth::WAVE_PRESET_COUNT);
                        std::snprintf(inst.name, sizeof(inst.name), "%s",
                                      synth::WavetableBank::instance().name(inst.wavsynth.user_slot));
                    }
                } else if (inst.type == seq::InstrumentType::Sampler) {
                    int s = inst.sampler.sample_slot + delta;
                    if (s < 0) s = 0;
                    if (s >= synth::SAMPLE_BANK_SIZE) s = synth::SAMPLE_BANK_SIZE - 1;
                    inst.sampler.sample_slot = s;
                } else if (inst.type == seq::InstrumentType::DrumKit) {
                    int v = (int)inst.drumkit.base_note + delta;
                    if (v < 0) v = 0;
                    if (v > 127) v = 127;
                    inst.drumkit.base_note = (uint8_t)v;
                }
                break;
            }
            case 2: {
                if (inst.type == seq::InstrumentType::DrumKit) {
                    // in drum mode row 2 = TABLE
                    int v = (int)inst.table_id + delta;
                    if (v < -1) v = -1;
                    if (v >= seq::MAX_TABLES) v = seq::MAX_TABLES - 1;
                    inst.table_id = (v < 0) ? seq::EMPTY : (uint8_t)v;
                } else {
                    uint32_t* a = (inst.type == seq::InstrumentType::Wavsynth)
                                ? &inst.wavsynth.attack : &inst.sampler.attack;
                    int v = (int)*a + delta * 100;
                    if (v < 0) v = 0;
                    *a = (uint32_t)v;
                }
                break;
            }
            case 3:
                if (inst.type == seq::InstrumentType::Wavsynth) {
                    int v = (int)inst.wavsynth.decay + delta * 200;
                    if (v < 0) v = 0;
                    inst.wavsynth.decay = (uint32_t)v;
                }
                break;
            case 4:
                if (inst.type == seq::InstrumentType::Wavsynth) {
                    int v = (int)inst.wavsynth.sustain + delta * 1024;
                    if (v < 0) v = 0;
                    if (v > fx::Q15_ONE) v = fx::Q15_ONE;
                    inst.wavsynth.sustain = (fx::q15)v;
                } else if (inst.type == seq::InstrumentType::Sampler) {
                    // FINE tune: +/-50 cents. A/B +/-1, X/Y +/-16 (fast)
                    int v = (int)inst.sampler.fine_cents + delta;
                    if (v < -50) v = -50;
                    if (v >  50) v =  50;
                    inst.sampler.fine_cents = (int8_t)v;
                }
                break;
            case 5: {
                uint32_t* r = (inst.type == seq::InstrumentType::Wavsynth)
                            ? &inst.wavsynth.release : &inst.sampler.release;
                int v = (int)*r + delta * 200;
                if (v < 0) v = 0;
                *r = (uint32_t)v;
                break;
            }
            case 6: { // table id (non-drum)
                int v = (int)inst.table_id + delta;
                if (v < -1) v = -1;
                if (v >= seq::MAX_TABLES) v = seq::MAX_TABLES - 1;
                inst.table_id = (v < 0) ? seq::EMPTY : (uint8_t)v;
                break;
            }
            case 7: { // POLY toggle
                inst.poly = !inst.poly;
                break;
            }
        }
        // any param edit -> refresh voices that are already sounding (live tweak)
        push_live_inst_params(cur_inst_);
    }

    // instrument preview
    // slice panel active (Sampler): A = auto-slice x16, B = clear all chops.
    // (L+SELECT is taken globally by the fullscreen scope toggle, so we use A/B here.)
    if (is_sampler && inst_panel_ == InstPanel::Slice && (in.a || in.b)) {
        auto& smp = synth::SampleBank::instance().slot(inst.sampler.sample_slot);
        if (in.a && !smp.empty()) {
            // even 16-way slice across the whole sample
            uint32_t total = smp.num_frames();
            for (int i = 0; i < synth::Sample::MAX_CHOPS; ++i)
                smp.chops[i] = (uint32_t)((uint64_t)i * total / synth::Sample::MAX_CHOPS);
            smp_chop_sel_ = 0;
            mark_dirty();
            return;
        }
        if (in.b) {
            // clear all chops
            for (int i = 0; i < synth::Sample::MAX_CHOPS; ++i)
                smp.chops[i] = 0xFFFFFFFFu;
            smp_chop_sel_ = 0;
            mark_dirty();
            return;
        }
    }
    // slice panel: X = spread chops onto a DrumKit (chop->kit, was R+SELECT in Sample view)
    if (is_sampler && inst_panel_ == InstPanel::Slice && in.x) {
        make_kit_from_sample(inst.sampler.sample_slot);
        return;
    }
    // WAVE panel: A/B = root note +/- (matches the "ROOT (A/B)" hint)
    if (is_sampler && inst_panel_ == InstPanel::Wave && (in.a || in.b)) {
        auto& smp = synth::SampleBank::instance().slot(inst.sampler.sample_slot);
        if (!smp.empty()) {
            int v = smp.root_note + (in.a ? 1 : -1);
            if (v < 0) v = 0;
            if (v > 127) v = 127;
            smp.root_note = v;
            mark_dirty();
        }
        return;
    }

    if (in.select_) {
        auto* v = project_.make_voice(cur_inst_);
        mixer_.replace_voice(0, v);
        if (v) {
            int note = 60;
            if (is_drum) {
                // play the pad the cursor is on (or base_note if at the top)
                int pad = (inst_row_ >= 3) ? (inst_row_ - 3) : 0;
                note = inst.drumkit.base_note + pad;
            }
            v->note_on(note, 100);
            // hold-to-sustain: gate stays open while SELECT is held, so the
            // right hand is free to tweak params with A/B on a sounding note.
            preview_gate_ = true;
        }
    }
}

// draw the per-instrument FX defaults strip as a one-line status bar pinned to the
// very bottom of the top screen (y=226..240) - same slot as the phrase FX hint bar.
// opaque backing so it can never collide with any instrument layout above it.
// 8 cells: FLT CUT RES DEL REV VOL PAN CRS. label dim + value bright, side by side.
// when inst_fx_col_ >= 0 the focused cell gets a highlight box + breathing brackets.
void App::draw_fx_section(Draw& d, const seq::Instrument& inst) {
    constexpr int BAR_Y  = 228;    // text baseline
    constexpr int CELL_W = 47;     // 8 cells * 47 = 376, + 24px "FX" prefix = 400
    constexpr int FX_X   = 24;     // cells start after the "FX" tag
    const bool focused = (inst_fx_col_ >= 0);

    // opaque backing bar + thin separator on top (mirrors the phrase hint bar)
    d.rect(0, BAR_Y - 2, 400, 14, pal::BG_HI);
    d.rect(0, BAR_Y - 3, 400, 1, focused ? pal::CURSOR : pal::HEADER);

    // "FX" tag; when focused it glows as the section marker
    d.text(4, BAR_Y, "FX", focused ? pal::CURSOR : pal::FG_DIM);

    char vb[8];
    for (int c = 0; c < FX_CELLS; ++c) {
        int x = FX_X + c * CELL_W;
        bool sel = focused && (inst_fx_col_ == c);

        // value text + a "is this a non-default value" flag for dimming
        bool active = true;
        switch (c) {
            case 0:   // FLT
                std::snprintf(vb, sizeof(vb), "%s", kFiltNames[inst.fx_filter_type <= 4 ? inst.fx_filter_type : 0]);
                active = (inst.fx_filter_type != 0);
                break;
            case 1:   // CUT (0..255)
                std::snprintf(vb, sizeof(vb), "%d", (int)inst.fx_cutoff);
                active = (inst.fx_cutoff != 255);
                break;
            case 2:   // RES
                std::snprintf(vb, sizeof(vb), "%d", (int)inst.fx_resonance);
                active = (inst.fx_resonance != 0);
                break;
            case 3:   // DEL send
                std::snprintf(vb, sizeof(vb), "%d", (int)inst.fx_send_del);
                active = (inst.fx_send_del != 0);
                break;
            case 4:   // REV send
                std::snprintf(vb, sizeof(vb), "%d", (int)inst.fx_send_rev);
                active = (inst.fx_send_rev != 0);
                break;
            case 5:   // VOL
                std::snprintf(vb, sizeof(vb), "%d", (int)inst.fx_volume);
                active = (inst.fx_volume != 255);
                break;
            case 6: {  // PAN: "C" centre, "L.."/"R.." off-centre
                int p = (int)inst.fx_pan;
                if (p == 0)      std::snprintf(vb, sizeof(vb), "C");
                else if (p < 0)  std::snprintf(vb, sizeof(vb), "L%d", -p);
                else             std::snprintf(vb, sizeof(vb), "R%d", p);
                active = (p != 0);
                break;
            }
            case 7:   // CRS bits (16 = clean)
                std::snprintf(vb, sizeof(vb), "%d", (int)inst.fx_bits);
                active = (inst.fx_bits != 16);
                break;
        }

        // focused cell: filled highlight box behind label+value
        if (sel) {
            uint8_t br = breathe_pulse(frame_, 64);
            ui::Color cur = lerp_color(with_alpha(pal::CURSOR, 130), pal::CURSOR, br);
            d.rect(x - 2, BAR_Y - 2, CELL_W - 2, 12, pal::BG);
            d.corner_brackets(x - 2, BAR_Y - 2, CELL_W - 2, 12, cur, 3, 1);
        }

        // label (dim ochre) + value right after it
        d.text(x, BAR_Y, kFxLabels[c], sel ? pal::CURSOR : with_alpha(pal::HEADER, 180));
        ui::Color vcol = sel ? pal::FG : (active ? pal::FG : pal::FG_DIM);
        d.text(x + 20, BAR_Y, vb, vcol);
    }

    // hint line only while focused, tucked right above the bar (transient, opaque)
    if (focused) {
        d.rect(0, BAR_Y - 13, 400, 10, pal::BG_HI);
        d.text(4, BAR_Y - 12, "L/R:CELL A/B X/Y:EDIT UP:EXIT", pal::FG_DIM);
    }
}
void App::draw_instrument(Draw& d) {
    auto& inst = project_.instruments[cur_inst_];
    const bool is_drum = (inst.type == seq::InstrumentType::DrumKit);
    constexpr int Y0 = 22;
    constexpr int ROW_H = 16;

    char buf[40];
    std::snprintf(buf, sizeof(buf), "INSTRUMENT %02X", cur_inst_);
    d.text(20, Y0, buf, pal::HEADER, 1);
    d.text(20, Y0 + 12, inst.name, pal::FG, 1);

    // bank awareness: instruments are GLOBAL, not owned by a track. editing
    // slot 00 changes every phrase whose I column says 00 - make that visible.
    {
        int used = 0;
        for (int p = 0; p < seq::MAX_PHRASES; ++p) {
            const auto& ph = project_.phrases[p];
            for (int s = 0; s < seq::PHRASE_STEPS; ++s)
                if (ph.steps[s].instrument == cur_inst_) { ++used; break; }
        }
        char ub[40];
        if (used > 0) std::snprintf(ub, sizeof(ub), "USED IN %d PHRASE%s", used, used == 1 ? "" : "S");
        else          std::snprintf(ub, sizeof(ub), "UNUSED");
        int ulen = (int)std::strlen(ub);
        d.text(396 - ulen * 6, Y0, ub, used ? pal::FG_DIM : pal::GRID);
        static const char* slot_hint = "L+<> SLOT  L+A CLONE";
        d.text(396 - (int)std::strlen(slot_hint) * 6, Y0 + 12, slot_hint, pal::GRID);
    }

    static const char* type_names[] = {"NONE", "WAVSYN", "SAMPLER", "DRUMKIT", "FMSYN", "DSN"};
    static const char* shape_names[] = {"SINE", "SAW", "SQUAR", "TRI", "NOIS"};
    static const char* note_names[12] = {"C ","C#","D ","D#","E ","F ","F#","G ","G#","A ","A#","B "};

    auto fmt_note = [&](uint8_t n, char* dst, size_t cap) {
        int oct = (int)n / 12 - 1;
        std::snprintf(dst, cap, "%s%d", note_names[n % 12], oct);
    };

    if (is_drum) {
        // 3 top rows + 4x4 grid (compact: leaves room for the FX strip at the bottom)
        static const char* dfields[] = {"TYPE  ", "BASE  ", "TABLE "};
        constexpr int DROW_H = 14;
        for (int i = 0; i < 3; ++i) {
            int y = Y0 + 28 + i * DROW_H;
            d.text(40, y, dfields[i], pal::HEADER);
            const int VX = 130;
            if (i == 0) {
                d.text(VX, y, type_names[(int)inst.type], pal::FG);
            } else if (i == 1) {
                fmt_note(inst.drumkit.base_note, buf, sizeof(buf));
                d.text(VX, y, buf, pal::FG);
            } else {
                if (inst.table_id == seq::EMPTY) d.text(VX, y, "NONE", pal::FG_DIM);
                else {
                    std::snprintf(buf, sizeof(buf), "%02X", inst.table_id);
                    d.text(VX, y, buf, pal::CURSOR);
                }
            }
            if (i == inst_row_) {
                d.rect(36, y - 2, 200, 1, pal::CURSOR);
                d.rect(36, y + 10, 200, 1, pal::CURSOR);
            }
        }

        // 4x4 grid of pads (compact cell height to clear the FX strip)
        const int GX = 36;
        const int GY = Y0 + 28 + 3 * DROW_H + 8;
        const int CW = 56, CH = 18;
        d.text(GX, GY - 12, "PADS  (note -> sample)", pal::HEADER);
        for (int py = 0; py < 4; ++py) {
            for (int px = 0; px < 4; ++px) {
                int pad = py * 4 + px;
                int x = GX + px * CW;
                int y = GY + py * CH;
                uint8_t slot = inst.drumkit.slots[pad];
                bool sel = (inst_row_ == 3 + pad);
                bool filled = (slot != 0xFF && slot < synth::SAMPLE_BANK_SIZE &&
                               !synth::SampleBank::instance().slot(slot).empty());

                // filled pads get a subtle background tint so the kit reads at a glance
                if (filled) d.rect(x + 1, y + 1, CW - 4, CH - 4, pal::BG_HI);

                // frame (breathing on the selected pad)
                uint32_t border;
                if (sel) {
                    uint8_t br = breathe_pulse(frame_, 64);
                    border = lerp_color(with_alpha(pal::CURSOR, 130), pal::CURSOR, br);
                } else border = filled ? pal::HEADER : pal::FG_DIM;
                d.corner_brackets(x, y, CW - 2, CH - 2, border, 4, 1);

                // note in the top-left
                fmt_note((uint8_t)(inst.drumkit.base_note + pad), buf, sizeof(buf));
                d.text(x + 4, y + 3, buf, sel ? pal::CURSOR : pal::FG_DIM);

                // slot / status
                if (slot == 0xFF) {
                    d.text(x + 4, y + 12, "--", pal::FG_DIM);
                } else if (!filled) {
                    std::snprintf(buf, sizeof(buf), "%02d?", slot);   // assigned but empty slot
                    d.text(x + 4, y + 12, buf, pal::RECORD);
                } else {
                    std::snprintf(buf, sizeof(buf), "%02d", slot);
                    d.text(x + 4, y + 12, buf, sel ? pal::CURSOR : pal::FG);
                }
            }
        }

        // POLY row below the grid
        {
            int py = GY + 4 * CH + 6;
            bool sel = (inst_row_ == 19);
            ui::Color clr = sel ? pal::CURSOR : pal::HEADER;
            d.text(GX, py, "POLY ", clr);
            d.text(GX + 50, py, inst.poly ? "ON" : "OFF",
                   inst.poly ? pal::PLAY    : pal::FG_DIM);
            if (sel) {
                d.rect(GX - 2, py - 2, 120, 1, pal::CURSOR);
                d.rect(GX - 2, py + 10, 120, 1, pal::CURSOR);
            }
        }
        draw_fx_section(d, inst);
        return;
    }

    // === FmSynth layout ===
    if (inst.type == seq::InstrumentType::FmSynth) {
        // top 6 fields: TYPE / ALGO / FB / MAST / TABLE / PRESET
        static const char* fm_top[] = {"TYPE  ", "ALGO  ", "FB    ", "MAST  ", "TABLE ", "PRESET"};
        constexpr int FROW = 13;
        for (int i = 0; i < 6; ++i) {
            int y = Y0 + 30 + i * FROW;
            d.text(20, y, fm_top[i], pal::HEADER);
            const int VX = 90;
            switch (i) {
                case 0: d.text(VX, y, type_names[(int)inst.type], pal::FG); break;
                case 1: {
                    std::snprintf(buf, sizeof(buf), "%d %s", inst.fm.algorithm,
                                  synth::fm_algo_name(inst.fm.algorithm));
                    d.text(VX, y, buf, pal::FG);
                    break;
                }
                case 2: std::snprintf(buf, sizeof(buf), "%d", inst.fm.feedback);
                        d.text(VX, y, buf, pal::FG); break;
                case 3: std::snprintf(buf, sizeof(buf), "%d", inst.fm.master_volume);
                        d.text(VX, y, buf, pal::FG); break;
                case 4:
                    if (inst.table_id == seq::EMPTY) d.text(VX, y, "NONE", pal::FG_DIM);
                    else { std::snprintf(buf, sizeof(buf), "%02X", inst.table_id);
                           d.text(VX, y, buf, pal::CURSOR); }
                    break;
                case 5: {
                    std::snprintf(buf, sizeof(buf), "%d/%d %s",
                        fm_preset_idx_ + 1, synth::FM_PRESET_COUNT,
                        synth::fm_preset_name((synth::FmPreset)fm_preset_idx_));
                    d.text(VX, y, buf, pal::FG_HEX);
                    break;
                }
            }
            if (i == inst_row_) {
                d.rect(16, y - 2, 200, 1, pal::CURSOR);
                d.rect(16, y + 10, 200, 1, pal::CURSOR);
            }
        }

        // === algorithm diagram (right side, above the op grid) ===
        // boxes = ops, arrows = modulation flow, bottom bar = output.
        // carriers get a green border; FB marker on op1 when feedback > 0.
        {
            const int algo = inst.fm.algorithm & 7;
            const uint8_t car = synth::fm_algo_carrier_mask(algo);

            // depth = how many modulation hops above the carrier row.
            // relaxation: for every edge m->j enforce depth[m] >= depth[j]+1.
            int depth[4] = {0, 0, 0, 0};
            for (int pass = 0; pass < 4; ++pass)
                for (int j = 0; j < 4; ++j) {
                    uint8_t mm = synth::fm_algo_mod_mask(algo, j);
                    for (int m = 0; m < 4; ++m)
                        if ((mm & (1 << m)) && depth[m] <= depth[j])
                            depth[m] = depth[j] + 1;
                }

            constexpr int BW = 20, BH = 12;      // op box size
            constexpr int XSP = 34, YSP = 17;    // spacing
            constexpr int DX0 = 252;             // diagram left edge
            // +4 (was +10): keep a visible gap between the output bar and the
            // op-grid header below - the diagram used to sit right on "DEC SUS REL"
            const int OUT_Y = Y0 + 30 + 5 * 13 + 4;   // output bar baseline

            int bx[4], by[4];
            for (int i = 0; i < 4; ++i) {
                bx[i] = DX0 + i * XSP;
                by[i] = OUT_Y - BH - 2 - depth[i] * YSP;
            }

            // modulation connectors (drawn under the boxes)
            for (int j = 0; j < 4; ++j) {
                uint8_t mm = synth::fm_algo_mod_mask(algo, j);
                for (int m = 0; m < 4; ++m) {
                    if (!(mm & (1 << m))) continue;
                    int cxm = bx[m] + BW / 2, cxj = bx[j] + BW / 2;
                    int y_from = by[m] + BH;         // bottom of modulator
                    int y_mid  = by[j] - 3;          // just above target
                    if (y_mid > y_from)
                        d.rect(cxm, y_from, 1, y_mid - y_from, pal::HEADER);
                    int xl = cxm < cxj ? cxm : cxj;
                    int xw = (cxm < cxj ? cxj - cxm : cxm - cxj) + 1;
                    d.rect(xl, y_mid, xw, 1, pal::HEADER);
                    d.rect(cxj, y_mid, 1, 3, pal::HEADER);
                }
            }

            // output bar + carrier drops
            d.rect(DX0, OUT_Y, 3 * XSP + BW, 1, pal::FG_DIM);
            d.text(DX0 - 26, OUT_Y - 3, "OUT", pal::FG_DIM);
            for (int i = 0; i < 4; ++i) {
                if (!(car & (1 << i))) continue;
                d.rect(bx[i] + BW / 2, by[i] + BH, 1, OUT_Y - (by[i] + BH), pal::PLAY);
            }

            // op boxes (over the connectors)
            for (int i = 0; i < 4; ++i) {
                bool is_car = (car & (1 << i)) != 0;
                bool op_on  = inst.fm.ops[i].level > 0;
                bool row_sel = (inst_row_ == 6 + i);
                d.rect(bx[i], by[i], BW, BH, pal::BG_HI);
                ui::Color border = row_sel ? pal::CURSOR
                                  : is_car ? pal::PLAY
                                  : op_on  ? pal::HEADER : pal::FG_DIM;
                d.rect(bx[i], by[i], BW, 1, border);
                d.rect(bx[i], by[i] + BH - 1, BW, 1, border);
                d.rect(bx[i], by[i], 1, BH, border);
                d.rect(bx[i] + BW - 1, by[i], 1, BH, border);
                char nb[2] = { (char)('1' + i), 0 };
                d.text(bx[i] + 7, by[i] + 2, nb, op_on ? pal::FG : pal::FG_DIM);
            }

            // feedback loop marker on op1
            if (inst.fm.feedback > 0)
                d.text(bx[0] - 14, by[0] + 2, "FB", pal::RECORD);
        }

        // op grid header: column labels
        constexpr int GX = 16;
        constexpr int GY = Y0 + 30 + 6 * FROW + 4;
        constexpr int COL_W = 48;
        const int LBL_W = 24;
        static const char* col_names[7] = {"RAT", "WAV", "LVL", "ATK", "DEC", "SUS", "REL"};
        d.text(GX, GY, "OP", pal::HEADER);
        for (int c = 0; c < 7; ++c) {
            d.text(GX + LBL_W + c * COL_W, GY, col_names[c], pal::HEADER);
        }

        // op rows
        for (int oi = 0; oi < 4; ++oi) {
            int y = GY + 14 + oi * 12;
            std::snprintf(buf, sizeof(buf), "%d", oi + 1);
            d.text(GX, y, buf, pal::FG);
            const auto& op = inst.fm.ops[oi];
            const char* vals[7];
            char vbufs[7][8];
            std::snprintf(vbufs[0], 8, "%s",  synth::fm_ratio_name(op.ratio_idx));
            std::snprintf(vbufs[1], 8, "%s",  synth::fm_op_wave_name(op.wave));
            std::snprintf(vbufs[2], 8, "%d",  op.level);
            std::snprintf(vbufs[3], 8, "%u",  op.attack);
            std::snprintf(vbufs[4], 8, "%u",  op.decay);
            std::snprintf(vbufs[5], 8, "%d",  op.sustain);
            std::snprintf(vbufs[6], 8, "%u",  op.release);
            for (int v = 0; v < 7; ++v) vals[v] = vbufs[v];

            bool row_sel = (inst_row_ == 6 + oi);
            for (int c = 0; c < 7; ++c) {
                int x = GX + LBL_W + c * COL_W;
                bool cell_sel = row_sel && (inst_col_ == c);
                ui::Color clr = cell_sel ? pal::CURSOR : (op.level > 0 ? pal::FG : pal::FG_DIM);
                if (cell_sel) {
                    d.rect(x - 2, y - 1, COL_W - 2, 10, pal::BG_HI);
                }
                d.text(x, y, vals[c], clr);
            }
            // highlight the row level marker (brightness proportional to op.level)
            if (op.level > 0) {
                int bar = (op.level * 12) / 127;
                d.rect(GX + 14, y + 4, bar, 1, 0xFF516B43);
            }
        }

        // POLY row below the op grid
        {
            int py = GY + 14 + 4 * 12 + 6;
            bool sel = (inst_row_ == 10);
            ui::Color clr = sel ? pal::CURSOR : pal::HEADER;
            d.text(GX, py, "POLY ", clr);
            d.text(GX + 50, py, inst.poly ? "ON" : "OFF",
                   inst.poly ? pal::PLAY    : pal::FG_DIM);
            if (sel) {
                d.rect(GX - 2, py - 2, 120, 1, pal::CURSOR);
                d.rect(GX - 2, py + 10, 120, 1, pal::CURSOR);
            }
        }
        draw_fx_section(d, inst);
        return;
    }

    // === DSN: M8-style param list (three columns 13/12/12) ===
    if (inst.type == seq::InstrumentType::DsnSynth) {
        auto& dp = inst.dsn;
        constexpr int DROW_HT = 13;
        // column x: name/value pairs. 3 columns across 400px.
        static const int kNx[3] = { 8,   142, 276 };
        static const int kVx[3] = { 56,  190, 324 };
        for (int i = 0; i < DSN_ROWS; ++i) {
            int col = dsn_col_of(i);
            int pos = i - kDsnColStart[col];
            int nx = kNx[col], vx = kVx[col];
            int y  = Y0 + 24 + pos * DROW_HT;
            bool sel = (i == inst_row_);
            d.text(nx, y, kDsnRowNames[i], sel ? pal::CURSOR : pal::HEADER);
            if (sel) {
                uint8_t br = breathe_pulse(frame_, 64);
                ui::Color cur = lerp_color(with_alpha(pal::CURSOR, 130), pal::CURSOR, br);
                d.corner_brackets(vx - 4, y - 2, 74, 11, cur, 3, 1);
            }
            char vb[24];
            auto pct = [](fx::q15 v) { return (int)((uint64_t)v * 100 / fx::Q15_ONE); };
            auto spct = [](int16_t v) { return (int)((int64_t)v * 100 / fx::Q15_ONE); };
            switch (i) {
                case DR_TYPE:      d.text(vx, y, "DSN", pal::FG); continue;
                case DR_PRESET:
                    std::snprintf(vb, sizeof(vb), "%s",
                        synth::dsn_preset_name((synth::DsnPreset)dsn_preset_idx_));
                    d.text(vx, y, vb, pal::FG_HEX);
                    continue;
                case DR_OCT:
                    if (dp.octave == 0) { d.text(vx, y, "0", pal::FG_DIM); continue; }
                    std::snprintf(vb, sizeof(vb), "%+d", dp.octave);
                    break;
                case DR_VCO1_WAVE: d.text(vx, y, synth::dsn_wave_name(dp.vco1_wave), pal::FG_HEX); continue;
                case DR_VCO1_PW:   std::snprintf(vb, sizeof(vb), "%d%%", pct(dp.vco1_pw)); break;
                case DR_VCO2_WAVE: d.text(vx, y, synth::dsn_wave_name(dp.vco2_wave), pal::FG_HEX); continue;
                case DR_VCO2_SEMI: std::snprintf(vb, sizeof(vb), "%+d", dp.vco2_semi); break;
                case DR_VCO2_DET:  std::snprintf(vb, sizeof(vb), "%+dct", dp.vco2_detune); break;
                case DR_SYNC:
                    d.text(vx, y, dp.vco2_sync ? "ON" : "OFF",
                           dp.vco2_sync ? pal::PLAY : pal::FG_DIM);
                    continue;
                case DR_BALANCE:   std::snprintf(vb, sizeof(vb), "%d:%d", 100 - pct(dp.balance), pct(dp.balance)); break;
                case DR_CUTOFF:    std::snprintf(vb, sizeof(vb), "%d%%", pct(dp.cutoff)); break;
                case DR_RES:       std::snprintf(vb, sizeof(vb), "%d%%", pct(dp.resonance)); break;
                case DR_FILT_TYPE:
                    d.text(vx, y, dsp::filter_type_name((dsp::FilterType)dp.vcf_type), pal::FG_HEX);
                    continue;
                case DR_PORTA:
                    if (dp.portamento == 0) { d.text(vx, y, "OFF", pal::FG_DIM); continue; }
                    std::snprintf(vb, sizeof(vb), "%d", dp.portamento);
                    break;
                case DR_EG1_A:  std::snprintf(vb, sizeof(vb), "%u", (unsigned)dp.eg1_attack); break;
                case DR_EG1_D:  std::snprintf(vb, sizeof(vb), "%u", (unsigned)dp.eg1_decay); break;
                case DR_EG1_S:  std::snprintf(vb, sizeof(vb), "%d%%", pct(dp.eg1_sustain)); break;
                case DR_EG1_R:  std::snprintf(vb, sizeof(vb), "%u", (unsigned)dp.eg1_release); break;
                case DR_EG1_PIT: std::snprintf(vb, sizeof(vb), "%+d%%", spct(dp.eg1_to_pitch)); break;
                case DR_EG1_CUT: std::snprintf(vb, sizeof(vb), "%+d%%", spct(dp.eg1_to_cutoff)); break;
                case DR_EG2_A:  std::snprintf(vb, sizeof(vb), "%u", (unsigned)dp.eg2_attack); break;
                case DR_EG2_D:  std::snprintf(vb, sizeof(vb), "%u", (unsigned)dp.eg2_decay); break;
                case DR_EG2_S:  std::snprintf(vb, sizeof(vb), "%d%%", pct(dp.eg2_sustain)); break;
                case DR_EG2_R:  std::snprintf(vb, sizeof(vb), "%u", (unsigned)dp.eg2_release); break;
                case DR_EG2_PIT: std::snprintf(vb, sizeof(vb), "%+d%%", spct(dp.eg2_to_pitch)); break;
                case DR_EG2_CUT: std::snprintf(vb, sizeof(vb), "%+d%%", spct(dp.eg2_to_cutoff)); break;
                case DR_MG1_WAVE: d.text(vx, y, dsp::mg_wave_name(dp.mg1_wave), pal::FG_HEX); continue;
                case DR_MG1_RATE: std::snprintf(vb, sizeof(vb), "%d%%", pct(dp.mg1_rate)); break;
                case DR_MG1_PIT:  std::snprintf(vb, sizeof(vb), "%+d%%", spct(dp.mg1_to_pitch)); break;
                case DR_MG1_CUT:  std::snprintf(vb, sizeof(vb), "%+d%%", spct(dp.mg1_to_cutoff)); break;
                case DR_MG2_WAVE: d.text(vx, y, dsp::mg_wave_name(dp.mg2_wave), pal::FG_HEX); continue;
                case DR_MG2_RATE: std::snprintf(vb, sizeof(vb), "%d%%", pct(dp.mg2_rate)); break;
                case DR_MG2_PW:   std::snprintf(vb, sizeof(vb), "%+d%%", spct(dp.mg2_to_pw)); break;
                case DR_MG2_VCA:  std::snprintf(vb, sizeof(vb), "%+d%%", spct(dp.mg2_to_vca)); break;
                case DR_VCA_MODE:
                    d.text(vx, y, dp.vca_mode ? "EG" : "GATE", pal::FG_HEX);
                    continue;
                case DR_VCA_LVL:  std::snprintf(vb, sizeof(vb), "%d%%", pct(dp.vca_level)); break;
                case DR_DRIVE:
                    if (dp.drive == 0) { d.text(vx, y, "OFF", pal::FG_DIM); continue; }
                    std::snprintf(vb, sizeof(vb), "%d%%", pct(dp.drive));
                    break;
                default: vb[0] = 0; break;
            }
            // dim zero-ish mod depths so active routing pops
            bool dim = (i == DR_EG1_PIT && dp.eg1_to_pitch == 0) ||
                       (i == DR_EG1_CUT && dp.eg1_to_cutoff == 0) ||
                       (i == DR_EG2_PIT && dp.eg2_to_pitch == 0) ||
                       (i == DR_EG2_CUT && dp.eg2_to_cutoff == 0) ||
                       (i == DR_MG1_PIT && dp.mg1_to_pitch == 0) ||
                       (i == DR_MG1_CUT && dp.mg1_to_cutoff == 0) ||
                       (i == DR_MG2_PW  && dp.mg2_to_pw == 0) ||
                       (i == DR_MG2_VCA && dp.mg2_to_vca == 0);
            d.text(vx, y, vb, dim ? pal::FG_DIM : pal::FG);
        }
        draw_fx_section(d, inst);
        return;
    }

    // === Sampler: M8-style param list (two columns) ===
    if (inst.type == seq::InstrumentType::Sampler) {
        auto& sp = inst.sampler;
        auto& smp = synth::SampleBank::instance().slot(sp.sample_slot);
        constexpr int SROW_H = 22;
        constexpr int HALF = SAMPLER_ROWS / 2;   // 6 rows per column
        // column geometry: left starts at x=14, right at x=206. each column
        // has a label (NAME) and a value (VAL) offset.
        for (int i = 0; i < SAMPLER_ROWS; ++i) {
            int col = i / HALF;          // 0 left, 1 right
            int pos = i % HALF;          // 0..5 vertical
            int nx = (col == 0) ? 14  : 206;
            int vx = (col == 0) ? 84  : 276;
            int y  = Y0 + 30 + pos * SROW_H;
            bool sel = (i == inst_row_);
            d.text(nx, y, kSamplerRowNames[i], sel ? pal::CURSOR : pal::HEADER);
            if (sel) {
                uint8_t br = breathe_pulse(frame_, 64);
                ui::Color cur = lerp_color(with_alpha(pal::CURSOR, 130), pal::CURSOR, br);
                d.corner_brackets(vx - 4, y - 2, 110, 11, cur, 3, 1);
            }
            const int VX = vx;
            char vb[40];
            switch (i) {
                case SR_TYPE:
                    d.text(VX, y, "SAMPLER", pal::FG);
                    break;
                case SR_SAMPLE:
                    if (smp.empty())
                        std::snprintf(vb, sizeof(vb), "%02d (EMPTY)", sp.sample_slot);
                    else
                        std::snprintf(vb, sizeof(vb), "%02d  %.2fs", sp.sample_slot,
                                      smp.num_frames() / (float)synth::SAMPLER_SR);
                    d.text(VX, y, vb, smp.empty() ? pal::FG_DIM : pal::FG);
                    break;
                case SR_PLAY:
                    d.text(VX, y, synth::play_mode_name(sp.play_mode), pal::FG_HEX);
                    break;
                case SR_SLICE:
                    if (sp.chromatic_slices)
                        d.text(VX, y, "CHROMATIC", pal::FG_HEX);
                    else if (sp.slice == 0)
                        d.text(VX, y, "WHOLE", pal::FG_DIM);
                    else {
                        std::snprintf(vb, sizeof(vb), "%d", sp.slice);
                        d.text(VX, y, vb, pal::FG);
                    }
                    break;
                case SR_START: {
                    int pct = (int)((uint64_t)sp.start * 1000 / fx::Q15_ONE);
                    std::snprintf(vb, sizeof(vb), "%d.%d%%", pct / 10, pct % 10);
                    d.text(VX, y, vb, pal::FG);
                    break;
                }
                case SR_LENGTH: {
                    int pct = (int)((uint64_t)sp.length * 1000 / fx::Q15_ONE);
                    std::snprintf(vb, sizeof(vb), "%d.%d%%", pct / 10, pct % 10);
                    d.text(VX, y, vb, pal::FG);
                    break;
                }
                case SR_LOOP_S: {
                    bool loops = (sp.play_mode == synth::PlayMode::FwdLoop ||
                                  sp.play_mode == synth::PlayMode::RevLoop);
                    std::snprintf(vb, sizeof(vb), "%u", (unsigned)smp.loop_start);
                    d.text(VX, y, vb, loops ? pal::FG : pal::FG_DIM);
                    break;
                }
                case SR_LOOP_E: {
                    bool loops = (sp.play_mode == synth::PlayMode::FwdLoop ||
                                  sp.play_mode == synth::PlayMode::RevLoop);
                    std::snprintf(vb, sizeof(vb), "%u", (unsigned)smp.loop_end);
                    d.text(VX, y, vb, loops ? pal::FG : pal::FG_DIM);
                    break;
                }
                case SR_DETUNE:
                    std::snprintf(vb, sizeof(vb), "%+d ct", (int)sp.fine_cents);
                    d.text(VX, y, vb, sp.fine_cents ? pal::FG : pal::FG_DIM);
                    break;
                case SR_ATTACK:
                    std::snprintf(vb, sizeof(vb), "%u", (unsigned)sp.attack);
                    d.text(VX, y, vb, pal::FG);
                    break;
                case SR_RELEASE:
                    std::snprintf(vb, sizeof(vb), "%u", (unsigned)sp.release);
                    d.text(VX, y, vb, pal::FG);
                    break;
                case SR_TABLE:
                    if (inst.table_id == seq::EMPTY)
                        d.text(VX, y, "--", pal::FG_DIM);
                    else {
                        std::snprintf(vb, sizeof(vb), "%02X", inst.table_id);
                        d.text(VX, y, vb, pal::FG);
                    }
                    break;
            }
        }
        // (KB/SLICE/LOAD live on the bottom-screen tabs; ZR = REC, START = play/stop.)
        // the FX defaults strip is drawn below.
        draw_fx_section(d, inst);
        return;
    }

    // normal layout (Wavsynth / Sampler / None)
    struct Field { const char* name; };
    static const Field fields[] = {
        {"TYPE  "},
        {"OSC/SL"},
        {"ATTACK"},
        {"DECAY "},
        {"SUSTN "},
        {"RELS  "},
        {"TABLE "},
        {"POLY  "},
    };

    for (int i = 0; i < 8; ++i) {
        int y = Y0 + 36 + i * ROW_H;
        // for Sampler row 4 (SUSTN) is renamed FINE (fine-tune in cents)
        const char* fname = fields[i].name;
        if (i == 4 && inst.type == seq::InstrumentType::Sampler) fname = "FINE  ";
        d.text(40, y, fname, pal::HEADER);

        const int VX = 130;
        switch (i) {
            case 0:
                d.text(VX, y, type_names[(int)inst.type], pal::FG);
                break;
            case 1:
                if (inst.type == seq::InstrumentType::Wavsynth) {
                    char pbuf[32];
                    const int user_n = synth::WavetableBank::instance().count();
                    const int total = synth::WAVE_PRESET_COUNT + user_n;
                    if (inst.wavsynth.shape == synth::WaveShape::User) {
                        std::snprintf(pbuf, sizeof(pbuf), "%d/%d USR:%s",
                            wav_preset_idx_ + 1, total,
                            synth::WavetableBank::instance().name(inst.wavsynth.user_slot));
                        d.text(VX, y, pbuf, pal::CURSOR);
                    } else {
                        std::snprintf(pbuf, sizeof(pbuf), "%d/%d %s",
                            wav_preset_idx_ + 1, total,
                            synth::wave_preset_name((synth::WavePreset)(wav_preset_idx_ < synth::WAVE_PRESET_COUNT ? wav_preset_idx_ : 0)));
                        d.text(VX, y, pbuf, pal::FG_HEX);
                    }
                } else if (inst.type == seq::InstrumentType::Sampler) {
                    std::snprintf(buf, sizeof(buf), "SLOT %02d", inst.sampler.sample_slot);
                    d.text(VX, y, buf, pal::FG);
                } else {
                    d.text(VX, y, "--", pal::FG_DIM);
                }
                break;
            case 2: {
                uint32_t a = (inst.type == seq::InstrumentType::Wavsynth)
                           ? inst.wavsynth.attack : inst.sampler.attack;
                std::snprintf(buf, sizeof(buf), "%5u", (unsigned)a);
                d.text(VX, y, buf, pal::FG);
                break;
            }
            case 3:
                if (inst.type == seq::InstrumentType::Wavsynth) {
                    std::snprintf(buf, sizeof(buf), "%5u", (unsigned)inst.wavsynth.decay);
                    d.text(VX, y, buf, pal::FG);
                } else d.text(VX, y, "--", pal::FG_DIM);
                break;
            case 4:
                if (inst.type == seq::InstrumentType::Wavsynth) {
                    std::snprintf(buf, sizeof(buf), "%5d", (int)inst.wavsynth.sustain);
                    d.text(VX, y, buf, pal::FG);
                } else if (inst.type == seq::InstrumentType::Sampler) {
                    // FINE: +/-50 cents, shown with sign
                    std::snprintf(buf, sizeof(buf), "%+5d", (int)inst.sampler.fine_cents);
                    d.text(VX, y, buf, inst.sampler.fine_cents != 0 ? pal::FG : pal::FG_DIM);
                } else d.text(VX, y, "--", pal::FG_DIM);
                break;
            case 5: {
                uint32_t r = (inst.type == seq::InstrumentType::Wavsynth)
                           ? inst.wavsynth.release : inst.sampler.release;
                std::snprintf(buf, sizeof(buf), "%5u", (unsigned)r);
                d.text(VX, y, buf, pal::FG);
                break;
            }
            case 6: {
                if (inst.table_id == seq::EMPTY) {
                    d.text(VX, y, "NONE", pal::FG_DIM);
                } else {
                    std::snprintf(buf, sizeof(buf), "%02X", inst.table_id);
                    d.text(VX, y, buf, pal::CURSOR);
                }
                break;
            }
            case 7: {
                d.text(VX, y, inst.poly ? "ON" : "OFF",
                       inst.poly ? pal::PLAY : pal::FG_DIM);
                break;
            }
        }

        if (i == inst_row_) {
            d.rect(36, y - 2, 200, 1, pal::CURSOR);
            d.rect(36, y + 10, 200, 1, pal::CURSOR);
        }
    }

    // FX defaults strip (wavsynth / none fall-through)
    draw_fx_section(d, inst);
}

// === ADSR envelope popup (DoubleSprattt request) ===
// small window pinned bottom-right of the top screen, shown ONLY while the
// cursor sits on an envelope param. linear segments - honest: every EG in the
// engine is linear. the stage being edited glows in the cursor color.
// serum-style extras: slide-in animation, time grid, and a LIVE dot riding
// the curve while a note is sounding (ui_env_stage/level voice introspection).
void App::draw_env_popup(Draw& d, uint32_t atk, uint32_t dec, fx::q15 sus,
                         uint32_t rel, int focus, const char* title,
                         int live_stage, fx::q15 live_level) {
    // slide-in: rises 12px and settles over 6 frames after (re)appearing
    uint32_t t = frame_ - env_popup_frame_;
    int dy = (t >= 6) ? 0 : (int)(6 - t) * 2;

    constexpr int PX = 262, PW = 134, PH = 76;
    const int PY = 142 + dy;
    d.rect(PX, PY, PW, PH, pal::PANEL);
    d.corner_brackets(PX, PY, PW, PH, pal::HEADER, 4, 1);
    d.text(PX + 4, PY + 3, title, pal::HEADER);
    static const char* stage_names[4] = { "ATK", "DEC", "SUS", "REL" };
    if (focus >= 0 && focus < 4)
        d.text(PX + PW - 4 - 18, PY + 3, stage_names[focus], pal::CURSOR);

    // plot area
    const int X0 = PX + 6, Yt = PY + 15, W = PW - 12, H = PH - 22;
    const int YB = Yt + H;                        // baseline (env = 0)

    // serum-ish grid: faint verticals every quarter + midline
    for (int i = 1; i < 4; ++i)
        d.rect(X0 + W * i / 4, Yt, 1, H, with_alpha(pal::GRID, 70));
    d.rect(X0, Yt + H / 2, W, 1, with_alpha(pal::GRID, 50));

    // time -> px, saturating around 1s (32000 frames @32k). min 5px so a zero
    // attack still reads as a stage.
    auto seg_w = [](uint32_t tt) -> int {
        return (int)(5 + (uint64_t)tt * 40 / ((uint64_t)tt + 16000));
    };
    int wa = seg_w(atk), wd = seg_w(dec), wr = seg_w(rel);
    int ws = 16;                                  // sustain shelf: fixed
    int total = wa + wd + wr + ws;
    wa = wa * (W - ws) / (total - ws);
    wd = wd * (W - ws) / (total - ws);
    wr = W - ws - wa - wd;
    int ys = YB - (int)((uint64_t)sus * H / fx::Q15_ONE);

    // draw a linear segment as 1px columns connected vertically (no line prim)
    auto seg = [&](int x0, int y0, int x1, int y1, bool hot) {
        Color c = hot ? pal::CURSOR : pal::PLAY;
        int n = x1 - x0; if (n < 1) n = 1;
        int prev = y0;
        for (int i = 0; i <= n; ++i) {
            int x = x0 + i;
            int y = y0 + (y1 - y0) * i / n;
            int a = y < prev ? y : prev;
            int b = y < prev ? prev : y;
            d.rect(x, a, 1, b - a + 1, c);
            if (hot && a > Yt) d.rect(x, a - 1, 1, 1, c);   // 2px when focused
            prev = y;
        }
    };
    // sustain level guide (dotted)
    for (int x = X0; x < X0 + W; x += 4)
        d.rect(x, ys, 2, 1, pal::GRID);

    int x = X0;
    const int xa = x, xd = x + wa, xs = x + wa + wd, xr = x + wa + wd + ws;
    seg(xa, YB, xd, Yt,     focus == 0);   // attack
    seg(xd, Yt, xs, ys,     focus == 1);   // decay
    seg(xs, ys, xr, ys,     focus == 2);   // sustain
    seg(xr, ys, X0 + W, YB, focus == 3);   // release
    // gate-off tick at the sustain/release seam
    d.rect(xr, Yt, 1, H, with_alpha(pal::FG_DIM, 120));

    // === LIVE dot: current env position while a note sounds ===
    // stage: 1=atk 2=dec 3=sus 4=rel. y comes straight from the level (always
    // exact); x is derived from the level's progress inside the stage.
    if (live_stage >= 1 && live_stage <= 4) {
        int32_t lv = live_level; if (lv < 0) lv = 0; if (lv > fx::Q15_ONE) lv = fx::Q15_ONE;
        int lx;
        switch (live_stage) {
            case 1:  // attack: 0 -> ONE
                lx = xa + (int)((int64_t)wa * lv / fx::Q15_ONE);
                break;
            case 2: {  // decay: ONE -> sus
                int32_t span = fx::Q15_ONE - (int32_t)sus;
                int32_t p = span > 0 ? (fx::Q15_ONE - lv) * 100 / span : 100;
                if (p < 0) p = 0; if (p > 100) p = 100;
                lx = xd + wd * p / 100;
                break;
            }
            case 3:  // sustain shelf: park mid-shelf
                lx = xs + ws / 2;
                break;
            default: {  // release: level -> 0 (approx from sustain as reference)
                int32_t ref = sus > 0 ? sus : fx::Q15_ONE;
                int32_t p = 100 - (int32_t)((int64_t)lv * 100 / ref);
                if (p < 0) p = 0; if (p > 100) p = 100;
                lx = xr + wr * p / 100;
                break;
            }
        }
        int ly = YB - (int)((int64_t)lv * H / fx::Q15_ONE);
        if (lx < X0) lx = X0; if (lx > X0 + W - 1) lx = X0 + W - 1;
        if (ly < Yt) ly = Yt; if (ly > YB) ly = YB;
        // glow halo + bright core (breathing slightly)
        uint8_t br = breathe_pulse(frame_, 24);
        d.rect(lx - 2, ly - 2, 5, 5, with_alpha(pal::CURSOR, (uint8_t)(60 + (br >> 2))));
        d.rect(lx - 1, ly - 1, 3, 3, pal::FLASH);
        // level readout tick on the right edge
        d.rect(X0 + W + 1, ly, 3, 2, pal::FLASH);
    }
}

// dispatcher: decide from the cursor position whether an envelope is being
// edited and which stage has focus. called after draw_instrument (on top).
void App::draw_env_overlay(Draw& d) {
    const auto& inst = project_.instruments[cur_inst_];

    // find the freshest sounding voice of this instrument (preview track 0
    // first, then sequencer tracks) - powers the live dot on the curve.
    auto live = [&](int idx, int& st, fx::q15& lv) {
        st = -1; lv = 0;
        for (int t = 0; t < audio::NUM_TRACKS; ++t) {
            auto& tr = mixer_.track(t);
            for (int k = 0; k < audio::TRACK_POLY; ++k) {
                auto* v = tr.voices[k];
                if (!v || !v->active() || v->inst_id != cur_inst_) continue;
                int s = v->ui_env_stage(idx);
                if (s > 0) { st = s; lv = v->ui_env_level(idx); return; }
            }
        }
    };

    bool shown = false;
    int st; fx::q15 lv;
    switch (inst.type) {
        case seq::InstrumentType::Wavsynth:
            // basic layout rows 2..5 = ATK/DEC/SUS/REL
            if (inst_row_ >= 2 && inst_row_ <= 5) {
                shown = true; env_anim_latch();
                live(0, st, lv);
                draw_env_popup(d, inst.wavsynth.attack, inst.wavsynth.decay,
                               inst.wavsynth.sustain, inst.wavsynth.release,
                               inst_row_ - 2, "ENV", st, lv);
            }
            break;
        case seq::InstrumentType::Sampler:
            if (inst_panel_ == InstPanel::Kb &&
                (inst_row_ == SR_ATTACK || inst_row_ == SR_RELEASE)) {
                shown = true; env_anim_latch();
                live(0, st, lv);
                // sampler carries a full ADSR in params (editor exposes A/R;
                // D/S default to 0/ONE) - draw the real thing
                draw_env_popup(d, inst.sampler.attack, inst.sampler.decay,
                               inst.sampler.sustain, inst.sampler.release,
                               inst_row_ == SR_ATTACK ? 0 : 3, "ENV", st, lv);
            }
            break;
        case seq::InstrumentType::FmSynth:
            // op rows 6..9, sub-cols 3..6 = ATK/DEC/SUS/REL of that operator
            if (inst_row_ >= 6 && inst_row_ <= 9 && inst_col_ >= 3 && inst_col_ <= 6) {
                shown = true; env_anim_latch();
                const auto& op = inst.fm.ops[inst_row_ - 6];
                live(inst_row_ - 6, st, lv);
                char t[10];
                std::snprintf(t, sizeof(t), "OP%d ENV", inst_row_ - 5);
                draw_env_popup(d, op.attack, op.decay,
                               (fx::q15)((int)op.sustain * fx::Q15_ONE / 127),
                               op.release, inst_col_ - 3, t, st, lv);
            }
            break;
        case seq::InstrumentType::DsnSynth:
            if (inst_row_ >= DR_EG1_A && inst_row_ <= DR_EG1_R) {
                shown = true; env_anim_latch();
                live(0, st, lv);
                draw_env_popup(d, inst.dsn.eg1_attack, inst.dsn.eg1_decay,
                               inst.dsn.eg1_sustain, inst.dsn.eg1_release,
                               inst_row_ - DR_EG1_A, "EG1", st, lv);
            } else if (inst_row_ >= DR_EG2_A && inst_row_ <= DR_EG2_R) {
                shown = true; env_anim_latch();
                live(1, st, lv);
                draw_env_popup(d, inst.dsn.eg2_attack, inst.dsn.eg2_decay,
                               inst.dsn.eg2_sustain, inst.dsn.eg2_release,
                               inst_row_ - DR_EG2_A, "EG2", st, lv);
            }
            break;
        default: break;
    }
    if (!shown) env_popup_on_ = false;   // popup hidden - rearm the slide-in
}

} // namespace trackr::ui
