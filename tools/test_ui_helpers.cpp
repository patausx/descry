#include "../core/ui/ui_internal.h"
#include <cstdint>
#include <cstdio>

using trackr::ui::bump_clamped;
using trackr::ui::wrap_index;
using trackr::ui::motion_linear;
using trackr::ui::motion_in;
using trackr::ui::motion_out;
using trackr::ui::motion_lerp;
using trackr::ui::motion_stagger;

static int failures = 0;
#define CHECK(expr) do { \
    if (!(expr)) { std::printf("FAIL line %d: %s\n", __LINE__, #expr); ++failures; } \
} while (0)

int main() {
    CHECK(wrap_index(0, -1, 4) == 3);
    CHECK(wrap_index(3,  1, 4) == 0);
    CHECK(wrap_index(1, -9, 4) == 0);
    CHECK(wrap_index(2, 1000001, 4) == 3);
    CHECK(wrap_index(2, -1000001, 4) == 1);
    CHECK(wrap_index(9, 3, 0) == 0);

    CHECK(motion_linear(0, 8) == 0);
    CHECK(motion_linear(8, 8) == 255);
    CHECK(motion_linear(99, 8) == 255);
    CHECK(motion_in(2, 8) < motion_linear(2, 8));
    CHECK(motion_out(2, 8) > motion_linear(2, 8));
    CHECK(motion_lerp(10, 30, 0) == 10);
    CHECK(motion_lerp(10, 30, 255) == 30);
    CHECK(motion_stagger(2, 2, 2, 8) == 0);
    CHECK(motion_stagger(12, 2, 2, 8) == 255);

    uint8_t u = 250;
    CHECK(bump_clamped(u, 1000, 0, 255));
    CHECK(u == 255);
    CHECK(!bump_clamped(u, 1, 0, 255));
    CHECK(u == 255);
    CHECK(bump_clamped(u, -1000, 0, 255));
    CHECK(u == 0);
    CHECK(!bump_clamped(u, -1, 0, 255));

    int8_t s = -120;
    CHECK(bump_clamped(s, -1000, -128, 127));
    CHECK(s == -128);
    CHECK(!bump_clamped(s, -42, -128, 127));
    CHECK(bump_clamped(s, 1000000, -128, 127));
    CHECK(s == 127);
    CHECK(!bump_clamped(s, 42, -128, 127));

    int v = 4;
    CHECK(!bump_clamped(v, 0, -10, 10));
    CHECK(v == 4);
    CHECK(bump_clamped(v, -20, -10, 10));
    CHECK(v == -10);

    if (failures) return 1;
    std::puts("ui helpers: ok");
    return 0;
}
