#include "serialize.h"
#include <cstring>

namespace trackr::seq {

bool save_project(const Project& p, const char* path) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;

    ProjectFileHeader h{};
    h.magic   = PROJECT_MAGIC;
    h.version = PROJECT_VERSION;
    h.project_size = sizeof(Project);

    std::fwrite(&h, sizeof(h), 1, f);
    std::fwrite(&p, sizeof(Project), 1, f);
    std::fclose(f);
    return true;
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

    std::size_t n = h.project_size < sizeof(Project) ? h.project_size : sizeof(Project);
    if (std::fread(&p, n, 1, f) != 1) { std::fclose(f); return false; }
    if (n < sizeof(Project)) {
        // zero-fill the appended tail (table_speed 0 = legacy default = 1)
        std::memset(reinterpret_cast<char*>(&p) + n, 0, sizeof(Project) - n);
    }
    std::fclose(f);
    return true;
}

} // namespace trackr::seq
