// Phase 22 (phonebaker): the bake pieces that must give the same result everywhere (bake/Portable.h).
#include "bake/Portable.h"

#include <doctest/doctest.h>

#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

// astc-encoder's sinf / cosf / logf (cmake/astcenc), last: its macros are for astc-encoder's own files.
#include "astcenc_portable_math.h"
#undef sinf
#undef cosf
#undef logf

using namespace vd;

namespace {

bool sameBits(const glm::mat4& a, const glm::mat4& b) {
    return std::memcmp(&a[0][0], &b[0][0], sizeof(glm::mat4)) == 0;
}

} // namespace

TEST_CASE("portableRotate: glm::rotate's matrix") {
    // A placement's three turns, at angles every 0.37 degrees and at the round ones.
    const glm::mat4 start = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, -2.0f, 7.5f));
    const glm::vec3 axes[] = {glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f),
                              glm::vec3(1.0f, 0.0f, 0.0f)};
    u32 checked = 0, different = 0;
    f32 worst = 0.0f;
    for (i32 step = -1946; step <= 1946; ++step) {
        const f32 degrees = static_cast<f32>(step) * 0.37f;
        for (const f32 angle : {degrees, std::round(degrees), std::round(degrees) - 90.0f}) {
            for (const glm::vec3& axis : axes) {
                const glm::mat4 ours = bake::portableRotate(start, glm::radians(angle), axis);
                const glm::mat4 glms = glm::rotate(start, glm::radians(angle), axis);
                ++checked;
                if (!sameBits(ours, glms)) {
                    ++different;
                }
                for (u32 c = 0; c < 4; ++c) {
                    for (u32 r = 0; r < 4; ++r) {
                        worst = std::max(worst, std::abs(ours[c][r] - glms[c][r]));
                    }
                }
            }
        }
    }
    // The C library's float sine and cosine are off by a bit now and then (MSVC's: 87 of these 35037): the
    // placements they turn move by at most that.
    MESSAGE(checked << " rotations, " << different << " not bit for bit glm's, largest difference " << worst);
    CHECK(worst < 1e-6f);
    CHECK(different * 50 < checked);
}

TEST_CASE("portableSin, portableCos, portableTan, portableAtan2: the rounded double") {
    CHECK(bake::portableSin(0.0f) == 0.0f);
    CHECK(bake::portableCos(0.0f) == 1.0f);
    CHECK(bake::portableTan(0.5f) == static_cast<f32>(std::tan(0.5)));
    CHECK(bake::portableAtan2(1.0f, -1.0f) == static_cast<f32>(std::atan2(1.0, -1.0)));
    CHECK(bake::portableAtan2(0.0f, 1.0f) == 0.0f);
}

TEST_CASE("portablePartition: std::partition of Microsoft's STL, on every platform") {
    std::mt19937 random(22);
    for (u32 round = 0; round < 200; ++round) {
        std::vector<u32> values(random() % 300);
        for (u32& v : values) {
            v = random() % 1000;
        }
        const u32 split = random() % 1000;
        const auto pred = [&](u32 v) { return v < split; };
        std::vector<u32> ours = values;
        const auto mid = bake::portablePartition(ours.begin(), ours.end(), pred);
        // A partition of the same elements.
        CHECK(std::all_of(ours.begin(), mid, pred));
        CHECK(std::none_of(mid, ours.end(), pred));
        std::vector<u32> sortedOurs = ours, sortedValues = values;
        std::sort(sortedOurs.begin(), sortedOurs.end());
        std::sort(sortedValues.begin(), sortedValues.end());
        CHECK(sortedOurs == sortedValues);
#if defined(_MSC_VER)
        // In the same order as the STL the packs were baked with.
        std::vector<u32> theirs = values;
        const auto theirMid = std::partition(theirs.begin(), theirs.end(), pred);
        CHECK(ours == theirs);
        CHECK(mid - ours.begin() == theirMid - theirs.begin());
#endif
    }
    // Two-ended: the first element out of place swaps with the last one.
    std::vector<u32> fixed = {5, 1, 7, 2, 9, 3};
    const auto mid = bake::portablePartition(fixed.begin(), fixed.end(), [](u32 v) { return v < 4; });
    CHECK(fixed == std::vector<u32>{3, 1, 2, 7, 9, 5});
    CHECK(mid - fixed.begin() == 3);
}

TEST_CASE("astc-encoder's angular tables: the same on every platform") {
    // The arguments astcenc_weight_align.cpp's prepare_angular_tables() takes, in its order, and an FNV-1a
    // hash of the tables: the one the PC's packs were baked with (MSVC, i7-6700K).
    constexpr f32 kPi = 3.14159265358979323846f;
    u64 hash = 1469598103934665603ull;
    u32 offTheDouble = 0;
    for (u32 j = 0; j < 64; ++j) {
        for (u32 i = 0; i < 32; ++i) {
            const f32 arg = (2.0f * kPi / (64 - 1.0f)) * static_cast<f32>(i + 1) * static_cast<f32>(j);
            const f32 s = vd_astcenc::sinf(arg), c = vd_astcenc::cosf(arg);
            u32 sb, cb;
            std::memcpy(&sb, &s, 4);
            std::memcpy(&cb, &c, 4);
            hash = (hash ^ sb) * 1099511628211ull;
            hash = (hash ^ cb) * 1099511628211ull;
            offTheDouble += s != static_cast<f32>(std::sin(static_cast<f64>(arg)));
        }
    }
    CHECK(hash == 0x72229eb48885521aull);
    CHECK(offTheDouble == 1); // sin_table[59][5], as MSVC's sinf gave it
    // The logarithms astcenc_config_init() takes for the block sizes the baker uses.
    const auto bits = [](f32 v) {
        u32 b;
        std::memcpy(&b, &v, 4);
        return b;
    };
    CHECK(bits(vd_astcenc::logf(16.0f)) == 0x40317218u);
    CHECK(bits(vd_astcenc::logf(36.0f)) == 0x40655860u);
    CHECK(bits(vd_astcenc::logf(10.0f)) == 0x40135d8eu);
}
