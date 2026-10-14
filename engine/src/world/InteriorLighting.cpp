#include "world/InteriorLighting.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace vd {
namespace {

f32 signNotZero(f32 v) {
    return v >= 0.0f ? 1.0f : -1.0f;
}

u32 toSnorm8Bits(f32 v) {
    return static_cast<u32>(static_cast<u8>(static_cast<i8>(std::lround(std::clamp(v, -1.0f, 1.0f) * 127.0f))));
}

f32 fromSnorm8Bits(u32 bits) {
    return std::max(static_cast<f32>(static_cast<i8>(static_cast<u8>(bits))) / 127.0f, -1.0f);
}

f32 luminance(const glm::vec3& c) {
    return 0.299f * c.x + 0.587f * c.y + 0.114f * c.z;
}

} // namespace

u32 packRgb8(const glm::vec3& rgb) {
    const glm::vec3 c = glm::clamp(rgb, 0.0f, 1.0f) * 255.0f + 0.5f;
    return static_cast<u32>(c.x) | (static_cast<u32>(c.y) << 8) | (static_cast<u32>(c.z) << 16);
}

glm::vec3 unpackRgb8(u32 rgba) {
    return glm::vec3(static_cast<f32>(rgba & 0xFF), static_cast<f32>((rgba >> 8) & 0xFF),
                     static_cast<f32>((rgba >> 16) & 0xFF)) /
           255.0f;
}

std::array<u32, 2> packInstanceLight(const InstanceLight& light) {
    // Octahedral direction (the shaders decode it: shaders/interior.glsl).
    glm::vec3 n = light.direction;
    const f32 sum = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
    n = sum > 1e-6f ? n / sum : glm::vec3(0.0f, 0.0f, 1.0f);
    glm::vec2 e(n.x, n.y);
    if (n.z < 0.0f) {
        e = glm::vec2((1.0f - std::abs(n.y)) * signNotZero(n.x), (1.0f - std::abs(n.x)) * signNotZero(n.y));
    }
    std::array<u32, 2> packed{packRgb8(light.ambient) | (toSnorm8Bits(e.x) << 24),
                              packRgb8(light.direct) | (toSnorm8Bits(e.y) << 24)};
    if (packed[0] == 0 && packed[1] == 0) {
        packed[0] = 1u << 16; // a black interior is still an interior (all zeros means outdoors)
    }
    return packed;
}

InstanceLight unpackInstanceLight(const std::array<u32, 2>& packed) {
    InstanceLight out;
    out.ambient = unpackRgb8(packed[0]);
    out.direct = unpackRgb8(packed[1]);
    const glm::vec2 e(fromSnorm8Bits(packed[0] >> 24), fromSnorm8Bits(packed[1] >> 24));
    glm::vec3 n(e.x, e.y, 1.0f - std::abs(e.x) - std::abs(e.y));
    if (n.z < 0.0f) {
        n = glm::vec3((1.0f - std::abs(e.y)) * signNotZero(e.x), (1.0f - std::abs(e.x)) * signNotZero(e.y), n.z);
    }
    out.direction = glm::normalize(n);
    return out;
}

InstanceLight interiorLight(const glm::vec3& baked, const glm::vec3& wmoAmbient, const glm::vec3& towardsLight) {
    InstanceLight out;
    out.ambient = wmoAmbient + baked * kInteriorFill;
    out.direct = baked * (1.0f - kInteriorFill);
    const f32 length = glm::length(towardsLight);
    out.direction = length > 1e-4f ? towardsLight / length : glm::vec3(0.0f, 0.0f, 1.0f);
    return out;
}

f32 lightAttenuation(const pack::ModelLight& light, f32 distance) {
    if (light.attenEnd <= 0.0f) {
        return 1.0f;
    }
    if (distance <= light.attenStart) {
        return 1.0f;
    }
    if (distance >= light.attenEnd) {
        return 0.0f;
    }
    return (light.attenEnd - distance) / std::max(light.attenEnd - light.attenStart, 1e-4f);
}

f32 lightStrength(const pack::ModelLight& light, f32 distance) {
    return light.intensity * luminance(unpackRgb8(light.color)) * lightAttenuation(light, distance);
}

glm::vec3 gridLight(const pack::ModelGroupLighting& group, std::span<const u32> cells, const glm::vec3& point) {
    const glm::ivec3 size(group.gridSize[0], group.gridSize[1], group.gridSize[2]);
    const u64 count = static_cast<u64>(size.x) * static_cast<u64>(size.y) * static_cast<u64>(size.z);
    if (group.cellSize <= 0.0f || count == 0 || group.firstCell + count > cells.size()) {
        return unpackRgb8(group.average);
    }
    // Trilinear between cell centers, so the light changes smoothly as an actor walks.
    const glm::vec3 local =
        glm::clamp((point - glm::vec3(group.gridMin[0], group.gridMin[1], group.gridMin[2])) / group.cellSize - 0.5f,
                   glm::vec3(0.0f), glm::vec3(size - 1));
    const glm::ivec3 base = glm::min(glm::ivec3(local), size - 1);
    const glm::vec3 t = local - glm::vec3(base);
    glm::vec3 sum(0.0f);
    for (u32 corner = 0; corner < 8; ++corner) {
        const glm::ivec3 c = glm::min(base + glm::ivec3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1), size - 1);
        const f32 w = ((corner & 1) ? t.x : 1.0f - t.x) * (((corner >> 1) & 1) ? t.y : 1.0f - t.y) *
                      (((corner >> 2) & 1) ? t.z : 1.0f - t.z);
        sum += w * unpackRgb8(cells[group.firstCell + (static_cast<usize>(c.z) * size.y + c.y) * size.x + c.x]);
    }
    return sum;
}

f32 fogWeight(const pack::ModelFog& fog, const glm::vec3& point) {
    const f32 distance = glm::length(point - glm::vec3(fog.position[0], fog.position[1], fog.position[2]));
    if (distance <= fog.innerRadius) {
        return 1.0f;
    }
    if (distance >= fog.outerRadius) {
        return 0.0f;
    }
    return (fog.outerRadius - distance) / std::max(fog.outerRadius - fog.innerRadius, 1e-4f);
}

InteriorFog interiorFog(std::span<const pack::ModelFog> fogs, std::span<const u8, 4> groupFogs, const glm::vec3& point,
                        bool underwater) {
    InteriorFog out;
    if (fogs.empty()) {
        return out;
    }
    const auto half = [&](const pack::ModelFog& fog) {
        return InteriorFog{unpackRgb8(underwater ? fog.underwaterColor : fog.color),
                           underwater ? fog.underwaterEnd : fog.end,
                           underwater ? fog.underwaterStartScale : fog.startScale};
    };
    out = half(fogs[0]);
    // The group's fogs within their outer radius (0x69dfdc), not the infinite ones (0x69dfe6), farthest first: the
    // client's heap pops the largest distance first and each blends over what is there.
    std::array<std::pair<f32, u8>, 4> reaching{};
    u32 count = 0;
    for (const u8 id : groupFogs) {
        if (id == 0 || id >= fogs.size() || (fogs[id].flags & pack::kModelFogInfinite)) {
            continue;
        }
        const pack::ModelFog& fog = fogs[id];
        const f32 distance = glm::length(point - glm::vec3(fog.position[0], fog.position[1], fog.position[2]));
        if (distance < fog.outerRadius) {
            reaching[count++] = {distance, id};
        }
    }
    std::sort(reaching.begin(), reaching.begin() + count, [](const auto& a, const auto& b) { return a.first > b.first; });
    for (u32 i = 0; i < count; ++i) {
        const pack::ModelFog& fog = fogs[reaching[i].second];
        const f32 weight = fogWeight(fog, point);
        const InteriorFog local = half(fog);
        out.color = glm::mix(out.color, local.color, weight);
        out.end = glm::mix(out.end, local.end, weight);
        out.startScale = glm::mix(out.startScale, local.startScale, weight);
    }
    return out;
}

f32 nightGlowFactor(f32 timeOfDay) {
    const f32 hour = std::fmod(std::max(timeOfDay, 0.0f), 2880.0f) / 120.0f;
    if (hour < 5.0f || hour >= 20.0f) {
        return 1.0f;
    }
    if (hour < 7.0f) {
        return 1.0f - (hour - 5.0f) * 0.5f;
    }
    if (hour < 18.0f) {
        return 0.0f;
    }
    return (hour - 18.0f) * 0.5f;
}

f32 lightIntensity(const pack::ModelLight& light, std::span<const pack::ModelLightKey> keys, u32 ms) {
    if (light.keyCount == 0 || light.loopMs == 0 || u64{light.firstKey} + light.keyCount > keys.size()) {
        return light.intensity;
    }
    const std::span<const pack::ModelLightKey> own = keys.subspan(light.firstKey, light.keyCount);
    const u32 t = ms % light.loopMs;
    if (t <= own.front().time) {
        return own.front().intensity;
    }
    for (usize k = 1; k < own.size(); ++k) {
        if (t <= own[k].time) {
            const u32 span = own[k].time - own[k - 1].time;
            const f32 f = span ? static_cast<f32>(t - own[k - 1].time) / static_cast<f32>(span) : 0.0f;
            return own[k - 1].intensity + (own[k].intensity - own[k - 1].intensity) * f;
        }
    }
    return own.back().intensity;
}

glm::vec2 terrainLayerOffset(u8 flags, f64 seconds) {
    if ((flags & kTerrainLayerAnimated) == 0) {
        return glm::vec2(0.0f);
    }
    // The client's table (8 directions, the diagonals not normalized); speed 1..7 is one texture repeat every
    // 11.2 / speed seconds.
    static constexpr f32 kDirectionU[8] = {0.0f, -1.0f, -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f};
    static constexpr f32 kDirectionV[8] = {1.0f, 1.0f, 0.0f, -1.0f, -1.0f, -1.0f, 0.0f, 1.0f};
    const u32 direction = flags & 0x07u;
    const f64 speed = static_cast<f64>((flags >> 3) & 0x07u);
    const f64 cycles = seconds * speed / 11.2;
    const f32 phase = static_cast<f32>(cycles - std::floor(cycles));
    return glm::vec2(kDirectionU[direction], kDirectionV[direction]) * phase;
}

} // namespace vd
