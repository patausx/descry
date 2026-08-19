#include "../app.h"
#include "../ui_internal.h"
#include "../../audio/fixed.h"
#include "../../synth/sample_utils.h"
#include "../../synth/sampler.h"
#include "../../synth/wavetable.h"
#include "../../synth/wavsynth.h"
#include "../../synth/wave_presets.h"
#include <cstdio>
#include <cstring>

namespace trackr::ui {

namespace {
void set_status(char* dst, const char* msg) { std::snprintf(dst, 40, "%s", msg); }
}

int App::rec_target_slot() const {
    if (screen_ == Screen::Instrument) {
        const auto& inst = project_.instruments[cur_inst_];
        if (inst.type == seq::InstrumentType::Sampler) return inst.sampler.sample_slot;
        if (inst.type == seq::InstrumentType::DrumKit && inst_row_ >= 3) {
            int pad = inst_row_ - 3;
            // sliced kits record back into their shared source sample
            if (inst.drumkit.sliced()) return inst.drumkit.sliced_sample();
            uint8_t slot = inst.drumkit.slots[pad];
            if (slot != 0xFF && slot < synth::SAMPLE_BANK_SIZE) return slot;
        }
    }
    return cur_sample_;
}

void App::on_rec_done(int slot) {
    if (screen_ == Screen::Instrument) {
        auto& inst = project_.instruments[cur_inst_];
        if (inst.type == seq::InstrumentType::DrumKit && inst_row_ >= 3 &&
            inst.drumkit.sliced_sample_enc == 0) {
            int pad = inst_row_ - 3;
            inst.drumkit.slots[pad] = (uint8_t)slot;
        }
    }
    cur_sample_ = (uint8_t)slot;
    smp_slice_view_slot_ = -1;  // new recording: reopen the whole waveform
    smp_wave_view_slot_ = -1;
    mark_project_dirty();
}

int App::make_wavetable_from_sample(int sample_slot) {
    const auto& src = synth::SampleBank::instance().slot(sample_slot);
    if (src.empty()) { set_status(smp_status_, "NO SAMPLE"); return -1; }
    const auto& sp = project_.instruments[cur_inst_].sampler;
    const uint32_t total = src.num_frames();
    uint32_t begin = (uint32_t)(((uint64_t)sp.start * total) >> 15);
    uint32_t end = begin + (uint32_t)(((uint64_t)sp.length * total) >> 15);
    if (end > total) end = total;
    // Snap both edges across a wider musical window than normal trim dragging:
    // captured regions often span many cycles, and matching low-amplitude seams
    // matters more than preserving the exact pixel-selected frame.
    const uint32_t radius = (end - begin) / 16 > 512 ? 512 : (end - begin) / 16;
    begin = find_zero_crossing_near(src, begin, radius);
    if (end < total) end = find_zero_crossing_near(src, end, radius);
    if (end <= begin + 1) { set_status(smp_status_, "WINDOW TOO SHORT"); return -1; }

    fx::q15 cycle[synth::WavetableBank::SIZE];
    if (!synth::WavetableBank::prepare_capture(src, begin, end, cycle)) {
        set_status(smp_status_, "WT: SILENT WINDOW"); return -1;
    }

    auto& bank = synth::WavetableBank::instance();
    char wt_name[20];
    int capture_count = 0;
    for (int slot = synth::WavetableBank::FILE_SLOTS; slot < synth::WavetableBank::SLOTS; ++slot)
        if (bank.occupied(slot)) ++capture_count;
    std::snprintf(wt_name, sizeof(wt_name), "CAP%02d-%02d", sample_slot, capture_count + 1);
    int wt_slot;
    {
        // Existing Wavsynth voices read the bank concurrently. Install is only
        // 2KB, so serialize the short copy; preparation and file I/O stay out.
        audio::Mixer::LockGuard _g(mixer_);
        wt_slot = bank.install_capture(cycle, wt_name);
    }
    if (wt_slot < 0) { set_status(smp_status_, "WT CAPTURE BANK FULL"); return -1; }
    if (!bank.save_captures("sdmc:/3ds/descry/wavetable")) {
        audio::Mixer::LockGuard _g(mixer_);
        bank.remove_capture(wt_slot);
        set_status(smp_status_, "WT SAVE FAILED"); return -1;
    }

    int inst_id = -1;
    for (int i = 1; i < seq::MAX_INSTRUMENTS; ++i) {
        if (project_.instruments[i].type == seq::InstrumentType::None) { inst_id = i; break; }
    }
    if (inst_id < 0) { set_status(smp_status_, "WT OK / NO FREE INST"); return -1; }
    auto& inst = project_.instruments[inst_id];
    inst.type = seq::InstrumentType::Wavsynth;
    inst.wavsynth = synth::WavsynthParams{};
    inst.wavsynth.shape = synth::WaveShape::User;
    inst.wavsynth.user_slot = (uint8_t)wt_slot;
    inst.poly = true;
    std::snprintf(inst.name, sizeof(inst.name), "%.15s", bank.name(wt_slot));
    cur_inst_ = (uint8_t)inst_id;
    wav_preset_idx_ = synth::WAVE_PRESET_COUNT + bank.index_of_slot(wt_slot);
    std::snprintf(smp_status_, sizeof(smp_status_), "WT %02d > INST %02X", wt_slot, inst_id);
    mark_project_dirty();
    return inst_id;
}

int App::make_sampler_inst_from_sample(int sample_slot) {
    auto& s = synth::SampleBank::instance().slot(sample_slot);
    if (s.empty()) { set_status(smp_status_, "NO SAMPLE"); return -1; }
    for (int i = 1; i < seq::MAX_INSTRUMENTS; ++i) {
        auto& it = project_.instruments[i];
        if (it.type == seq::InstrumentType::Sampler && it.sampler.sample_slot == sample_slot) {
            std::snprintf(smp_status_, sizeof(smp_status_), "INST %02X EXISTS", i);
            return i;
        }
    }
    int free_id = -1;
    for (int i = 1; i < seq::MAX_INSTRUMENTS; ++i)
        if (project_.instruments[i].type == seq::InstrumentType::None) { free_id = i; break; }
    if (free_id < 0) { set_status(smp_status_, "NO FREE INST"); return -1; }
    auto& inst = project_.instruments[free_id];
    inst.type = seq::InstrumentType::Sampler;
    inst.sampler = synth::SamplerParams{};
    inst.sampler.sample_slot = sample_slot;
    inst.sampler.length = fx::Q15_ONE;
    inst.sampler.release = 4000;
    std::snprintf(inst.name, sizeof(inst.name), "smp%02d", sample_slot);
    std::snprintf(smp_status_, sizeof(smp_status_), "SAMPLER > INST %02X", free_id);
    mark_project_dirty();
    return free_id;
}

// Create/rebuild a zero-copy sliced DrumKit: all 16 pads reference the original
// sample and select a sorted slice. No SampleBank slots are allocated.
int App::make_kit_from_sample(int sample_slot) {
    auto& s = synth::SampleBank::instance().slot(sample_slot);
    if (s.empty()) { set_status(smp_status_, "NO SAMPLE"); return -1; }

    uint32_t sorted[synth::Sample::MAX_CHOPS];
    int n = synth::sample_chops_sorted(s, sorted);
    if (n == 0) {
        synth::sample_auto_slice(s, 16);
        n = synth::sample_chops_sorted(s, sorted);
    }
    if (n == 0) { set_status(smp_status_, "SLICE FAILED"); return -1; }
    if (n > synth::DRUMKIT_PADS) n = synth::DRUMKIT_PADS;

    char expect[16];
    std::snprintf(expect, sizeof(expect), "chop %02d", sample_slot);
    int kit_id = -1;
    bool rebuild = false;
    for (int i = 1; i < seq::MAX_INSTRUMENTS; ++i) {
        auto& it = project_.instruments[i];
        if (it.type == seq::InstrumentType::DrumKit &&
            std::strncmp(it.name, expect, sizeof(expect)) == 0) {
            kit_id = i; rebuild = true; break;
        }
    }
    if (!rebuild) {
        for (int i = 1; i < seq::MAX_INSTRUMENTS; ++i)
            if (project_.instruments[i].type == seq::InstrumentType::None) { kit_id = i; break; }
    }
    if (kit_id < 0) { set_status(smp_status_, "NO FREE INST"); return -1; }

    auto& kit = project_.instruments[kit_id];
    kit.type = seq::InstrumentType::DrumKit;
    kit.drumkit = synth::DrumKitParams{};
    kit.drumkit.base_note = 60;
    kit.drumkit.set_sliced_sample(sample_slot);
    std::snprintf(kit.name, sizeof(kit.name), "chop %02d", sample_slot);
    std::snprintf(smp_status_, sizeof(smp_status_), "%s KIT %02X: %d PADS",
                  rebuild ? "REBUILT" : "NEW", kit_id, n);
    cur_inst_ = (uint8_t)kit_id;
    mark_project_dirty();
    return kit_id;
}

// Quantize sorted slice marker positions to a full 16-step phrase.
int App::spread_slices_to_phrase(int sample_slot) {
    auto& s = synth::SampleBank::instance().slot(sample_slot);
    if (s.empty()) { set_status(smp_status_, "NO SAMPLE"); return -1; }
    uint32_t total = s.num_frames();
    if (!total) return -1;
    uint32_t sorted[synth::Sample::MAX_CHOPS];
    int n = synth::sample_chops_sorted(s, sorted);
    if (!n) { synth::sample_auto_slice(s, 16); n = synth::sample_chops_sorted(s, sorted); }
    if (!n) return -1;

    auto& inst = project_.instruments[cur_inst_];
    if (inst.type != seq::InstrumentType::Sampler) return -1;
    inst.sampler.chromatic_slices = true;
    inst.sampler.slice = 0;
    auto& ph = project_.phrases[cur_phrase_];
    for (int k = 0; k < seq::PHRASE_STEPS; ++k) {
        ph.steps[k].note = seq::EMPTY;
        ph.steps[k].instrument = seq::EMPTY;
    }
    int placed = 0;
    for (int k = 0; k < n; ++k) {
        int step = (int)(((uint64_t)sorted[k] * seq::PHRASE_STEPS + total / 2) / total);
        if (step >= seq::PHRASE_STEPS) step = seq::PHRASE_STEPS - 1;
        if (ph.steps[step].note != seq::EMPTY) continue;
        ph.steps[step].note = (uint8_t)(s.root_note + k);
        ph.steps[step].instrument = cur_inst_;
        ph.steps[step].velocity = 0x7F;
        ++placed;
    }
    ph.length = seq::PHRASE_STEPS;
    std::snprintf(smp_status_, sizeof(smp_status_), "%d/%d SLICES > PHR %02X", placed, n, cur_phrase_);
    mark_project_dirty();
    return placed;
}

void App::shuffle_phrase_steps() {
    auto& ph = project_.phrases[cur_phrase_];
    int len = seq::phrase_len(ph);
    if (len < 2) return;
    seq::Phrase before = ph;
    static uint32_t rng = 0;
    rng ^= frame_ * 2654435761u;
    if (!rng) rng = 0x9E3779B9u;
    auto next = [&]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
    for (int i = len - 1; i > 0; --i) {
        int j = (int)(next() % (uint32_t)(i + 1));
        if (i == j) continue;
        snapshot_step(i);
        seq::PhraseStep tmp = ph.steps[i]; ph.steps[i] = ph.steps[j]; commit_step(i);
        snapshot_step(j); ph.steps[j] = tmp; commit_step(j);
    }
    std::snprintf(smp_status_, sizeof(smp_status_), "SHUFFLED PHR %02X", cur_phrase_);
    uint16_t mask = 0;
    for (int r = 0; r < len; ++r)
        if (std::memcmp(&before.steps[r], &ph.steps[r], sizeof(seq::PhraseStep)) != 0)
            mask |= (uint16_t)(1u << r);
    start_phrase_motion(mask, 1);
    mark_project_dirty();
}

} // namespace trackr::ui
