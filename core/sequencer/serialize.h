// serialization of Project to binary
// format: magic "TR3D" + version + raw Project struct
// since we are always on one arch (arm-3ds) endianness/layout are the same
#pragma once
#include "project.h"
#include <cstdio>

namespace trackr::seq {

constexpr uint32_t PROJECT_MAGIC   = 0x44335254;  // "TR3D" little-endian
constexpr uint32_t PROJECT_VERSION = 12;  // v12: reverb size/damp tail (v11: table_speed)
// oldest version we can still load (as a struct prefix + zero-filled tail)
constexpr uint32_t PROJECT_VERSION_MIN = 10;

struct ProjectFileHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t project_size;
    uint32_t reserved;
};

// returns true on success
bool save_project(const Project& p, const char* path);
bool load_project(Project& p, const char* path);

// sanity-check a freshly loaded project before letting the player near it.
// a corrupted / truncated / hand-edited file must not be able to cause OOB
// indexing (instrument >= MAX_INSTRUMENTS in a step) or div-by-zero (bpm 0).
// FIXES anything out of range in place (clamps / resets to safe defaults)
// and returns the number of fields it had to fix (0 = file was clean).
int validate_project(Project& p);

} // namespace trackr::seq
