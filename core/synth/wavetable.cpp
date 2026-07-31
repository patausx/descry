// user wavetable bank implementation. see wavetable.h.
#include "wavetable.h"
#include "wav_loader.h"
#include "sampler.h"
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <dirent.h>

namespace trackr::synth {

namespace {
constexpr uint32_t CAP_MAGIC = 0x42545744u; // 'DWTB'
constexpr uint16_t CAP_VERSION = 1;
struct CaptureHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t slots;
    uint16_t size;
    uint16_t reserved;
};

static void capture_path(char* out, std::size_t cap, const char* dir, bool temp) {
    std::snprintf(out, cap, "%s/descry_captures.wtb%s", dir, temp ? ".tmp" : "");
}

// Resample one logical frame window into a seamless SIZE-point cycle. Stereo is
// folded to mono; endpoint interpolation wraps to the window start.
static bool cycle_resample_window(const Sample& src, uint32_t begin, uint32_t end,
                                  fx::q15* dst, int dst_len) {
    const uint32_t total = src.num_frames();
    if (!dst || total < 2 || begin >= total) return false;
    if (end > total) end = total;
    if (end <= begin + 1) return false;
    const uint32_t n = end - begin;
    const int ch = src.channels == 2 ? 2 : 1;
    auto mono = [&](uint32_t frame) -> int32_t {
        if (ch == 1) return src.data[frame];
        return ((int32_t)src.data[(std::size_t)frame * 2] +
                (int32_t)src.data[(std::size_t)frame * 2 + 1]) / 2;
    };
    for (int i = 0; i < dst_len; ++i) {
        const uint64_t pos = ((uint64_t)i << 16) * n / (uint32_t)dst_len;
        const uint32_t local0 = (uint32_t)(pos >> 16);
        const uint32_t frac = (uint32_t)(pos & 0xFFFF);
        const uint32_t local1 = (local0 + 1) % n;
        const int32_t a = mono(begin + local0);
        const int32_t b = mono(begin + local1);
        dst[i] = (fx::q15)(a + (int32_t)(((int64_t)(b - a) * frac) >> 16));
    }

    // DC removal, then normalize to 95% full scale. This makes quiet recordings
    // useful as oscillators without driving the wavsynth mixer into hard limits.
    int64_t sum = 0;
    for (int i = 0; i < dst_len; ++i) sum += dst[i];
    const int32_t mean = (int32_t)(sum / dst_len);
    int32_t peak = 0;
    for (int i = 0; i < dst_len; ++i) {
        int32_t v = (int32_t)dst[i] - mean;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        dst[i] = (fx::q15)v;
        const int32_t av = v < 0 ? -v : v;
        if (av > peak) peak = av;
    }
    if (peak < 16) return false;
    constexpr int32_t TARGET = 31128; // 95% q15
    for (int i = 0; i < dst_len; ++i) {
        int32_t v = (int32_t)(((int64_t)dst[i] * TARGET) / peak);
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        dst[i] = (fx::q15)v;
    }
    return true;
}
} // namespace

WavetableBank& WavetableBank::instance() {
    static WavetableBank bank;
    return bank;
}

void WavetableBank::rebuild_active_count() {
    active_count_ = 0;
    for (bool on : occupied_) if (on) ++active_count_;
}

int WavetableBank::slot_at(int active_index) const {
    if (active_index < 0) return -1;
    for (int slot = 0; slot < SLOTS; ++slot) {
        if (!occupied_[slot]) continue;
        if (active_index-- == 0) return slot;
    }
    return -1;
}

int WavetableBank::index_of_slot(int wanted) const {
    int index = 0;
    for (int slot = 0; slot < SLOTS; ++slot) {
        if (!occupied_[slot]) continue;
        if (slot == wanted) return index;
        ++index;
    }
    return -1;
}

bool WavetableBank::prepare_capture(const Sample& src, uint32_t start_frame,
                                    uint32_t end_frame, fx::q15* out) {
    return cycle_resample_window(src, start_frame, end_frame, out, SIZE);
}

int WavetableBank::install_capture(const fx::q15* cycle, const char* label) {
    if (!cycle) return -1;
    int slot = -1;
    for (int i = FILE_SLOTS; i < SLOTS; ++i) {
        if (!occupied_[i]) { slot = i; break; }
    }
    if (slot < 0) return -1;
    std::memcpy(data_[slot], cycle, sizeof(data_[slot]));
    std::snprintf(names_[slot], sizeof(names_[slot]), "%s", label && label[0] ? label : "capture");
    occupied_[slot] = true;
    rebuild_active_count();
    return slot;
}

bool WavetableBank::remove_capture(int slot) {
    if (slot < FILE_SLOTS || slot >= SLOTS || !occupied_[slot]) return false;
    occupied_[slot] = false;
    names_[slot][0] = 0;
    std::memset(data_[slot], 0, sizeof(data_[slot]));
    rebuild_active_count();
    return true;
}

bool WavetableBank::save_captures(const char* dir) const {
    char path[256], tmp[260];
    capture_path(path, sizeof(path), dir, false);
    capture_path(tmp, sizeof(tmp), dir, true);
    FILE* f = std::fopen(tmp, "wb");
    if (!f) return false;
    CaptureHeader h{CAP_MAGIC, CAP_VERSION, CAPTURE_SLOTS, SIZE, 0};
    bool ok = std::fwrite(&h, sizeof(h), 1, f) == 1;
    for (int slot = FILE_SLOTS; ok && slot < SLOTS; ++slot) {
        const uint8_t on = occupied_[slot] ? 1 : 0;
        ok = std::fwrite(&on, sizeof(on), 1, f) == 1 &&
             std::fwrite(names_[slot], sizeof(names_[slot]), 1, f) == 1 &&
             std::fwrite(data_[slot], sizeof(data_[slot]), 1, f) == 1;
    }
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) { std::remove(tmp); return false; }
    std::remove(path);
    if (std::rename(tmp, path) != 0) { std::remove(tmp); return false; }
    return true;
}

void WavetableBank::load_captures(const char* dir) {
    char path[256];
    capture_path(path, sizeof(path), dir, false);
    FILE* f = std::fopen(path, "rb");
    if (!f) return;
    CaptureHeader h{};
    if (std::fread(&h, sizeof(h), 1, f) != 1 || h.magic != CAP_MAGIC ||
        h.version != CAP_VERSION || h.slots != CAPTURE_SLOTS || h.size != SIZE) {
        std::fclose(f); return;
    }
    bool good = true;
    for (int slot = FILE_SLOTS; slot < SLOTS; ++slot) {
        uint8_t on = 0;
        if (std::fread(&on, sizeof(on), 1, f) != 1 ||
            std::fread(names_[slot], sizeof(names_[slot]), 1, f) != 1 ||
            std::fread(data_[slot], sizeof(data_[slot]), 1, f) != 1) {
            good = false; break;
        }
        names_[slot][sizeof(names_[slot]) - 1] = 0;
        occupied_[slot] = on != 0;
    }
    std::fclose(f);
    if (!good) {
        for (int slot = FILE_SLOTS; slot < SLOTS; ++slot) {
            occupied_[slot] = false; names_[slot][0] = 0;
            std::memset(data_[slot], 0, sizeof(data_[slot]));
        }
    }
}

int WavetableBank::scan_dir(const char* dir) {
    // Rebuild only file-backed slots. Physical IDs 0..15 retain the old stable
    // alphabetical semantics; capture slots are loaded separately from one bank.
    for (int i = 0; i < FILE_SLOTS; ++i) {
        occupied_[i] = false; names_[i][0] = 0;
        std::memset(data_[i], 0, sizeof(data_[i]));
    }

    char found[FILE_SLOTS * 2][20];
    int nfound = 0;
    DIR* d = opendir(dir);
    if (d) {
        while (nfound < FILE_SLOTS * 2) {
            dirent* e = readdir(d);
            if (!e) break;
            const char* nm = e->d_name;
            const std::size_t len = std::strlen(nm);
            if (len < 5 || len >= sizeof(found[0]) || strcasecmp(nm + len - 4, ".wav") != 0) continue;
            std::strncpy(found[nfound], nm, sizeof(found[0]) - 1);
            found[nfound][sizeof(found[0]) - 1] = 0;
            ++nfound;
        }
        closedir(d);
    }
    for (int i = 1; i < nfound; ++i) {
        char key[20]; std::memcpy(key, found[i], sizeof(key));
        int j = i - 1;
        while (j >= 0 && std::strcmp(found[j], key) > 0) {
            std::memcpy(found[j + 1], found[j], sizeof(key)); --j;
        }
        std::memcpy(found[j + 1], key, sizeof(key));
    }

    int file_count = 0;
    for (int i = 0; i < nfound && file_count < FILE_SLOTS; ++i) {
        char path[256];
        const int written = std::snprintf(path, sizeof(path), "%s/%s", dir, found[i]);
        if (written <= 0 || written >= (int)sizeof(path)) continue;
        Sample tmp;
        auto r = load_wav_to_sample(path, tmp, 32000, 32000);
        if ((int)r < 0 || tmp.num_frames() < 2) continue;
        if (!cycle_resample_window(tmp, 0, tmp.num_frames(), data_[file_count], SIZE)) continue;
        std::size_t len = std::strlen(found[i]) - 4;
        if (len >= sizeof(names_[0])) len = sizeof(names_[0]) - 1;
        std::memcpy(names_[file_count], found[i], len); names_[file_count][len] = 0;
        occupied_[file_count] = true;
        ++file_count;
    }
    load_captures(dir);
    rebuild_active_count();
    return active_count_;
}

} // namespace trackr::synth
