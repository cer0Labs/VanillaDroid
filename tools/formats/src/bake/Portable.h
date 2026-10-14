#pragma once

#include "core/Types.h"

#include <glm/glm.hpp>

#include <cmath>
#include <iterator>
#include <utility>

// Phase 22 (ADR-0005 §3): the same packs from the same client on every platform. A pack's recipe is a
// digest of what it holds, so the baker built for Windows (MSVC), Android and macOS (clang, libc++) must
// write the same bytes. Three things differ between those and stay out of the bake:
// - fused multiply-add (clang contracts a * b + c on arm64): the bake is built with -ffp-contract=off;
// - the C library's float sin / cos / tan / atan2, which may differ in the last bit: the functions below
//   compute in double and round once: alike everywhere but for vanishingly rare near-ties;
// - algorithms whose result order is the library's own (std::partition, std::sort of equal elements,
//   std::nth_element): portablePartition below, std::stable_sort, or comparisons without ties.
namespace vd::bake {

inline f32 portableSin(f32 radians) {
    return static_cast<f32>(std::sin(static_cast<f64>(radians)));
}
inline f32 portableCos(f32 radians) {
    return static_cast<f32>(std::cos(static_cast<f64>(radians)));
}
inline f32 portableTan(f32 radians) {
    return static_cast<f32>(std::tan(static_cast<f64>(radians)));
}
inline f32 portableAtan2(f32 y, f32 x) {
    return static_cast<f32>(std::atan2(static_cast<f64>(y), static_cast<f64>(x)));
}

// glm::rotate (glm 1.0.3, the same arithmetic) with portableSin / portableCos.
inline glm::mat4 portableRotate(const glm::mat4& m, f32 radians, const glm::vec3& v) {
    const f32 c = portableCos(radians);
    const f32 s = portableSin(radians);
    const glm::vec3 axis(glm::normalize(v));
    const glm::vec3 temp((1.0f - c) * axis);
    glm::mat4 rotate;
    rotate[0][0] = c + temp[0] * axis[0];
    rotate[0][1] = temp[0] * axis[1] + s * axis[2];
    rotate[0][2] = temp[0] * axis[2] - s * axis[1];
    rotate[1][0] = temp[1] * axis[0] - s * axis[2];
    rotate[1][1] = c + temp[1] * axis[1];
    rotate[1][2] = temp[1] * axis[2] + s * axis[0];
    rotate[2][0] = temp[2] * axis[0] + s * axis[1];
    rotate[2][1] = temp[2] * axis[1] - s * axis[0];
    rotate[2][2] = c + temp[2] * axis[2];
    glm::mat4 result;
    result[0] = m[0] * rotate[0][0] + m[1] * rotate[0][1] + m[2] * rotate[0][2];
    result[1] = m[0] * rotate[1][0] + m[1] * rotate[1][1] + m[2] * rotate[1][2];
    result[2] = m[0] * rotate[2][0] + m[1] * rotate[2][1] + m[2] * rotate[2][2];
    result[3] = m[3];
    return result;
}

// std::partition as Microsoft's STL does it for bidirectional iterators (so the packs baked on Windows so
// far stay the same): the first element that fails `pred` swaps with the last one that passes, until they
// meet. Returns the first element of the second group.
template <typename It, typename Pred> It portablePartition(It first, It last, Pred pred) {
    for (;;) {
        for (;;) { // skip the elements already in place at the front
            if (first == last) {
                return first;
            }
            if (!pred(*first)) {
                break;
            }
            ++first;
        }
        do { // and at the back
            --last;
            if (first == last) {
                return first;
            }
        } while (!pred(*last));
        std::iter_swap(first, last);
        ++first;
    }
}

} // namespace vd::bake
