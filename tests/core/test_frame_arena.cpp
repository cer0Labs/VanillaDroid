#include "core/FrameArena.h"

#include <doctest/doctest.h>

#include <cstdint>

TEST_CASE("FrameArena honours alignment and resets") {
    vd::FrameArena arena(1024);

    void* a = arena.allocate(3, 1);
    void* b = arena.allocate(8, 16);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(b) % 16 == 0);
    CHECK(arena.used() == 24); // 3 bytes, padded to 16, plus 8

    arena.reset();
    CHECK(arena.used() == 0);
    CHECK(arena.peak() == 24);
    CHECK(arena.allocate(8, 16) == a); // allocation restarts at the beginning of the block
}

TEST_CASE("FrameArena value-initializes arrays") {
    vd::FrameArena arena(256);
    int* values = arena.allocateArray<int>(16);
    REQUIRE(values != nullptr);
    for (int i = 0; i < 16; ++i) {
        CHECK(values[i] == 0);
    }
}
