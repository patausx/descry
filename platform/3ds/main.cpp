// trackr3ds v5 - full m8-style ui
// all layers green. final build with phrase/chain/song/instrument views,
// hex editing, navigation, mic recording.

#include <3ds.h>
#include <citro2d.h>
#include <cstdio>
#include <cstring>
#include <malloc.h>

#include "../../core/audio/mixer.h"
#include "../../core/synth/wavsynth.h"
#include "../../core/synth/wavetable.h"
#include "../../core/synth/sampler.h"
#include "../../core/synth/mic_recorder.h"
#include "../../core/sequencer/project.h"
#include "../../core/sequencer/player.h"
#include "../../core/sequencer/serialize.h"
#include "../../core/sequencer/fx.h"
#include "../../core/ui/app.h"
#include "../../core/ui/logo.h"
#include "mic_3ds.h"
#include "draw_3ds.h"
#include "audio_ndsp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#include <sys/stat.h>
#include <ctime>

using namespace trackr;

// === sample save/load (raw mono int16) ===
static const char* SAMPLE_DIR = "sdmc:/3ds/descry";
static const char* SESSION_PATH = "sdmc:/3ds/descry/session.tr3d";
static const char* RENDER_PATH = "sdmc:/3ds/descry/render.wav";
static const char* THEME_PATH = "sdmc:/3ds/descry/theme.cfg";      // legacy (v<=1.0.3)
static const char* SETTINGS_PATH = "sdmc:/3ds/descry/settings.cfg";

// settings persistence: tiny binary blob ('DSC1' + Settings POD).
// replaces the old one-byte theme.cfg; that file is still read once as a
// fallback so a 1.0.3 user's theme survives the upgrade.
static bool load_settings(ui::App::Settings& out) {
    FILE* f = std::fopen(SETTINGS_PATH, "rb");
    if (!f) return false;
    char magic[4] = {0};
    bool ok = std::fread(magic, 1, 4, f) == 4 &&
              std::memcmp(magic, "DSC1", 4) == 0 &&
              std::fread(&out, 1, sizeof(out), f) == sizeof(out);
    std::fclose(f);
    return ok;
}
static void save_settings(const ui::App::Settings& s) {
    FILE* f = std::fopen(SETTINGS_PATH, "wb");
    if (!f) return;
    std::fwrite("DSC1", 1, 4, f);
    std::fwrite(&s, 1, sizeof(s), f);
    std::fclose(f);
}

// legacy theme persistence (read-only fallback; settings.cfg replaced it)
static int load_theme_idx() {
    FILE* f = std::fopen(THEME_PATH, "rb");
    if (!f) return 0;
    int c = std::fgetc(f);
    std::fclose(f);
    return (c >= '0' && c <= '9') ? (c - '0') : 0;
}

static void slot_path(int slot, char* buf, std::size_t n) {
    std::snprintf(buf, n, "sdmc:/3ds/descry/project_%02X.tr3d", slot);
}

// globals (moved here because the functions below need them)
static audio::Mixer  g_mixer;
static seq::Project  g_project;

// === WAV writer (16-bit stereo PCM) ===
struct WavHeader {
    char     riff[4];      // "RIFF"
    uint32_t size;         // 36 + data_size
    char     wave[4];      // "WAVE"
    char     fmt[4];       // "fmt "
    uint32_t fmt_size;     // 16
    uint16_t audio_format; // 1 = PCM
    uint16_t channels;     // 2
    uint32_t sample_rate;
    uint32_t byte_rate;    // sr * channels * 2
    uint16_t block_align;  // channels * 2
    uint16_t bits_per_sample; // 16
    char     data[4];      // "data"
    uint32_t data_size;
};

static bool write_wav(const char* path, const int16_t* samples, std::size_t frame_count, int sr) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    WavHeader h;
    std::memcpy(h.riff, "RIFF", 4);
    std::memcpy(h.wave, "WAVE", 4);
    std::memcpy(h.fmt,  "fmt ", 4);
    std::memcpy(h.data, "data", 4);
    h.fmt_size = 16;
    h.audio_format = 1;
    h.channels = 2;
    h.sample_rate = sr;
    h.byte_rate = sr * 2 * 2;
    h.block_align = 2 * 2;
    h.bits_per_sample = 16;
    h.data_size = frame_count * 2 * 2;
    h.size = 36 + h.data_size;
    std::fwrite(&h, sizeof(h), 1, f);
    std::fwrite(samples, 2, frame_count * 2, f);
    std::fclose(f);
    return true;
}

// homebrew thread stack size - was about 32k by default, too small for our fx objects
extern "C" {
    u32 __stacksize__ = 512 * 1024;  // 512 KB
}

// === render to wav (render the whole song) ===
static bool render_song_to_wav() {
    // Mixer + Player on the heap (~80kb total, dangerous on the stack)
    auto* xmix = new audio::Mixer();
    auto* xplayer = new seq::Player(g_project, *xmix);
    xplayer->play_song(0);

    constexpr int SR = 32000;
    constexpr std::size_t MAX_SECONDS = 60;
    constexpr std::size_t CHUNK = 1024;
    std::vector<int16_t> data;
    data.reserve(SR * 2 * 4);

    int16_t buf[CHUNK * 2];
    std::size_t total_frames = 0;
    std::size_t silence_frames = 0;
    while (xplayer->playing() && total_frames < SR * MAX_SECONDS) {
        xplayer->advance(CHUNK, SR);
        xmix->render((fx::q15*)buf, CHUNK);
        data.insert(data.end(), buf, buf + CHUNK * 2);
        total_frames += CHUNK;
        bool any = false;
        for (std::size_t i = 0; i < CHUNK * 2; ++i) if (buf[i] != 0) { any = true; break; }
        if (!any) silence_frames += CHUNK;
        else silence_frames = 0;
        if (silence_frames > SR) break;
    }

    for (int t = 0; t < seq::NUM_TRACKS; ++t) {
        xmix->clear_voices(t);
    }

    delete xplayer;
    delete xmix;

    if (data.empty()) return false;
    return write_wav(RENDER_PATH, data.data(), total_frames, SR);
}

// globals (needed before save/load)
// (moved up)

// === sample file format .s16 ===
// header 64 byte:
//   magic 4   = 'TR3S'
//   version 1 = 1
//   channels 1 = 1 or 2
//   root_note 1
//   reserved 1
//   loop_start 4
//   loop_end   4
//   chops 16*4 = 64 byte... doesn't fit. shift the header.
// make magic+meta = 16 byte, then chops 64 byte = 80 byte total
struct SampleFileHeader {
    uint32_t magic;       // 'TR3S' = 0x53335254
    uint8_t  version;     // 1
    uint8_t  channels;    // 1 or 2
    uint8_t  root_note;
    uint8_t  flags;       // bit0 = reversed (legacy)
    uint32_t loop_start;
    uint32_t loop_end;
};
static_assert(sizeof(SampleFileHeader) == 16, "sample header layout");
constexpr uint32_t SAMPLE_FILE_MAGIC = 0x53335254;  // 'TR3S' little-endian

// === sample dirty tracking ===
// exit autosave used to rewrite EVERY non-empty sample to SD - multi-MB of
// FAT writes = the "15 seconds to close" complaint. hashing RAM is ~1000x
// faster than SD i/o, so we fingerprint each slot at load/save time and skip
// unchanged slots on exit. 0 = "no fingerprint yet" (always writes).
static uint32_t g_sample_hash[synth::SAMPLE_BANK_SIZE] = {0};

static uint32_t sample_fingerprint(const synth::Sample& s) {
    uint32_t h = 2166136261u;
    auto mix = [&h](const void* p, std::size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        for (std::size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
    };
    mix(&s.channels,   sizeof(s.channels));
    mix(&s.root_note,  sizeof(s.root_note));
    mix(&s.reversed,   sizeof(s.reversed));
    mix(&s.loop_start, sizeof(s.loop_start));
    mix(&s.loop_end,   sizeof(s.loop_end));
    mix(s.chops, sizeof(s.chops));
    mix(s.data.data(), s.data.size() * sizeof(int16_t));
    uint32_t r = h ^ (uint32_t)s.data.size();
    return r ? r : 1;   // reserve 0 for "no fingerprint"
}

static void save_sample_to_sd(int slot) {
    auto& s = synth::SampleBank::instance().slot(slot);
    if (s.data.empty()) return;
    mkdir("sdmc:/3ds", 0777);
    mkdir(SAMPLE_DIR, 0777);
    char path[64];
    std::snprintf(path, sizeof(path), "%s/sample_%02d.s16", SAMPLE_DIR, slot);
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    SampleFileHeader h{};
    h.magic       = SAMPLE_FILE_MAGIC;
    h.version     = 1;
    h.channels    = s.channels ? s.channels : 1;
    h.root_note   = (uint8_t)s.root_note;
    h.flags       = s.reversed ? 1 : 0;
    h.loop_start  = s.loop_start;
    h.loop_end    = s.loop_end;
    std::fwrite(&h, sizeof(h), 1, f);
    std::fwrite(s.chops, sizeof(s.chops), 1, f);
    std::fwrite(s.data.data(), sizeof(int16_t), s.data.size(), f);
    std::fclose(f);
    g_sample_hash[slot] = sample_fingerprint(s);   // written = clean
}

static void load_sample_from_sd(int slot) {
    char path[64];
    std::snprintf(path, sizeof(path), "%s/sample_%02d.s16", SAMPLE_DIR, slot);
    FILE* f = std::fopen(path, "rb");
    if (!f) return;
    std::fseek(f, 0, SEEK_END);
    long bytes = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (bytes <= 0 || bytes > 32 * 1024 * 1024) { std::fclose(f); return; }
    auto& s = synth::SampleBank::instance().slot(slot);

    // peek magic
    SampleFileHeader h{};
    std::fread(&h, sizeof(h), 1, f);
    if (h.magic == SAMPLE_FILE_MAGIC && h.version == 1) {
        // new format with a header
        std::fread(s.chops, sizeof(s.chops), 1, f);
        long header_bytes = (long)sizeof(h) + (long)sizeof(s.chops);
        long audio_bytes = bytes - header_bytes;
        if (audio_bytes <= 0) { std::fclose(f); return; }
        s.channels   = h.channels ? h.channels : 1;
        s.root_note  = h.root_note;
        s.reversed   = (h.flags & 1) != 0;
        s.loop_start = h.loop_start;
        s.loop_end   = h.loop_end;
        s.data.resize(audio_bytes / 2);
        std::fread(s.data.data(), 2, s.data.size(), f);
    } else {
        // legacy: raw mono int16, without metadata
        std::fseek(f, 0, SEEK_SET);
        s.channels   = 1;
        s.root_note  = 60;
        s.loop_start = 0;
        s.loop_end   = 0;
        s.reversed   = false;
        for (int i = 0; i < synth::Sample::MAX_CHOPS; ++i) s.chops[i] = 0xFFFFFFFFu;
        s.data.resize(bytes / 2);
        std::fread(s.data.data(), 2, s.data.size(), f);
    }
    std::fclose(f);
    g_sample_hash[slot] = sample_fingerprint(s);   // freshly loaded = clean
}

static void save_full_project() {
    mkdir("sdmc:/3ds", 0777);
    mkdir(SAMPLE_DIR, 0777);
    seq::save_project(g_project, SESSION_PATH);
    // plus all non-empty samples - but ONLY the ones that actually changed
    // since load / last save (fingerprint diff). typical exit: zero SD writes
    // for samples -> app closes near-instantly instead of ~15s.
    for (int i = 0; i < synth::SAMPLE_BANK_SIZE; ++i) {
        auto& s = synth::SampleBank::instance().slot(i);
        if (s.data.empty()) continue;
        if (sample_fingerprint(s) == g_sample_hash[i]) continue;
        save_sample_to_sd(i);
    }
}
static void load_full_project() {
    seq::load_project(g_project, SESSION_PATH);
    for (int i = 0; i < synth::SAMPLE_BANK_SIZE; ++i) {
        load_sample_from_sd(i);
    }
}

// === named slot ops ===
static bool slot_save(int slot) {
    if (slot < 0 || slot >= 16) return false;
    char path[80];
    slot_path(slot, path, sizeof(path));
    mkdir("sdmc:/3ds", 0777);
    mkdir(SAMPLE_DIR, 0777);
    return seq::save_project(g_project, path);
}
static bool slot_load(int slot) {
    if (slot < 0 || slot >= 16) return false;
    char path[80];
    slot_path(slot, path, sizeof(path));
    return seq::load_project(g_project, path);
}
static void slot_delete(int slot) {
    if (slot < 0 || slot >= 16) return;
    char path[80];
    slot_path(slot, path, sizeof(path));
    std::remove(path);
}
static bool slot_exists(int slot) {
    char path[80];
    slot_path(slot, path, sizeof(path));
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}
// read just the project name from a slot file (no full 62kb load).
// name lives at a fixed offset inside the raw Project blob after the header.
static bool slot_peek_name(int slot, char* out, std::size_t n) {
    char path[80];
    slot_path(slot, path, sizeof(path));
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    seq::ProjectFileHeader h{};
    bool ok = false;
    // accept the current version AND older prefix-compatible ones (v10+):
    // `name` sits before the appended tail, so its offset is identical
    if (std::fread(&h, sizeof(h), 1, f) == 1 &&
        h.magic == seq::PROJECT_MAGIC &&
        h.version >= seq::PROJECT_VERSION_MIN && h.version <= seq::PROJECT_VERSION) {
        long off = (long)sizeof(h) + (long)offsetof(seq::Project, name);
        if (std::fseek(f, off, SEEK_SET) == 0) {
            char buf[24] = {0};
            if (std::fread(buf, 1, sizeof(buf), f) == sizeof(buf)) {
                buf[sizeof(buf) - 1] = 0;
                std::snprintf(out, n, "%s", buf);
                ok = true;
            }
        }
    }
    std::fclose(f);
    return ok;
}
// refresh both presence and names for the project menu
static void refresh_slots(ui::App& app) {
    for (int i = 0; i < 16; ++i) {
        app.slot_present[i] = slot_exists(i);
        app.slot_names[i][0] = 0;
        if (app.slot_present[i]) {
            if (!slot_peek_name(i, app.slot_names[i], sizeof(app.slot_names[i])))
                std::snprintf(app.slot_names[i], sizeof(app.slot_names[i]), "(old ver)");
        }
    }
}

// === audio: the real implementation lives in audio_ndsp.{h,cpp} (worker thread on core 1).
// there used to be an inline AudioSimple duplicate - it ate the UI thread's cpu, crackled on vsync. removed.

// === demo project setup ===
static void setup_demo() {
    auto& i0 = g_project.instruments[0];
    i0.type = seq::InstrumentType::Wavsynth;
    std::strcpy(i0.name, "saw lead");
    i0.wavsynth.shape   = synth::WaveShape::Saw;
    i0.wavsynth.attack  = 100;
    i0.wavsynth.decay   = 12000;
    i0.wavsynth.sustain = 0;
    i0.wavsynth.release = 2000;
    i0.table_id = 0;     // attach table 00 - filter sweep

    // demo table 00 - filter sweep + tremolo (16 rows, looping)
    auto& tbl0 = g_project.tables[0];
    // smooth cutoff sweep from 30..FF and back
    static const uint8_t sweep[16] = {
        0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0, 0xFF,
        0xF0, 0xD0, 0xB0, 0x90, 0x70, 0x50, 0x30, 0x20,
    };
    static const uint8_t trem[16]  = {
        0xFF, 0xE0, 0xC0, 0xA0, 0x90, 0xA0, 0xC0, 0xE0,
        0xFF, 0xE0, 0xC0, 0xA0, 0x90, 0xA0, 0xC0, 0xE0,
    };
    for (int i = 0; i < 16; ++i) {
        tbl0.rows[i].fx[0].cmd = seq::fx_cmd::FIL;
        tbl0.rows[i].fx[0].value = sweep[i];
        tbl0.rows[i].fx[1].cmd = seq::fx_cmd::VOL;
        tbl0.rows[i].fx[1].value = trem[i];
    }

    auto& i1 = g_project.instruments[1];
    i1.type = seq::InstrumentType::Sampler;
    std::strcpy(i1.name, "mic samp");
    i1.sampler.sample_slot = 0;
    i1.sampler.start = 0;
    i1.sampler.length = fx::Q15_ONE;
    i1.sampler.attack = 0;
    i1.sampler.release = 4000;
    i1.sampler.loop = false;

    // instrument 02 - DRUM KIT (one inst for the whole kit!)
    //   pad 0 (C-4) = kick     | pad 1 (C#) = snare
    //   pad 2 (D)   = hat-cls  | pad 3 (D#) = hat-opn
    //   pad 4 (E)   = clap     | pad 5 (F)  = tom
    //   pad 6 (F#)  = rim      | pad 7..15  = empty
    {
        auto& it = g_project.instruments[2];
        it.type = seq::InstrumentType::DrumKit;
        std::strcpy(it.name, "drum kit");
        it.drumkit = synth::DrumKitParams{};
        it.drumkit.base_note = 60;   // C-4
        for (int d = 0; d < 7; ++d) {
            it.drumkit.slots[d] = (uint8_t)(1 + d);   // slots 1..7 = kick..rim
        }
    }

    // === BREAKBEAT DEMO ===
    // phrase 0 - BASS line on saw lead (no mod table, clean sound)
    // disable table_id for instrument 0 - so the saw plays without fx wob
    g_project.instruments[0].table_id = seq::EMPTY;
    auto& p0 = g_project.phrases[0];
    // pulsing bass (low notes A2/D3/F2/G2)
    static const int bass_notes[seq::PHRASE_STEPS] = {
        45, 0xFF, 45, 0xFF,    50, 0xFF, 0xFF, 0xFF,
        41, 0xFF, 41, 0xFF,    43, 0xFF, 43, 0xFF,
    };
    for (int i = 0; i < seq::PHRASE_STEPS; ++i) {
        if (bass_notes[i] != 0xFF) {
            p0.steps[i].note = bass_notes[i];
            p0.steps[i].instrument = 0;
            p0.steps[i].velocity = 100;
        }
    }
    g_project.chains[0].rows[0].phrase = 0;

    // phrase 1 - KICK breakbeat (syncopated)
    // step:    0 1 2 3 4 5 6 7 8 9 A B C D E F
    // kick:    K . . . . . K . . . K . . K . .
    auto& p1 = g_project.phrases[1];
    for (int s : {0, 6, 10, 13}) {
        p1.steps[s].note = 60;          // C-4 = pad 0 = kick
        p1.steps[s].instrument = 2;     // drum kit
        p1.steps[s].velocity = 120;
    }

    // phrase 2 - SNARE backbeat + ghost notes
    auto& p2 = g_project.phrases[2];
    for (int s : {4, 12}) {
        p2.steps[s].note = 61;          // C#-4 = pad 1 = snare
        p2.steps[s].instrument = 2;
        p2.steps[s].velocity = 120;
    }
    for (int s : {2, 7, 15}) {
        p2.steps[s].note = 61;          // snare ghost
        p2.steps[s].instrument = 2;
        p2.steps[s].velocity = 50;
    }

    // phrase 3 - HAT 16th notes with open hat accents
    auto& p3 = g_project.phrases[3];
    for (int s = 0; s < seq::PHRASE_STEPS; ++s) {
        p3.steps[s].note = 62;          // D-4 = pad 2 = hat-cls
        p3.steps[s].instrument = 2;
        p3.steps[s].velocity = (s % 2 == 0) ? 80 : 50;
    }
    // open hat (pad 3 = D#-4) on 7 and 15
    p3.steps[7].note = 63;
    p3.steps[7].velocity = 100;
    p3.steps[15].note = 63;
    p3.steps[15].velocity = 100;

    // chains: 0=bass, 1=kick, 2=snare, 3=hat
    g_project.chains[1].rows[0].phrase = 1;
    g_project.chains[2].rows[0].phrase = 2;
    g_project.chains[3].rows[0].phrase = 3;

    // song row 0 - all 4 tracks together
    g_project.song.rows[0].chain[0] = 0;     // T0 = bass
    g_project.song.rows[0].chain[1] = 1;     // T1 = kick
    g_project.song.rows[0].chain[2] = 2;     // T2 = snare
    g_project.song.rows[0].chain[3] = 3;     // T3 = hat
    g_project.song.bpm = 110;                // breakbeat vibe
    g_project.song.groove = 6;
}

// === edge-trigger input with repeat ===
struct EdgeInput {
    u32 last_held = 0;
    u32 hold_frames[32] = {0};

    static constexpr int REPEAT_DELAY  = 18;
    static constexpr int REPEAT_PERIOD = 4;

    bool edge(u32 mask, u32 down, u32 held) {
        // edge on press
        if (down & mask) return true;
        // repeat for d-pad/abxy after a delay
        if (held & mask) {
            int bit = __builtin_ctz(mask);
            if (hold_frames[bit] >= REPEAT_DELAY &&
                ((hold_frames[bit] - REPEAT_DELAY) % REPEAT_PERIOD) == 0) {
                return true;
            }
        }
        return false;
    }

    void update(u32 held) {
        for (int b = 0; b < 32; ++b) {
            if (held & (1u << b)) hold_frames[b]++;
            else                  hold_frames[b] = 0;
        }
        last_held = held;
    }
};

// === in-app screenshot (R+SELECT) ===
// rosalina's screenshot deadlocks apps running audio threads on core1
// (luma3ds issues #1483/#1846) - so we bake our own, bypassing rosalina entirely.
// gfxInitDefault = linear-heap BGR8 framebuffers, freely readable by the CPU.
//
// framebuffer layout is rotated 90deg: index = (x*240 + (239-y)) * 3, bytes B,G,R.
// a 24bpp bottom-up BMP wants row r (from the bottom) = screen y (239-r), so
// src = fb[(x*240 + r)*3] and the BGR byte order matches 1:1 - plain copy.
static void write_bmp_240(const char* path, const u8* fb, int w) {
    const int h = 240;
    const int row_bytes = w * 3;            // 1200/960 - multiple of 4, no padding
    const int data_bytes = row_bytes * h;
    u8 hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    u32 fsz = 54 + (u32)data_bytes;
    hdr[2] = (u8)fsz; hdr[3] = (u8)(fsz >> 8); hdr[4] = (u8)(fsz >> 16); hdr[5] = (u8)(fsz >> 24);
    hdr[10] = 54;                            // pixel data offset
    hdr[14] = 40;                            // BITMAPINFOHEADER
    hdr[18] = (u8)w;  hdr[19] = (u8)(w >> 8);
    hdr[22] = (u8)h;  hdr[23] = (u8)(h >> 8);
    hdr[26] = 1;                             // planes
    hdr[28] = 24;                            // bpp
    hdr[34] = (u8)data_bytes; hdr[35] = (u8)(data_bytes >> 8);
    hdr[36] = (u8)(data_bytes >> 16); hdr[37] = (u8)(data_bytes >> 24);

    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fwrite(hdr, 1, 54, f);
    static u8 line[400 * 3];                 // max screen width
    for (int r = 0; r < h; ++r) {
        u8* d = line;
        for (int x = 0; x < w; ++x) {
            const u8* s = fb + (x * 240 + r) * 3;
            *d++ = s[0]; *d++ = s[1]; *d++ = s[2];
        }
        std::fwrite(line, 1, row_bytes, f);
    }
    std::fclose(f);
}

// dump both screens to sdmc:/3ds/descry/screens/scrNNN_{top,bot}.bmp.
// reads the CURRENT back buffer = the last presented frame (the ui redraws the
// same content every frame, so it's visually identical to what's on screen).
// returns the shot index, or -1 on failure.
static int save_screenshot() {
    mkdir("sdmc:/3ds/descry/screens", 0777);
    static int next_idx = 0;                 // persists per session - scan resumes where we stopped
    char pt[80], pb[80];
    struct stat st;
    for (; next_idx < 1000; ++next_idx) {
        std::snprintf(pt, sizeof(pt), "sdmc:/3ds/descry/screens/scr%03d_top.bmp", next_idx);
        if (stat(pt, &st) != 0) break;
    }
    if (next_idx >= 1000) return -1;
    std::snprintf(pb, sizeof(pb), "sdmc:/3ds/descry/screens/scr%03d_bot.bmp", next_idx);

    u16 fw, fh;
    u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fw, &fh);
    if (fb) write_bmp_240(pt, fb, 400);
    fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fw, &fh);
    if (fb) write_bmp_240(pb, fb, 320);
    return next_idx++;
}

int main() {
    gfxInitDefault();
    irrstInit();   // for the C-stick (right stick on new3ds) - without this irrstCstickRead returns garbage
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS * 4);  // more objects for the bitmap font
    C2D_Prepare();

    C3D_RenderTarget* top    = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    // === new3ds enhanced mode: 804 MHz CPU + L2 cache ===
    // call AFTER all graphics init (otherwise PICA200 reads the clock in the old mode).
    // on new3ds: much faster audio/synthesis.
    // on old3ds: no-op (runs at 268MHz as before, no problems).
    osSetSpeedupEnable(true);

    // ptmu - battery level / charging status (for the header indicator)
    ptmuInit();

    setup_demo();

    seq::Player player(g_project, g_mixer);
    ui::App app(g_project, player, g_mixer);

    platform::Audio3DS audio;
    if (!audio.init(g_mixer, player)) {
        // ndspInit failed. 99% of the time this is a missing DSP firmware dump,
        // NOT a broken build:
        //  - emulators (azahar/citra): the virtual SD has no dspfirm.cdc ->
        //    the DSP service can't start -> apps using ndsp bounce instantly
        //  - real console: same story if the user never ran a DSP dumper
        // show a readable screen instead of silently returning to the menu
        // (github issue #2: "instantly exits/crashes back to the main UI").
        platform::Draw3DS edraw;
        for (int f = 0; f < 60 * 60 && aptMainLoop(); ++f) {   // up to ~60s
            hidScanInput();
            if (hidKeysDown()) break;
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            C2D_TargetClear(top, C2D_Color32(10, 10, 14, 255));
            C2D_SceneBegin(top);
            int y = 36;
            edraw.text(24, y, "AUDIO INIT FAILED (NDSP)", ui::pal::RECORD, 2); y += 28;
            edraw.text(24, y, "descry could not start the 3DS DSP audio service.", ui::pal::FG); y += 12;
            edraw.text(24, y, "this is almost always a missing DSP firmware dump.", ui::pal::FG); y += 20;
            edraw.text(24, y, "EMULATOR (AZAHAR / CITRA):", ui::pal::HEADER); y += 12;
            edraw.text(24, y, " dump dspfirm.cdc from a real 3DS and place it at", ui::pal::FG); y += 12;
            edraw.text(24, y, " sdmc:/3ds/dspfirm.cdc on the emulated SD card", ui::pal::FG_HEX); y += 20;
            edraw.text(24, y, "REAL CONSOLE:", ui::pal::HEADER); y += 12;
            edraw.text(24, y, " run the DSP1 homebrew once to dump the firmware", ui::pal::FG); y += 24;
            edraw.text(24, y, "PRESS ANY BUTTON TO EXIT", ui::pal::FG_DIM);
            C2D_TargetClear(bottom, C2D_Color32(10, 10, 14, 255));
            C2D_SceneBegin(bottom);
            C3D_FrameEnd(0);
        }
        C2D_Fini();
        C3D_Fini();
        ptmuExit();
        gfxExit();
        return 1;
    }

    platform::Mic3DS mic;
    bool mic_ok = mic.platform_init();
    synth::SampleRecorder rec(mic);

    // === deferred boot loading ===
    // all the SD i/o (session, 32 samples, wavetables, slot scan) used to run
    // BEFORE the first frame - a visible freeze on a full card. now it's a state
    // machine driven one step per splash frame: the sweep animates while the SD
    // grinds underneath. if the user skips the splash, the rest is drained sync.
    int boot_step = 0;
    const int BOOT_STEPS = 1 + synth::SAMPLE_BANK_SIZE + 2;  // session + samples + wt + slots
    auto boot_load_step = [&]() -> bool {
        if (boot_step == 0) {
            seq::load_project(g_project, SESSION_PATH);   // demo stays if absent
            app.sync_mixer_from_song();   // apply the session's mixer settings
        } else if (boot_step <= (int)synth::SAMPLE_BANK_SIZE) {
            load_sample_from_sd(boot_step - 1);
        } else if (boot_step == (int)synth::SAMPLE_BANK_SIZE + 1) {
            mkdir("sdmc:/3ds/descry/wavetable", 0777);
            synth::WavetableBank::instance().scan_dir("sdmc:/3ds/descry/wavetable");
        } else if (boot_step == (int)synth::SAMPLE_BANK_SIZE + 2) {
            refresh_slots(app);
        } else {
            return false;   // done
        }
        ++boot_step;
        return boot_step <= BOOT_STEPS;
    };

    // restore saved settings (before first draw). settings.cfg is the new home;
    // fall back to the legacy theme.cfg so a 1.0.3 user's theme survives.
    {
        ui::App::Settings s;
        if (load_settings(s)) app.apply_settings(s);
        else                  app.set_theme(load_theme_idx());
    }
    ui::App::Settings last_settings = app.get_settings();

    platform::Draw3DS draw;
    EdgeInput edge;

    // === boot splash: wordmark in THEME colors, playhead-sweep reveal ===
    // runs after set_theme(load_theme_idx()) so pal:: already holds the user's
    // theme. the logo is drawn in the theme's motion accent (PLAY) with the same
    // bevel rule as the brand kit; a bright sweep line reveals it left-to-right
    // like a playhead crossing the screen. any button skips.
    // doubles as the loading screen: boot_load_step() runs during the HOLD phase
    // (after the sweep lands) - the sweep itself stays butter-smooth, no SD i/o.
    {
        constexpr int CELL = 2;
        const int ox = (400 - ui::LOGO_W * CELL) / 2;
        const int oy = (240 - ui::LOGO_H * CELL) / 2;

        // theme-derived splash palette
        const ui::Color spl_bg   = ui::pal::PANEL;
        const ui::Color spl_base = ui::pal::PLAY;
        const ui::Color spl_hi   = ui::lerp_color(ui::pal::PLAY, ui::pal::FG, 120);
        const ui::Color spl_sh   = ui::lerp_color(ui::pal::PLAY, ui::pal::PANEL, 120);
        const u8 br = (spl_bg >> 16) & 0xFF, bg_ = (spl_bg >> 8) & 0xFF, bb = spl_bg & 0xFF;

        constexpr int SWEEP_FRAMES = 48;   // reveal phase
        constexpr int TOTAL_FRAMES = 84;   // ~1.4s incl. hold

        for (int frame = 0; frame < TOTAL_FRAMES && aptMainLoop(); ++frame) {
            hidScanInput();
            if (hidKeysDown()) break;   // impatient? straight to work (drain below)

            // SD i/o only after the sweep has landed - one bite per hold frame
            if (frame >= SWEEP_FRAMES) boot_load_step();

            // sweep position in logo columns (runs a bit past the edge so the
            // trailing glow fully settles)
            const int sweep = frame * (ui::LOGO_W + 8) / SWEEP_FRAMES;
            const bool done = frame >= SWEEP_FRAMES;

            // hold phase: gentle breathe on the highlight
            const uint8_t pulse = done ? ui::breathe_pulse((uint32_t)(frame - SWEEP_FRAMES), 80) : 0;

            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            C2D_TargetClear(top, C2D_Color32(br, bg_, bb, 255));
            C2D_SceneBegin(top);
            for (int gy = 0; gy < ui::LOGO_H; ++gy) {
                for (int gx = 0; gx < ui::LOGO_W; ++gx) {
                    if (!ui::logo_cell(gx, gy)) continue;
                    if (!done && gx >= sweep) continue;   // not revealed yet
                    // same bevel rule as the brand kit renderer
                    bool lit    = !ui::logo_cell(gx, gy - 1) || !ui::logo_cell(gx - 1, gy);
                    bool shaded = !ui::logo_cell(gx, gy + 1) || !ui::logo_cell(gx + 1, gy);
                    ui::Color c = (lit && !shaded) ? spl_hi
                                : (shaded && !lit) ? spl_sh
                                :                    spl_base;
                    // trailing glow: cells just behind the sweep flash toward FG
                    if (!done) {
                        int dist = sweep - gx;
                        if (dist >= 0 && dist < 6)
                            c = ui::lerp_color(c, ui::pal::FG, (uint8_t)((5 - dist) * 42));
                    } else if (pulse && lit && !shaded) {
                        // hold phase: highlight edge breathes softly
                        c = ui::lerp_color(spl_hi, ui::pal::FG, (uint8_t)(pulse / 3));
                    }
                    draw.rect(ox + gx * CELL, oy + gy * CELL, CELL, CELL, c);
                }
            }
            // the sweep line itself: a thin playhead crossing the logo band
            if (!done) {
                int sx = ox + sweep * CELL;
                if (sx < 398)
                    draw.rect(sx, oy - 10, 2, ui::LOGO_H * CELL + 20,
                              ui::with_alpha(ui::pal::FG, 190));
            }
            C2D_TargetClear(bottom, C2D_Color32(br, bg_, bb, 255));
            C2D_SceneBegin(bottom);
            // tagline fades in once the sweep lands
            if (done) {
                u32 ta = (u32)((frame - SWEEP_FRAMES) * 255 / 24);
                if (ta > 255) ta = 255;
                draw.text(226, 228, "open tracker", ui::with_alpha(ui::pal::FG_DIM, (uint8_t)ta), 1);
            }
            C3D_FrameEnd(0);
        }
    }

    // drain whatever boot i/o is left (splash skipped or SD slower than the sweep)
    while (boot_load_step()) {}

    bool prev_y = false;
    bool prev_touch = false;
    int  rec_target = -1;       // keep the slot between begin/stop

    // smart L/R: track whether a dpad press happened during the hold (then it's a modifier, not a tap)
    bool prev_l_held = false;
    bool prev_r_held = false;
    bool l_used_modifier = false;
    bool r_used_modifier = false;

    // screenshot toast ("SAVED scr012") - frames left + last index
    int shot_toast = 0;
    int shot_idx   = -1;

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        u32 held = hidKeysHeld();

        if (down & KEY_SELECT && (held & KEY_START)) break;  // select+start = exit

        // held_L + SELECT = toggle fullscreen scope (performance visualizer)
        if ((down & KEY_SELECT) && (held & KEY_L)) {
            app.toggle_scope_full();
        }

        // held_R + SELECT = in-app screenshot (both screens -> BMP on SD).
        // captured BEFORE this frame is drawn -> the toast never leaks into the shot
        // (unless you snap twice within a second). SD write ~100ms = one skipped
        // frame; audio doesn't care, it lives on the worker thread.
        if ((down & KEY_SELECT) && (held & KEY_R)) {
            shot_idx   = save_screenshot();
            shot_toast = 45;                 // ~0.75s
            r_used_modifier = true;          // this R hold was a combo, not a screen-switch tap
        }

        // mic recording: ZR - record into the slot the app specifies
        bool y_now = (held & KEY_ZR) != 0;
        if (mic_ok) {
            if (y_now && !prev_y) {
                rec_target = app.rec_target_slot();
                rec.begin_recording(rec_target);
            } else if (!y_now && prev_y) {
                rec.stop_recording();
                if (rec_target >= 0) app.on_rec_done(rec_target);
                rec_target = -1;
            }
            rec.tick();
        }
        prev_y = y_now;

        // input → InputState
        ui::InputState in;
        in.up    = edge.edge(KEY_DUP,    down, held);
        in.down  = edge.edge(KEY_DDOWN,  down, held);
        in.left  = edge.edge(KEY_DLEFT,  down, held);
        in.right = edge.edge(KEY_DRIGHT, down, held);
        in.a     = edge.edge(KEY_A,      down, held);
        in.b     = edge.edge(KEY_B,      down, held);
        in.x     = edge.edge(KEY_X,      down, held);
        in.y     = edge.edge(KEY_Y,      down, held);
        in.start = (down & KEY_START) != 0;
        in.select_ = (down & KEY_SELECT) != 0;
        // if SELECT came with held_L - that was a scope toggle, don't pass it as preview
        if ((down & KEY_SELECT) && (held & KEY_L)) in.select_ = false;
        // same for held_R - screenshot combo, not a preview
        if ((down & KEY_SELECT) && (held & KEY_R)) in.select_ = false;
        in.held_l = (held & KEY_L) != 0;
        in.held_r = (held & KEY_R) != 0;
        in.held_zl = (held & KEY_ZL) != 0;   // ZL modifier for copy/paste
        in.held_select = (held & KEY_SELECT) != 0;   // gate for hold-to-sustain preview

        // === SMART L/R TAP ===
        // screens switch ON RELEASE, only if there was no dpad modifier during the hold
        bool l_held_now = (held & KEY_L) != 0;
        bool r_held_now = (held & KEY_R) != 0;
        // if a dpad OR an action button (A/B/X/Y) was pressed while L/R is held - it's a modifier (combo), not a tap.
        // without A/B/X/Y a bug: releasing R+A (delete) -> R counted as a tap -> the screen jumped.
        constexpr u32 COMBO_KEYS = KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT |
                                   KEY_A | KEY_B | KEY_X | KEY_Y;
        if (l_held_now && (down & (COMBO_KEYS | KEY_SELECT))) l_used_modifier = true;
        if (r_held_now && (down & COMBO_KEYS)) r_used_modifier = true;
        // release detection
        bool l_released = prev_l_held && !l_held_now;
        bool r_released = prev_r_held && !r_held_now;
        // a tap counts only if a modifier didn't trigger
        // ZL is now purely a copy/paste modifier (ZL+X/Y) - no screen switching
        // (tap L already does prev-screen, so ZL-back was redundant).
        in.l = (l_released && !l_used_modifier);
        in.r = (r_released && !r_used_modifier);
        // reset the modifier flag after release
        if (!l_held_now) l_used_modifier = false;
        if (!r_held_now) r_used_modifier = false;
        prev_l_held = l_held_now;
        prev_r_held = r_held_now;

        // === analog sticks: left circle pad + right C-stick ===
        // raw values -156..+156. deadzone cuts off center drift.
        // normalize to -1000..+1000 for easier mapping.
        {
            constexpr int DEADZONE = 20;     // rest threshold (sticks drift near center)
            constexpr int MAXRAW  = 156;
            auto norm = [](int raw, int& out) -> bool {
                if (raw > -DEADZONE && raw < DEADZONE) { out = 0; return false; }
                // remove deadzone and scale the remainder to 0..1000
                int sign = raw < 0 ? -1 : 1;
                int mag  = (raw < 0 ? -raw : raw) - DEADZONE;
                int span = MAXRAW - DEADZONE;
                int v = mag * 1000 / (span > 0 ? span : 1);
                if (v > 1000) v = 1000;
                out = sign * v;
                return true;
            };
            circlePosition cp, cs;
            hidCircleRead(&cp);          // left circle pad
            irrstCstickRead(&cs);        // right C-stick (new3ds)
            bool lax = norm(cp.dx, in.lstick_x);
            bool lay = norm(cp.dy, in.lstick_y);
            bool cax = norm(cs.dx, in.cstick_x);
            bool cay = norm(cs.dy, in.cstick_y);
            in.lstick_active = lax || lay;
            in.cstick_active = cax || cay;
        }

        edge.update(held);

        app.update(in);
        // recording tint: mic capture OR master resample = the UI runs hot
        app.set_recording(rec.is_recording() || g_mixer.is_resampling());
        app.tick();   // animation layer: advance the frame counter (trail/flash/playhead pulses)

        // touch on the bottom screen - edge (down) + continuous move + release
        bool touch_now = (held & KEY_TOUCH) != 0;
        if (down & KEY_TOUCH) {
            touchPosition tp;
            hidTouchRead(&tp);
            app.touch(tp.px, tp.py);
        } else if (touch_now && prev_touch) {
            // finger is held - forward the current position to touch_move (for drag)
            touchPosition tp;
            hidTouchRead(&tp);
            app.touch_move(tp.px, tp.py);
        }
        if (!touch_now && prev_touch) {
            app.touch_release();
        }
        prev_touch = touch_now;

        // handle save/load by flags from the ui
        if (app.consume_save_request()) { save_full_project(); app.dirty = false; }
        if (app.consume_load_request()) { load_full_project(); app.dirty = false; }
        if (app.consume_reset_request()) {
            // stop playback, zero the project, restart the demo
            if (player.playing()) player.stop();
            std::memset(&g_project, 0, sizeof(g_project));
            // reconstruct default initialization via placement-new
            new (&g_project) seq::Project();
            setup_demo();
            app.dirty = false;   // fresh demo is clean, no autosave
        }
        if (app.consume_render_request()) {
            // pause realtime audio briefly, render, resume
            // (realtime audio uses g_mixer and the live player, render has its own xmix/xplayer - no conflict)
            bool ok = render_song_to_wav();
            std::snprintf(app.slot_status, sizeof(app.slot_status),
                          ok ? "rendered -> 3ds/descry/render.wav"
                             : "RENDER FAILED (empty song?)");
        }

        // === project menu actions ===
        {
            int slot;
            ui::App::ProjAction act;
            if (app.consume_proj_action(slot, act)) {
                switch (act) {
                    case ui::App::ProjAction::Load:
                        if (slot_load(slot)) {
                            std::snprintf(app.slot_status, sizeof(app.slot_status),
                                          "loaded slot %02X", slot);
                            app.dirty = false;
                        } else {
                            // empty slot - reset live to a blank project
                            // (sample bank stays - it's a singleton, not part of Project)
                            std::memset(&g_project, 0, sizeof(g_project));
                            new (&g_project) seq::Project();
                            std::snprintf(app.slot_status, sizeof(app.slot_status),
                                          "BLANKED (slot %02X was empty)", slot);
                            app.dirty = false;
                        }
                        // either way the Song's mixer block changed - push it into
                        // the engine now, not on the next mixer-screen visit
                        app.sync_mixer_from_song();
                        break;
                    case ui::App::ProjAction::Save:
                        slot_save(slot);
                        std::snprintf(app.slot_status, sizeof(app.slot_status),
                                      "saved to slot %02X", slot);
                        break;
                    case ui::App::ProjAction::New:
                        // create a fresh demo in this slot without affecting live
                        // important: don't copy g_project onto the stack (62kb)! use the heap
                        {
                            auto* save = new seq::Project(g_project);
                            std::memset(&g_project, 0, sizeof(g_project));
                            new (&g_project) seq::Project();
                            setup_demo();
                            slot_save(slot);
                            g_project = *save;
                            delete save;
                        }
                        std::snprintf(app.slot_status, sizeof(app.slot_status),
                                      "new demo in slot %02X", slot);
                        break;
                    case ui::App::ProjAction::Delete:
                        slot_delete(slot);
                        std::snprintf(app.slot_status, sizeof(app.slot_status),
                                      "deleted slot %02X", slot);
                        break;
                    default: break;
                }
                // refresh slot scan
                refresh_slots(app);
            }
        }

        audio.tick();

        // mirror the audio underrun counter for the scope debug footer
        app.debug_xruns = audio.underruns();

        // settings changed (theme / octave / kb mode / kaoss assigns)? persist.
        // a few bytes memcmp per frame, a tiny file write only on change.
        {
            ui::App::Settings now = app.get_settings();
            if (std::memcmp(&now, &last_settings, sizeof(now)) != 0) {
                last_settings = now;
                save_settings(now);
            }
        }

        // === update system status (battery + clock) - once every ~30 frames (0.5s) ===
        {
            static int status_ctr = 0;
            if (--status_ctr <= 0) {
                status_ctr = 30;
                u8 batt = 5; bool charging = false;
                PTMU_GetBatteryLevel(&batt);
                u8 chg = 0; PTMU_GetBatteryChargeState(&chg);
                charging = (chg != 0);
                time_t t = time(nullptr);
                struct tm* lt = localtime(&t);
                int hh = lt ? lt->tm_hour : -1;
                int mm = lt ? lt->tm_min  : 0;
                app.set_system_status((int)batt, charging, hh, mm);
            }
        }

        // === render ===
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

        C2D_TargetClear(top, C2D_Color32(16, 16, 24, 255));
        C2D_SceneBegin(top);
        app.draw_top(draw);

        // === big waveform overlay during recording (over the top screen) ===
        if (rec.is_recording() && rec_target >= 0 &&
            rec_target < (int)synth::SAMPLE_BANK_SIZE) {
            auto& s = synth::SampleBank::instance().slot(rec_target);
            // background panel covering the whole screen
            draw.rect(0, 50, 400, 130, 0xFF200810);
            // frame
            draw.rect(0, 50, 400, 2, 0xFFFF4040);
            draw.rect(0, 178, 400, 2, 0xFFFF4040);

            // title
            char hdr[48];
            std::snprintf(hdr, sizeof(hdr), "● REC → SLOT %02d   %.2fs   %u frames",
                          rec_target, s.data.size() / 32000.0f, (unsigned)s.data.size());
            draw.text(12, 56, hdr, 0xFFFFD0D0, 1);

            // waveform area
            constexpr int WAVE_X = 8;
            constexpr int WAVE_Y = 78;
            constexpr int WAVE_W = 384;
            constexpr int WAVE_H = 90;
            draw.rect(WAVE_X, WAVE_Y, WAVE_W, WAVE_H, 0xFF080812);
            draw.rect(WAVE_X, WAVE_Y + WAVE_H / 2, WAVE_W, 1, 0xFF402030);

            if (!s.data.empty()) {
                std::size_t total = s.data.size();
                for (int x = 0; x < WAVE_W; ++x) {
                    std::size_t a = (std::size_t)x * total / WAVE_W;
                    std::size_t b = (std::size_t)(x + 1) * total / WAVE_W;
                    if (b <= a) b = a + 1;
                    if (b > total) b = total;
                    int32_t mn = 32767, mx = -32768;
                    // full scan for an exact min/max
                    for (std::size_t i = a; i < b; ++i) {
                        int32_t v = s.data[i];
                        if (v < mn) mn = v;
                        if (v > mx) mx = v;
                    }
                    int yt = WAVE_Y + WAVE_H / 2 - (mx * (WAVE_H / 2)) / 32768;
                    int yb = WAVE_Y + WAVE_H / 2 - (mn * (WAVE_H / 2)) / 32768;
                    if (yt > yb) { int t = yt; yt = yb; yb = t; }
                    if (yt < WAVE_Y) yt = WAVE_Y;
                    if (yb >= WAVE_Y + WAVE_H) yb = WAVE_Y + WAVE_H - 1;
                    draw.rect(WAVE_X + x, yt, 1, yb - yt + 1, 0xFF80FFC0);   // green as everywhere
                }
            }
            // mic-tip
            draw.text(12, 184, "● RECORDING — RELEASE ZR TO STOP", 0xFFFF8080, 1);
        }

        // flush top scene before switching to bottom
        C2D_TargetClear(bottom, C2D_Color32(16, 16, 24, 255));
        C2D_SceneBegin(bottom);
        app.draw_bottom(draw);

        // recording overlay on the bottom screen - more modest, the main one is on top
        if (rec.is_recording()) {
            float p = rec.progress();
            draw.rect(20, 56, 280, 14, 0xFF200810);
            draw.rect(20, 56, (int)(280 * p), 14, 0xFFFF3030);
        }

        // screenshot toast - bottom edge of the touch screen, out of the way
        if (shot_toast > 0) {
            shot_toast--;
            char msg[48];
            if (shot_idx >= 0)
                std::snprintf(msg, sizeof(msg), "SAVED screens/scr%03d", shot_idx);
            else
                std::snprintf(msg, sizeof(msg), "SCREENSHOT FAILED");
            draw.rect(0, 228, 320, 12, 0xFF102018);
            draw.text(6, 230, msg, 0xFF80FFC0, 1);
        }

        C3D_FrameEnd(0);
    }

    if (player.playing()) player.stop();
    rec.stop_recording();
    if (mic_ok) mic.platform_shutdown();
    audio.shutdown();
    // autosave to session.tr3d - ONLY if the user changed something
    // (otherwise a fresh setup_demo would endlessly get stuck on old data)
    if (app.dirty) {
        save_full_project();
    }
    C2D_Fini();
    C3D_Fini();
    ptmuExit();
    gfxExit();
    return 0;
}
