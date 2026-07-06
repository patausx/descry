// App: sample-related helpers that survive the Sample-screen removal.
// Owns: rec_target_slot / on_rec_done (mic recording targets) and the
// make-instrument-from-sample factories (used by the SLICE panel's chop->kit).
// The old sp404-style Sample screen was removed - sample editing lives in the
// Instrument view bottom panels now (WAVE / SLICE / LOAD / REC).
#include "../app.h"
#include "../ui_internal.h"
#include "../../audio/fixed.h"
#include "../../synth/sample_utils.h"
#include "../../synth/sampler.h"
#include <cstdio>
#include <cstring>

namespace trackr::ui {

namespace {
// write a status string into the instance buffer
void set_status(char* dst, const char* msg) {
    std::snprintf(dst, 40, "%s", msg);
}
} // anonymous namespace

int App::rec_target_slot() const {
    if (screen_ == Screen::Instrument) {
        const auto& inst = project_.instruments[cur_inst_];
        if (inst.type == seq::InstrumentType::Sampler) {
            return inst.sampler.sample_slot;
        }
        if (inst.type == seq::InstrumentType::DrumKit) {
            // if the cursor is on a pad and it has a slot - write to it
            if (inst_row_ >= 3) {
                int pad = inst_row_ - 3;
                uint8_t slot = inst.drumkit.slots[pad];
                if (slot != 0xFF && slot < synth::SAMPLE_BANK_SIZE) return slot;
            }
        }
    }
    // other views - always cur_sample_
    return cur_sample_;
}

void App::on_rec_done(int slot) {
    if (screen_ == Screen::Instrument) {
        auto& inst = project_.instruments[cur_inst_];
        if (inst.type == seq::InstrumentType::DrumKit && inst_row_ >= 3) {
            int pad = inst_row_ - 3;
            // write slot into the pad (overwrite allowed)
            inst.drumkit.slots[pad] = (uint8_t)slot;
        }
    }
    cur_sample_ = (uint8_t)slot;
}

// === shared helpers for creating instruments from a sample ===

int App::make_sampler_inst_from_sample(int sample_slot) {
    auto& s = synth::SampleBank::instance().slot(sample_slot);
    if (s.empty()) {
        set_status(smp_status_, "NO SAMPLE");
        return -1;
    }
    // search for an existing Sampler pointing at this slot
    for (int i = 1; i < seq::MAX_INSTRUMENTS; ++i) {
        auto& it = project_.instruments[i];
        if (it.type == seq::InstrumentType::Sampler && it.sampler.sample_slot == sample_slot) {
            std::snprintf(smp_status_, sizeof(smp_status_), "INST %02X EXISTS", i);
            return i;
        }
    }
    // find the first empty one
    int free_id = -1;
    for (int i = 1; i < seq::MAX_INSTRUMENTS; ++i) {
        if (project_.instruments[i].type == seq::InstrumentType::None) { free_id = i; break; }
    }
    if (free_id < 0) { set_status(smp_status_, "NO FREE INST"); return -1; }
    auto& inst = project_.instruments[free_id];
    inst.type = seq::InstrumentType::Sampler;
    inst.sampler = synth::SamplerParams{};
    inst.sampler.sample_slot = sample_slot;
    inst.sampler.length = fx::Q15_ONE;
    inst.sampler.release = 4000;
    std::snprintf(inst.name, sizeof(inst.name), "smp%02d", sample_slot);
    std::snprintf(smp_status_, sizeof(smp_status_), "SAMPLER → INST %02X", free_id);
    mark_dirty();
    return free_id;
}

int App::make_kit_from_sample(int sample_slot) {
    auto& s = synth::SampleBank::instance().slot(sample_slot);
    if (s.empty()) {
        set_status(smp_status_, "NO SAMPLE");
        return -1;
    }
    uint32_t total = s.num_frames();
    if (total == 0) { set_status(smp_status_, "NO SAMPLE"); return -1; }

    // search for an existing DrumKit named "chop NN" - reuse instead of duplicating
    char expect_name[16];
    std::snprintf(expect_name, sizeof(expect_name), "chop %02d", sample_slot);
    for (int i = 1; i < seq::MAX_INSTRUMENTS; ++i) {
        auto& it = project_.instruments[i];
        if (it.type == seq::InstrumentType::DrumKit &&
            std::strncmp(it.name, expect_name, sizeof(expect_name)) == 0) {
            cur_inst_ = (uint8_t)i;
            std::snprintf(smp_status_, sizeof(smp_status_), "KIT %02X EXISTS", i);
            return i;
        }
    }

    // if there are no chops - auto-slice 16
    int n = synth::sample_chop_count(s);
    if (n == 0) {
        synth::sample_auto_slice(s, 16);
        n = synth::sample_chop_count(s);
    }
    if (n == 0) { set_status(smp_status_, "SLICE FAILED"); return -1; }

    // find an empty inst
    int kit_id = -1;
    for (int i = 1; i < seq::MAX_INSTRUMENTS; ++i) {
        if (project_.instruments[i].type == seq::InstrumentType::None) { kit_id = i; break; }
    }
    if (kit_id < 0) { set_status(smp_status_, "NO FREE INST"); return -1; }

    // copy each chop region into free bank slots (so pads own their samples)
    const int channels = s.channels ? s.channels : 1;
    uint8_t kit_slots[synth::DRUMKIT_PADS];
    for (int i = 0; i < synth::DRUMKIT_PADS; ++i) kit_slots[i] = 0xFF;

    // collect valid, sorted chop positions
    uint32_t chops_sorted[synth::Sample::MAX_CHOPS];
    int n_chops = 0;
    for (int i = 0; i < synth::Sample::MAX_CHOPS; ++i) {
        if (s.chops[i] != 0xFFFFFFFFu) chops_sorted[n_chops++] = s.chops[i];
    }
    // insertion sort (n<=16)
    for (int i = 1; i < n_chops; ++i) {
        uint32_t v = chops_sorted[i];
        int j = i - 1;
        while (j >= 0 && chops_sorted[j] > v) { chops_sorted[j + 1] = chops_sorted[j]; --j; }
        chops_sorted[j + 1] = v;
    }

    int filled = 0;
    for (int i = 0; i < n_chops && filled < synth::DRUMKIT_PADS; ++i) {
        uint32_t a = chops_sorted[i];
        uint32_t b = (i + 1 < n_chops) ? chops_sorted[i + 1] : total;
        if (b <= a) continue;
        // find a free bank slot
        int slot = -1;
        for (int k = 0; k < synth::SAMPLE_BANK_SIZE; ++k) {
            if (synth::SampleBank::instance().slot(k).data.empty()) { slot = k; break; }
        }
        if (slot < 0) break;
        auto& dst = synth::SampleBank::instance().slot(slot);
        // copy the chunk accounting for channels
        dst.data.assign(s.data.begin() + (size_t)a * channels,
                        s.data.begin() + (size_t)b * channels);
        dst.channels  = (uint8_t)channels;
        dst.root_note = s.root_note;
        dst.loop_start = 0;
        dst.loop_end   = 0;
        for (int c = 0; c < synth::Sample::MAX_CHOPS; ++c) dst.chops[c] = 0xFFFFFFFFu;
        kit_slots[filled++] = (uint8_t)slot;
    }

    auto& kit = project_.instruments[kit_id];
    kit.type = seq::InstrumentType::DrumKit;
    kit.drumkit = synth::DrumKitParams{};
    kit.drumkit.base_note = 60;
    for (int i = 0; i < synth::DRUMKIT_PADS; ++i) kit.drumkit.slots[i] = kit_slots[i];
    std::snprintf(kit.name, sizeof(kit.name), "chop %02d", sample_slot);
    std::snprintf(smp_status_, sizeof(smp_status_), "KIT → INST %02X (%d pads)", kit_id, filled);
    cur_inst_ = (uint8_t)kit_id;
    mark_dirty();
    return kit_id;
}

} // namespace trackr::ui
