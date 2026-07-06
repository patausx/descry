#include "project.h"

namespace trackr::seq {

audio::Voice* Project::make_voice(uint8_t instrument_id) {
    if (instrument_id >= MAX_INSTRUMENTS) return nullptr;
    auto& inst = instruments[instrument_id];
    switch (inst.type) {
        case InstrumentType::Wavsynth: {
            auto* v = new synth::Wavsynth();
            v->params = inst.wavsynth;
            return v;
        }
        case InstrumentType::Sampler: {
            auto* v = new synth::Sampler();
            v->params = inst.sampler;
            return v;
        }
        case InstrumentType::DrumKit: {
            auto* v = new synth::DrumKitVoice();
            v->params = inst.drumkit;
            return v;
        }
        case InstrumentType::FmSynth: {
            auto* v = new synth::FmSynth();
            v->params = inst.fm;
            return v;
        }
        case InstrumentType::DsnSynth: {
            auto* v = new synth::DsnSynth();
            v->params = inst.dsn;
            return v;
        }
        default:
            return nullptr;
    }
}

// === clone helpers (lsdj/m8-style) ===

bool Project::phrase_empty(uint8_t id) const {
    if (id >= MAX_PHRASES) return false;
    for (const auto& s : phrases[id].steps) {
        if (s.note != EMPTY || s.instrument != EMPTY) return false;
        for (const auto& f : s.fx)
            if (f.cmd != 0 || f.value != 0) return false;
    }
    return true;
}

bool Project::chain_empty(uint8_t id) const {
    if (id >= MAX_CHAINS) return false;
    for (const auto& r : chains[id].rows)
        if (r.phrase != EMPTY || r.transpose != 0) return false;
    return true;
}

bool Project::phrase_referenced(uint8_t id) const {
    for (const auto& c : chains)
        for (const auto& r : c.rows)
            if (r.phrase == id) return true;
    return false;
}

bool Project::chain_referenced(uint8_t id) const {
    for (const auto& row : song.rows)
        for (int t = 0; t < NUM_TRACKS; ++t)
            if (row.chain[t] == id) return true;
    return false;
}

int Project::find_free_phrase(uint8_t near_id) const {
    for (int i = 1; i < MAX_PHRASES; ++i) {
        int id = (near_id + i) % MAX_PHRASES;
        if (phrase_empty((uint8_t)id) && !phrase_referenced((uint8_t)id)) return id;
    }
    return -1;
}

int Project::find_free_chain(uint8_t near_id) const {
    for (int i = 1; i < MAX_CHAINS; ++i) {
        int id = (near_id + i) % MAX_CHAINS;
        if (chain_empty((uint8_t)id) && !chain_referenced((uint8_t)id)) return id;
    }
    return -1;
}

int Project::clone_phrase(uint8_t src) {
    if (src >= MAX_PHRASES) return -1;
    int dst = find_free_phrase(src);
    if (dst < 0) return -1;
    phrases[dst] = phrases[src];
    return dst;
}

int Project::clone_chain_deep(uint8_t src) {
    if (src >= MAX_CHAINS) return -1;
    int dst = find_free_chain(src);
    if (dst < 0) return -1;
    chains[dst] = chains[src];

    // remap referenced phrases to fresh copies; the same source phrase
    // appearing in several rows maps to ONE new copy (keeps intra-chain sharing).
    uint8_t map_from[CHAIN_ROWS]; // src phrase ids already cloned
    uint8_t map_to[CHAIN_ROWS];
    int     map_n = 0;
    for (auto& r : chains[dst].rows) {
        if (r.phrase == EMPTY) continue;
        int mapped = -1;
        for (int m = 0; m < map_n; ++m)
            if (map_from[m] == r.phrase) { mapped = map_to[m]; break; }
        if (mapped < 0) {
            mapped = clone_phrase(r.phrase);
            if (mapped < 0) continue;   // bank full: keep the shared original
            map_from[map_n] = r.phrase;
            map_to[map_n]   = (uint8_t)mapped;
            ++map_n;
        }
        r.phrase = (uint8_t)mapped;
    }
    return dst;
}

} // namespace trackr::seq
