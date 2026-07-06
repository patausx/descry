// user wavetable bank implementation. see wavetable.h.
#include "wavetable.h"
#include "wav_loader.h"
#include "sampler.h"
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <dirent.h>

namespace trackr::synth {

WavetableBank& WavetableBank::instance() {
    static WavetableBank bank;
    return bank;
}

// resample an arbitrary-length mono cycle into SIZE points (linear interp).
// whole file = one cycle: works for AKWF (~600pts) and any exported cycle.
static void cycle_resample(const Sample& src, fx::q15* dst, int dst_len) {
    const uint32_t n = src.num_frames();
    if (n < 2) { std::memset(dst, 0, dst_len * sizeof(fx::q15)); return; }
    const int ch = src.channels ? src.channels : 1;
    for (int i = 0; i < dst_len; ++i) {
        // position in source: i/dst_len * n (fixed point 32.16)
        uint64_t pos = ((uint64_t)i << 16) * n / (uint32_t)dst_len;
        uint32_t i0 = (uint32_t)(pos >> 16);
        uint32_t frac = (uint32_t)(pos & 0xFFFF);
        uint32_t i1 = (i0 + 1) % n;                     // wrap: it's a cycle
        int32_t a = src.data[i0 * ch];                  // left/mono channel
        int32_t b = src.data[i1 * ch];
        dst[i] = (fx::q15)(a + (((b - a) * (int32_t)frac) >> 16));
    }
    // remove DC offset (cheap mean subtract) - many single-cycle packs have some
    int64_t sum = 0;
    for (int i = 0; i < dst_len; ++i) sum += dst[i];
    int32_t mean = (int32_t)(sum / dst_len);
    if (mean != 0)
        for (int i = 0; i < dst_len; ++i) {
            int32_t v = dst[i] - mean;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            dst[i] = (fx::q15)v;
        }
}

int WavetableBank::scan_dir(const char* dir) {
    count_ = 0;

    // collect .wav names first so we can sort alphabetically (stable slot order)
    char found[SLOTS * 2][20];
    int  nfound = 0;
    DIR* d = opendir(dir);
    if (!d) return 0;
    while (nfound < SLOTS * 2) {
        dirent* e = readdir(d);
        if (!e) break;
        const char* nm = e->d_name;
        if (std::strlen(nm) < 5 || std::strlen(nm) >= sizeof(found[0])) continue;
        std::size_t len = std::strlen(nm);
        if (strcasecmp(nm + len - 4, ".wav") != 0) continue;
        std::strncpy(found[nfound], nm, sizeof(found[0]) - 1);
        found[nfound][sizeof(found[0]) - 1] = 0;
        ++nfound;
    }
    closedir(d);
    // insertion sort by name (char arrays can't be std::sort'ed directly;
    // n is tiny so this is fine)
    for (int i = 1; i < nfound; ++i) {
        char key[20];
        std::memcpy(key, found[i], sizeof(key));
        int j = i - 1;
        while (j >= 0 && std::strcmp(found[j], key) > 0) {
            std::memcpy(found[j + 1], found[j], sizeof(key));
            --j;
        }
        std::memcpy(found[j + 1], key, sizeof(key));
    }

    // load up to SLOTS files. a single cycle is tiny - a temp Sample on the
    // heap per file is fine (freed on scope exit).
    for (int i = 0; i < nfound && count_ < SLOTS; ++i) {
        char path[200];
        std::snprintf(path, sizeof(path), "%s/%s", dir, found[i]);
        Sample tmp;
        // 1 second cap is plenty for any single-cycle file (AKWF ~600 frames)
        auto r = load_wav_to_sample(path, tmp, 32000, 32000);
        if ((int)r < 0 || tmp.num_frames() < 2) continue;
        cycle_resample(tmp, data_[count_], SIZE);
        // display name: strip .wav, uppercase-ish truncate
        std::size_t len = std::strlen(found[i]) - 4;
        if (len >= sizeof(names_[0])) len = sizeof(names_[0]) - 1;
        std::memcpy(names_[count_], found[i], len);
        names_[count_][len] = 0;
        ++count_;
    }
    return count_;
}

} // namespace trackr::synth
