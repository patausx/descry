#include "serialize.h"
#include "scale.h"
#include <cstring>
#include <cstdio>
#include <new>

namespace trackr::seq {

bool save_project(const Project& p, const char* path) {
    // write to a temp file first and rename over the target only when every
    // byte landed. a full/yanked SD card used to leave a HALF-WRITTEN project
    // at the real path while the UI reported success - the user found out at
    // the next load, long after the good data was gone.
    char tmp_path[128];
    std::snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE* f = std::fopen(tmp_path, "wb");
    if (!f) return false;

    ProjectFileHeader h{};
    h.magic   = PROJECT_MAGIC;
    h.version = PROJECT_VERSION;
    h.project_size = sizeof(Project);

    bool ok = std::fwrite(&h, sizeof(h), 1, f) == 1
           && std::fwrite(&p, sizeof(Project), 1, f) == 1;
    // fclose flushes - it can be the call that hits the "disk full" wall,
    // so its result is part of the success check.
    ok = (std::fclose(f) == 0) && ok;

    if (!ok) { std::remove(tmp_path); return false; }
    // rename is atomic-enough on FAT: either the old file survives or the
    // complete new one is in place. never a torn half of each.
    std::remove(path);
    return std::rename(tmp_path, path) == 0;
}

bool load_project(Project& p, const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;

    ProjectFileHeader h{};
    if (std::fread(&h, sizeof(h), 1, f) != 1) { std::fclose(f); return false; }
    if (h.magic != PROJECT_MAGIC) { std::fclose(f); return false; }
    // exact current version OR an older one whose blob is a prefix of today's
    // Project (new fields are only ever APPENDED - see project.h tail comment)
    bool ok = (h.version == PROJECT_VERSION && h.project_size == sizeof(Project))
           || (h.version >= PROJECT_VERSION_MIN && h.version < PROJECT_VERSION &&
               h.project_size <= sizeof(Project));
    if (!ok) { std::fclose(f); return false; }

    // read into a heap temp: a short read (truncated file, dying SD) must not
    // leave the CALLER's project half-old half-new - especially since the
    // caller is usually the live project the audio thread is playing from.
    // heap, not stack: Project is ~62KB and __stacksize__ is only 512K.
    auto* tmp = new (std::nothrow) Project();
    if (!tmp) { std::fclose(f); return false; }

    std::size_t n = h.project_size < sizeof(Project) ? h.project_size : sizeof(Project);
    if (std::fread(tmp, n, 1, f) != 1) { std::fclose(f); delete tmp; return false; }
    if (n < sizeof(Project)) {
        // zero-fill the appended tail (table_speed 0 = legacy default = 1)
        std::memset(reinterpret_cast<char*>(tmp) + n, 0, sizeof(Project) - n);
    }
    std::fclose(f);

    // never hand the player raw bytes from disk - clamp anything OOB-capable
    validate_project(*tmp);

    p = *tmp;
    delete tmp;
    return true;
}

// clamp every field the player/engine indexes or divides by. the file passed
// magic+version, but bitrot / manual edits / a torn write from an old build
// can still put garbage anywhere. philosophy: fix and keep playing - a track
// with one reset step beats a refused (or crashing) load.
int validate_project(Project& p) {
    int fixed = 0;

    // --- song globals ---
    auto& song = p.song;
    if (song.bpm < 20) { song.bpm = 120; ++fixed; }        // 0 = div-by-zero in the tick math
    if (song.swing > 50) { song.swing = 0; ++fixed; }
    if (song.scale_root >= 12) { song.scale_root = 0; ++fixed; }
    if (song.scale_type >= SCALE_COUNT) { song.scale_type = 0; ++fixed; }
    if (song.duck_src != EMPTY && song.duck_src >= NUM_TRACKS) { song.duck_src = EMPTY; ++fixed; }
    // groove: 0 is only valid inside groove_steps (end marker); the scalar
    // fallback must never be 0 ticks/step
    if (song.groove == 0 || song.groove > 32) { song.groove = TICKS_PER_STEP; ++fixed; }
    for (auto& g : song.groove_steps)
        if (g > 32) { g = 0; ++fixed; }

    // --- phrase steps: the only place a uint8 can actually index OOB ---
    // (chains/song cells are uint8 into 256-sized banks - can't escape; but
    // instruments[] is 128 entries, so a step's instrument byte can.)
    for (auto& ph : p.phrases) {
        if (ph.length > PHRASE_STEPS) { ph.length = PHRASE_STEPS; ++fixed; }
        for (auto& st : ph.steps) {
            if (st.note != EMPTY && st.note > 127) { st.note = EMPTY; ++fixed; }
            if (st.instrument != EMPTY && st.instrument >= MAX_INSTRUMENTS) {
                st.instrument = EMPTY; ++fixed;
            }
            if (st.velocity > 127) { st.velocity = 127; ++fixed; }
        }
    }

    // --- instruments ---
    for (auto& inst : p.instruments) {
        if ((uint8_t)inst.type >= INSTRUMENT_TYPE_COUNT) {
            // unknown type -> make_voice would static_cast into the wrong union
            // member. neutralize the whole instrument.
            inst = Instrument{};
            ++fixed;
            continue;
        }
        if (inst.table_id != EMPTY && inst.table_id >= MAX_TABLES) {
            inst.table_id = EMPTY; ++fixed;
        }
        if (inst.type == InstrumentType::Sampler) {
            auto& sp = inst.sampler;
            if (sp.sample_slot < 0 || sp.sample_slot >= synth::SAMPLE_BANK_SIZE) {
                sp.sample_slot = 0; ++fixed;
            }
            if (sp.slice > synth::Sample::MAX_CHOPS) { sp.slice = 0; ++fixed; }
            if (sp.sync_bars > 8) { sp.sync_bars = 0; ++fixed; }
            // bar_frames is recomputed from Song on every make_voice; a stale
            // value from disk must never reach the rate math
            sp.bar_frames = 0;
            if ((uint8_t)sp.play_mode >= (uint8_t)synth::PlayMode::Count) {
                sp.play_mode = synth::PlayMode::Fwd; ++fixed;
            }
        }
        if (inst.type == InstrumentType::DrumKit) {
            auto& dk = inst.drumkit;
            if (dk.sliced_sample_enc > synth::SAMPLE_BANK_SIZE) {
                dk.sliced_sample_enc = 0; ++fixed;
            }
            for (auto& sl : dk.slots)
                if (sl != 0xFF && sl >= synth::SAMPLE_BANK_SIZE) { sl = 0xFF; ++fixed; }
        }
    }

    // --- table speeds (appended tail; 0 = legacy = 1 is fine, cap the top) ---
    for (auto& ts : p.table_speed)
        if (ts > 16) { ts = 16; ++fixed; }

    // --- v13 tail: mute mask only has NUM_TRACKS meaningful bits ---
    {
        const uint8_t valid = (uint8_t)((1u << NUM_TRACKS) - 1);
        if (p.track_mute & (uint8_t)~valid) { p.track_mute &= valid; ++fixed; }
    }

    return fixed;
}

} // namespace trackr::seq
