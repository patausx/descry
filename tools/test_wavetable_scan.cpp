// Host regression for WavetableBank::scan_dir slot assignment.
//
// Guards two bugs that only ever showed up on a real SD card:
//   1. filenames >= 20 chars were silently dropped, which killed nearly every
//      real single-cycle pack (AKWF_hvoice_0001.wav is exactly 20).
//   2. only the first 32 readdir entries were sorted, so with more than
//      FILE_SLOTS files the surviving set depended on FAT directory order and
//      could reshuffle when any file was added or removed - silently
//      repointing user_slot references in already-saved projects.
#include "core/synth/wavetable.h"
#include "core/synth/sampler.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace trackr;

namespace {

const char* kDir = "/tmp/descry-wt-scan";

void write_cycle_wav(const std::string& path, int frames = 600) {
    std::vector<int16_t> pcm(frames);
    for (int i = 0; i < frames; ++i)
        pcm[i] = (int16_t)(20000.0 * std::sin(i * 2.0 * 3.14159265358979 / frames));
    const uint32_t data_bytes = (uint32_t)(pcm.size() * sizeof(int16_t));
    FILE* f = std::fopen(path.c_str(), "wb");
    assert(f && "cannot create test wav");
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + data_bytes);
    std::fwrite("WAVEfmt ", 1, 8, f); u32(16);
    u16(1); u16(1); u32(32000); u32(64000); u16(2); u16(16);
    std::fwrite("data", 1, 4, f); u32(data_bytes);
    std::fwrite(pcm.data(), 1, data_bytes, f);
    std::fclose(f);
}

void reset_dir() {
    std::system("rm -rf /tmp/descry-wt-scan && mkdir -p /tmp/descry-wt-scan");
}

int scan() {
    return synth::WavetableBank::instance().scan_dir(kDir);
}

const char* slot_name(int i) {
    return synth::WavetableBank::instance().name(i);
}

} // namespace

int main() {
    constexpr int FS = synth::WavetableBank::FILE_SLOTS;

    // --- 1. long filenames must load, not be skipped -----------------------
    reset_dir();
    const char* long_names[] = {
        "AKWF_0001.wav",           // 13
        "AKWF_hvoice_0001.wav",    // 20 - used to be dropped
        "AKWF_altosax_0001.wav",   // 21 - used to be dropped
        "pad_warm_analog.wav",     // 19
    };
    for (const char* n : long_names) write_cycle_wav(std::string(kDir) + "/" + n);
    assert(scan() == 4 && "every valid wav must occupy a slot regardless of name length");
    // alphabetical, and the display name is truncated but never empty
    assert(std::strcmp(slot_name(0), "AKWF_0001") == 0);
    assert(std::strcmp(slot_name(1), "AKWF_altosax_0001") == 0);
    assert(std::strcmp(slot_name(2), "AKWF_hvoice_0001") == 0);
    assert(std::strcmp(slot_name(3), "pad_warm_analog") == 0);

    // --- 2. name length boundary: <64 chars in, >=64 out -------------------
    reset_dir();
    write_cycle_wav(std::string(kDir) + "/" + std::string(59, 'a') + ".wav");  // 63, ok
    write_cycle_wav(std::string(kDir) + "/" + std::string(60, 'b') + ".wav");  // 64, skipped
    assert(scan() == 1 && "63-char name loads, 64-char name is rejected");

    // --- 3. slots are alphabetical over the WHOLE directory ----------------
    // Far more files than slots, created in shuffled order so that raw readdir
    // order cannot accidentally match the expected result.
    reset_dir();
    std::vector<std::string> names;
    for (int i = 0; i < FS * 3; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%c%d_tone.wav", (char)('a' + i / 10), i % 10);
        names.emplace_back(buf);
    }
    std::vector<std::string> shuffled = names;
    for (std::size_t i = shuffled.size(); i > 1; --i)   // deterministic shuffle
        std::swap(shuffled[i - 1], shuffled[(i * 7919) % i]);
    for (const auto& n : shuffled) write_cycle_wav(std::string(kDir) + "/" + n);

    assert(scan() == FS && "a full directory must fill every file slot");
    std::vector<std::string> sorted = names;
    for (std::size_t i = 1; i < sorted.size(); ++i)     // insertion sort, no <algorithm>
        for (std::size_t j = i; j > 0 && sorted[j] < sorted[j - 1]; --j)
            std::swap(sorted[j], sorted[j - 1]);
    for (int i = 0; i < FS; ++i) {
        std::string want = sorted[i].substr(0, sorted[i].size() - 4);
        assert(want == slot_name(i) && "slot order must be the alphabetical prefix, no gaps");
    }

    // --- 4. adding a LATE-sorting file must not disturb existing slots -----
    // (the old 32-entry readdir window could reshuffle everything here)
    std::vector<std::string> before;
    for (int i = 0; i < FS; ++i) before.emplace_back(slot_name(i));
    write_cycle_wav(std::string(kDir) + "/zzz_last.wav");
    assert(scan() == FS);
    for (int i = 0; i < FS; ++i)
        assert(before[i] == slot_name(i) && "late file must not move earlier slots");

    // --- 5. junk and unreadable files are ignored, not counted -------------
    reset_dir();
    write_cycle_wav(std::string(kDir) + "/good.wav");
    { FILE* f = std::fopen("/tmp/descry-wt-scan/notes.txt", "wb");
      std::fwrite("junk", 1, 4, f); std::fclose(f); }
    { FILE* f = std::fopen("/tmp/descry-wt-scan/broken.wav", "wb");
      std::fwrite("NOTAWAVE", 1, 8, f); std::fclose(f); }
    assert(scan() == 1 && "non-wav and malformed files must not occupy slots");
    assert(std::strcmp(slot_name(0), "good") == 0);

    // --- 6. empty and missing directories are safe ------------------------
    reset_dir();
    assert(scan() == 0);
    assert(synth::WavetableBank::instance().scan_dir("/tmp/descry-no-such-dir") == 0);
    assert(std::strcmp(slot_name(0), "--") == 0 && "empty slot renders as --");

    std::system("rm -rf /tmp/descry-wt-scan");
    std::puts("wavetable scan_dir regression: ok");
}
