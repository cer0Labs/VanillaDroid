#include "world/GrassPlacement.h"

#include "pack/TerrainFormat.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vd::grass {
namespace {

// WoW.exe 0x802700: the generator's table. Its four lags read the first 47, 53, 59 and 61 words.
constexpr u32 kRandomTable[61] = {
    0x9927148e, 0x08c7aafd, 0x1f3ee6d5, 0xda55bbf6, 0x6a4aa075, 0xff97bde8, 0x9fbc9bde, 0x46a18a81, 0x63e30b6e,
    0x5d6c7a76, 0xca69d388, 0x25b947c3, 0x3fa2ab83, 0xba7c41a6, 0x0195ace5, 0xc109cf7e, 0x717062d9, 0x0205db8d,
    0x54ef8724, 0x3037d4c6, 0x7bcb1bd0, 0xecd8e4b8, 0xdcadce49, 0xc494a913, 0x0dae398f, 0x0edd5218, 0x85f5fa78,
    0x6dafd258, 0x3b53b2a4, 0xbe50a551, 0x11f42dfc, 0xf1169848, 0x663ddf86, 0x2f2e445e, 0x176b0736, 0xb64c298b,
    0xe75f89e2, 0xe121a7cd, 0xed65c94d, 0x239ceefe, 0x04b77d33, 0x402a9a9e, 0xf35b10b3, 0x921c7782, 0x571e4e20,
    0x8c067222, 0xfb732c67, 0xbf0ac259, 0x0cf95c79, 0x68121a28, 0x42193474, 0xf884c0b1, 0x9d15f038, 0x6f3af260,
    0x91eb90b4, 0x61357f1d, 0x5603325a, 0x932bc5a3, 0x434b0f80, 0x3ce0a8f7, 0x2664d196};

constexpr u32 rotl(u32 v, u32 n) {
    return (v << n) | (v >> (32 - n));
}

// A lag's byte offset one word back, wrapping at its length.
constexpr u32 stepBack(u32 offset, u32 back, u32 length) {
    const i32 next = static_cast<i32>(offset) - static_cast<i32>(back);
    return static_cast<u32>(next < 0 ? next + static_cast<i32>(length) : next);
}

f32 bitsToFloat(u32 bits) {
    f32 f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// The client's float constants (the x87 code computes in double and stores floats where noted).
const f32 kHalfCell = bitsToFloat(0x40055555u); // 2.0833333
const f32 kShadowTexels = bitsToFloat(0x3ff5c28fu); // 1.92: shadow texels per yard (64 / 33.3)
const f32 kScaleSpread = bitsToFloat(0x3dcccccdu);  // 0.1
const f32 kPi = bitsToFloat(0x40490fdbu);

struct Vec {
    f64 x, y, z;
};

Vec sub(const Vec& a, const Vec& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

// A chunk vertex in the client's chunk-local frame: x along world +X (-t), y along +Y (-s), z the height.
Vec outerVertex(const f32* heights, u32 column, u32 row) {
    return {-static_cast<f64>(row) * pack::kUnitSize, -static_cast<f64>(column) * pack::kUnitSize,
            heights[row * 17 + column]};
}

// One texture batch of the chunk (CDetailDoodadInst): what it holds so far.
struct Batch {
    u64 texture = 0; // 0 = unused
    u32 vertices = 0;
    u32 indices = 0;
};

} // namespace

void ClientRandom::setSeed(u32 seed) {
    m_sum = seed;
    m_indices = ((seed % 47) << 24 | (seed % 53) << 16 | (seed % 59) << 8 | (seed % 61)) << 2;
}

u32 ClientRandom::next() {
    const u32 lag47 = stepBack(m_indices >> 24, 4, 47 * 4);
    const u32 lag53 = stepBack((m_indices >> 16) & 0xFF, 12, 53 * 4);
    const u32 lag59 = stepBack((m_indices >> 8) & 0xFF, 24, 59 * 4);
    const u32 lag61 = stepBack(m_indices & 0xFF, 28, 61 * 4);
    const u32 mixed = rotl(kRandomTable[lag59 / 4], 3) ^ rotl(kRandomTable[lag53 / 4], 2) ^
                      kRandomTable[lag61 / 4] ^ rotl(kRandomTable[lag47 / 4], 1);
    m_indices = lag47 << 24 | lag53 << 16 | lag59 << 8 | lag61;
    m_sum += mixed;
    return m_sum;
}

f32 ClientRandom::toSigned(u32 random) {
    const f32 oneToTwo = bitsToFloat((random & 0x7FFFFFu) | 0x3F800000u);
    return static_cast<i32>(random) >= 0 ? oneToTwo - 2.0f : 2.0f - oneToTwo;
}

u32 maxPerChunk(u32 frillDensity, u32 maxDensity) {
    return std::min(frillDensity, kMaxFrillDensity) * maxDensity;
}

f32 fadeAt(f32 depth, f32 fadeStart, f32 fadeEnd) {
    const f32 ramp = (depth - fadeStart) / std::max(fadeEnd - fadeStart, 0.001f);
    return std::clamp(1.0f - ramp, 0.0f, 1.0f) * (252.0f / 255.0f);
}

u32 placeChunk(const ChunkInput& input, std::span<const pack::GrassEffect> effects,
               std::span<const pack::GrassModel> models, u32 frillDensity, std::span<PlacedGrass> out,
               PlacementStats* stats) {
    const pack::GrassChunk* chunk = input.chunk;
    if (!chunk || !input.heights || (chunk->flags & pack::kGrassChunkGrows) == 0 || frillDensity == 0 ||
        out.empty()) {
        return 0;
    }
    frillDensity = std::min(frillDensity, kMaxFrillDensity);
    ClientRandom random(input.seed);
    // 1. The cells, drawn up front (two randoms each).
    u8 columns[kMaxFrillDensity];
    u8 rows[kMaxFrillDensity];
    for (u32 i = 0; i < frillDensity; ++i) {
        columns[i] = static_cast<u8>(random.next() & 7u);
        rows[i] = static_cast<u8>(random.next() & 7u);
    }
    // 4. CDetailDoodadInst's budget per batch: both counts stay strictly under it.
    const u32 budget = std::min(frillDensity * 64u, 8192u);
    Batch batches[kMaxBatches];
    u32 count = 0;
    PlacementStats local;
    local.cellsDrawn = frillDensity;
    for (u32 i = 0; i < frillDensity; ++i) {
        const u32 column = columns[i];
        const u32 row = rows[i];
        if ((chunk->noEffect[row] >> column) & 1u) {
            continue;
        }
        if ((chunk->holes >> ((column >> 1) + 4 * (row >> 1))) & 1u) {
            continue;
        }
        const u32 layer = (chunk->cellLayers[row] >> (column * 2)) & 3u;
        const u16 effectIndex = chunk->effects[layer];
        if (effectIndex == pack::kNoGrassEffect || effectIndex >= effects.size()) {
            continue;
        }
        const pack::GrassEffect& effect = effects[effectIndex];
        ++local.cellsGrowing;
        const f32 cellX = static_cast<f32>(static_cast<f64>(column) * kCellSize); // along s
        const f32 cellY = static_cast<f32>(static_cast<f64>(row) * kCellSize);    // along t
        // The cell's quad: four triangles around its center vertex (the terrain's level 0 fan).
        const Vec center{-(static_cast<f64>(row) + 0.5) * pack::kUnitSize,
                         -(static_cast<f64>(column) + 0.5) * pack::kUnitSize,
                         input.heights[row * 17 + 9 + column]};
        for (u32 j = 0; j < effect.density; ++j) {
            const f32 offsetX = static_cast<f32>(ClientRandom::toSigned(random.next()) * static_cast<f64>(kHalfCell) +
                                                 kHalfCell); // along s, 0..cell
            const f32 offsetY = static_cast<f32>(ClientRandom::toSigned(random.next()) * static_cast<f64>(kHalfCell) +
                                                 kHalfCell); // along t
            const u16 model = effect.models[(j + i) & 3u];
            if (model == pack::kNoGrassModel) {
                continue;
            }
            // The MCSH bit under it (x87 fistp: round to nearest even).
            bool shaded = false;
            if (input.shadowBits) {
                const f32 texelS = static_cast<f32>((static_cast<f64>(offsetX) + cellX) * kShadowTexels);
                const f32 texelT = static_cast<f32>((static_cast<f64>(offsetY) + cellY) * kShadowTexels);
                const i64 s = std::llrint(static_cast<f64>(texelS) - 0.5);
                const i64 t = std::llrint(static_cast<f64>(texelT) - 0.5);
                const i64 byte = (s >> 3) + 8 * t;
                shaded = byte >= 0 && byte < static_cast<i64>(pack::kGrassShadowBytes) &&
                         ((input.shadowBits[byte] >> (s & 7)) & 1u) != 0;
            }
            // Which triangle of the fan: by the two diagonals of the cell.
            const u32 triangle = (static_cast<f64>(offsetY) - offsetX < 0.0 ? 1u : 0u) +
                                 (static_cast<f64>(offsetX) + offsetY - kCellSize > 0.0 ? 2u : 0u);
            Vec a{};
            Vec b{};
            switch (triangle) {
            case 0: // west edge (the quad's first column of vertices)
                a = outerVertex(input.heights, column, row + 1);
                b = outerVertex(input.heights, column, row);
                break;
            case 1: // north edge (its first row)
                a = outerVertex(input.heights, column, row);
                b = outerVertex(input.heights, column + 1, row);
                break;
            case 2: // south edge (its last row)
                a = outerVertex(input.heights, column + 1, row + 1);
                b = outerVertex(input.heights, column, row + 1);
                break;
            default: // east edge (its last column)
                a = outerVertex(input.heights, column + 1, row);
                b = outerVertex(input.heights, column + 1, row + 1);
                break;
            }
            const Vec u = sub(a, center);
            const Vec v = sub(b, center);
            Vec n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
            const f64 length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (length <= 0.0) {
                continue;
            }
            const f64 up = n.z < 0.0 ? -1.0 / length : 1.0 / length;
            n = {n.x * up, n.y * up, n.z * up};
            const f32 x = static_cast<f32>(-static_cast<f64>(offsetY) - cellY);
            const f32 y = static_cast<f32>(-static_cast<f64>(offsetX) - cellX);
            const f32 z = static_cast<f32>(center.z - (n.x * (x - center.x) + n.y * (y - center.y)) / n.z);
            const f32 scale =
                static_cast<f32>(ClientRandom::toSigned(random.next()) * static_cast<f64>(kScaleSpread) + 1.0);
            const f64 angle = static_cast<f64>(static_cast<f32>(ClientRandom::toSigned(random.next()) *
                                                                static_cast<f64>(kPi)));
            // The batch it goes into (or nothing).
            if (model >= models.size()) {
                ++local.dropped;
                continue;
            }
            const pack::GrassModel& m = models[model];
            u32 slot = kMaxBatches;
            for (u32 k = 0; k < kMaxBatches; ++k) {
                if (batches[k].texture == m.texture && batches[k].texture != 0 &&
                    batches[k].vertices + m.vertexCount < budget && batches[k].indices + m.indexCount < budget) {
                    slot = k;
                    break;
                }
            }
            if (slot == kMaxBatches) {
                for (u32 k = 0; k < kMaxBatches; ++k) {
                    if (batches[k].texture == 0) {
                        batches[k].texture = m.texture != 0 ? m.texture : ~0ull;
                        slot = k;
                        break;
                    }
                }
            }
            if (slot == kMaxBatches) {
                ++local.dropped;
                continue;
            }
            batches[slot].vertices += m.vertexCount;
            batches[slot].indices += m.indexCount;
            if (count >= out.size()) {
                ++local.dropped;
                continue;
            }
            PlacedGrass& p = out[count++];
            p.position[0] = x;
            p.position[1] = y;
            p.position[2] = z;
            p.cosine = static_cast<f32>(std::cos(angle));
            p.sine = static_cast<f32>(std::sin(angle));
            p.scale = scale;
            p.normal[0] = static_cast<f32>(n.x);
            p.normal[1] = static_cast<f32>(n.y);
            p.normal[2] = static_cast<f32>(n.z);
            p.model = model;
            p.shaded = shaded ? 1 : 0;
            p.batch = static_cast<u8>(slot);
        }
    }
    if (stats) {
        stats->cellsDrawn += local.cellsDrawn;
        stats->cellsGrowing += local.cellsGrowing;
        stats->dropped += local.dropped;
    }
    return count;
}

} // namespace vd::grass
