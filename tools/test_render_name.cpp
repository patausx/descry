// Host regression for the render filename logic (github issue #6:
// "change title before rendering" — the export used to be a single hardcoded
// render.wav, so a second render destroyed the first).
//
// Covers seq::sanitize_basename (project name -> FAT-safe basename) and
// seq::next_free_filename (never overwrite an existing take).
#include "core/sequencer/project.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <sys/stat.h>

using namespace trackr;

static std::string san(const char* in, std::size_t cap = 32) {
    char buf[64];
    assert(cap <= sizeof(buf));
    seq::sanitize_basename(in, buf, cap);
    return buf;
}

static void touch(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "wb");
    assert(f);
    std::fputc('x', f);
    std::fclose(f);
}

int main() {
    // === sanitize_basename ===
    assert(san("ACID TEST") == "ACID_TEST");
    assert(san("untitled") == "untitled");
    // the project rename UI pads with spaces -> must not leak into the filename
    assert(san("MY TRACK            ") == "MY_TRACK");
    assert(san("A  B") == "A_B");                 // runs of '_' collapse
    // path separators / wildcards must never survive: this is the whole reason
    // the sanitizer exists (a name with '/' would fopen into a missing dir)
    assert(san("bad/name") == "badname");
    assert(san("../../etc/passwd") == "....etcpasswd");
    assert(san("a:b*c?d\"e") == "abcde");
    // never empty, never a bare dot chain
    assert(san("") == "untitled");
    assert(san("   ") == "untitled");
    assert(san("...") == "untitled");
    assert(san("///") == "untitled");
    // kept characters
    assert(san("kick-01_v2.1") == "kick-01_v2.1");
    // truncation must stay terminated and within cap
    {
        char buf[8];
        seq::sanitize_basename("ABCDEFGHIJKLMNOP", buf, sizeof(buf));
        assert(std::strlen(buf) == 7);
        assert(std::strcmp(buf, "ABCDEFG") == 0);
    }
    // null input is tolerated
    {
        char buf[16];
        seq::sanitize_basename(nullptr, buf, sizeof(buf));
        assert(std::strcmp(buf, "untitled") == 0);
    }

    // === next_free_filename ===
    const char* dir = "/tmp/descry-render-name-test";
    std::string rm = std::string("rm -rf ") + dir;
    std::system(rm.c_str());
    std::system((std::string("mkdir -p ") + dir).c_str());

    char out[64];
    // empty dir -> plain name
    seq::next_free_filename(dir, "SONG", ".wav", out, sizeof(out));
    assert(std::strcmp(out, "SONG.wav") == 0);

    // existing take must NOT be reused (the actual bug)
    touch(std::string(dir) + "/SONG.wav");
    seq::next_free_filename(dir, "SONG", ".wav", out, sizeof(out));
    assert(std::strcmp(out, "SONG_01.wav") == 0);

    touch(std::string(dir) + "/SONG_01.wav");
    seq::next_free_filename(dir, "SONG", ".wav", out, sizeof(out));
    assert(std::strcmp(out, "SONG_02.wav") == 0);

    // gaps are filled (deleting _01 makes it free again)
    std::remove((std::string(dir) + "/SONG_01.wav").c_str());
    seq::next_free_filename(dir, "SONG", ".wav", out, sizeof(out));
    assert(std::strcmp(out, "SONG_01.wav") == 0);

    // a different project name is unaffected by SONG's takes
    seq::next_free_filename(dir, "OTHER", ".wav", out, sizeof(out));
    assert(std::strcmp(out, "OTHER.wav") == 0);

    // saturation: 100 takes -> recycle _99 instead of failing/looping forever
    touch(std::string(dir) + "/FULL.wav");
    for (int i = 1; i <= 99; ++i) {
        char p[128];
        std::snprintf(p, sizeof(p), "%s/FULL_%02d.wav", dir, i);
        touch(p);
    }
    seq::next_free_filename(dir, "FULL", ".wav", out, sizeof(out));
    assert(std::strcmp(out, "FULL_99.wav") == 0);

    // missing dir -> first name is free (main mkdirs before writing)
    seq::next_free_filename("/tmp/descry-no-such-dir-xyz", "X", ".wav", out, sizeof(out));
    assert(std::strcmp(out, "X.wav") == 0);

    // === end to end: what the user actually sees ===
    {
        seq::Project p;
        std::snprintf(p.name, sizeof(p.name), "%s", "Acid Jam 3");
        char base[32], name[64];
        seq::sanitize_basename(p.name, base, sizeof(base));
        assert(std::strcmp(base, "Acid_Jam_3") == 0);
        seq::next_free_filename(dir, base, ".wav", name, sizeof(name));
        assert(std::strcmp(name, "Acid_Jam_3.wav") == 0);
    }

    std::system(rm.c_str());
    std::puts("render name: ok");
}
