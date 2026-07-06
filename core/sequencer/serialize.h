// serialization of Project to binary
// format: magic "TR3D" + version + raw Project struct
// since we are always on one arch (arm-3ds) endianness/layout are the same
#pragma once
#include "project.h"
#include <cstdio>

namespace trackr::seq {

constexpr uint32_t PROJECT_MAGIC   = 0x44335254;  // "TR3D" little-endian
constexpr uint32_t PROJECT_VERSION = 11;  // v11: table_speed tail (v10: sidechain duck)
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

} // namespace trackr::seq
