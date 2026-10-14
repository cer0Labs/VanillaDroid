#include "ui/UiModels.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "ui/DrawList.h"
#include "ui/UiAssets.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace vd::ui {
namespace {

using pack::UiModelId;

// WoW.exe 0x76D1A0: a Model frame's model is scaled by NDCToDDCHeight(1) / 0.6 (1 at any aspect: the interface's 768
// units are the screen's height) times its model scale and the frame's effective scale, in DDC units, of which the
// 768-unit-high interface has 1280 per unit (DDC height 0.6).
constexpr f32 kUnitsPerModelUnit = 1280.0f;
constexpr f32 kCooldownUnits = 36.0f;       // ActionButtonTemplate: the Cooldown model frame, 36 x 36
constexpr f32 kPingUnits = 50.0f;           // Minimap.xml: MiniMapPing, 50 x 50
constexpr f32 kPingModelScale = 0.4f;
constexpr u16 kAnimStand = 0;
constexpr u16 kAnimDeath = 1;
constexpr u16 kAnimBirth = 127;
constexpr u64 kFlashToleranceMs = 250; // a cooldown gone this close to its end ran out (else it was cut short)
constexpr u64 kForgetMs = 2000;        // a widget not drawn this long is forgotten
constexpr u64 kAutocastGapMs = 250;    // an autocast model not drawn this long starts over when it shows again
constexpr u32 kMaxPolygon = 16;
constexpr u32 kCircleSides = 32;                    // a round button's viewport for the model's sprites
constexpr u32 kMaxCirclePolygon = kCircleSides + 4; // a triangle cut by it
constexpr u32 kM2BlendAdd = 4;
constexpr u32 kM2BlendNoAlphaAdd = 3;
constexpr u32 kParticleInheritScale = 0x20;
constexpr u32 kParticleRandomSpinSign = 0x200;
constexpr f32 kInf = std::numeric_limits<f32>::infinity();

template <typename T> bool section(std::span<const u8> blob, const pack::UiModelArray& a, std::span<const T>& out) {
    if (u64{a.offset} + u64{a.count} * sizeof(T) > blob.size() || (a.count > 0 && a.offset % alignof(T) != 0)) {
        return false;
    }
    out = {reinterpret_cast<const T*>(blob.data() + a.offset), a.count};
    return true;
}

constexpr u32 params(u32 layer, UiMode mode, u32 extra = 0) {
    return (layer & 4095u) | (static_cast<u32>(mode) << 12) | ((extra & 255u) << 16);
}

void unpack(u32 rgba, f32* out) {
    for (u32 i = 0; i < 4; ++i) {
        out[i] = static_cast<f32>((rgba >> (8 * i)) & 255u) / 255.0f;
    }
}

u32 pack(const f32* c) {
    u32 out = 0;
    for (u32 i = 0; i < 4; ++i) {
        const f32 v = std::clamp(c[i], 0.0f, 1.0f);
        out |= static_cast<u32>(v * 255.0f + 0.5f) << (8 * i);
    }
    return out;
}

// 3 x 4 affine matrices, rows: x' = m[0..3] . (x, y, z, 1).
void identity(f32* m) {
    static constexpr f32 kIdentity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::memcpy(m, kIdentity, sizeof(kIdentity));
}

void multiply(const f32* a, const f32* b, f32* out) {
    f32 r[12];
    for (u32 row = 0; row < 3; ++row) {
        for (u32 col = 0; col < 4; ++col) {
            r[row * 4 + col] = a[row * 4] * b[col] + a[row * 4 + 1] * b[4 + col] + a[row * 4 + 2] * b[8 + col] +
                               (col == 3 ? a[row * 4 + 3] : 0.0f);
        }
    }
    std::memcpy(out, r, sizeof(r));
}

// Rotation of a unit quaternion (x, y, z, w), rows.
void rotation(const f32* q, f32* r) {
    const f32 x = q[0], y = q[1], z = q[2], w = q[3];
    r[0] = 1.0f - 2.0f * (y * y + z * z);
    r[1] = 2.0f * (x * y - z * w);
    r[2] = 2.0f * (x * z + y * w);
    r[3] = 2.0f * (x * y + z * w);
    r[4] = 1.0f - 2.0f * (x * x + z * z);
    r[5] = 2.0f * (y * z - x * w);
    r[6] = 2.0f * (x * z - y * w);
    r[7] = 2.0f * (y * z + x * w);
    r[8] = 1.0f - 2.0f * (x * x + y * y);
}

// The 1.12 particle's look over its life (as fx/ParticleMath.h lifeRamp): birth, mid, death.
f32 ramp(f32 t, f32 mid, f32 a, f32 b, f32 c) {
    if (t <= mid) {
        return mid > 0.0f ? a + (b - a) * (t / mid) : b;
    }
    return mid < 1.0f ? b + (c - b) * ((t - mid) / (1.0f - mid)) : b;
}

// A particle's own random numbers (stateless: the k-th particle of an emitter always gets the same).
f32 random01(u32 k, u32 salt) {
    u32 x = k * 0x9E3779B1u ^ salt * 0x85EBCA77u;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return static_cast<f32>(x >> 8) * (1.0f / 16777216.0f);
}

struct PolyVertex {
    f32 x, y, u;
};

// Keeps the part of a convex polygon where u >= limit (sign 1) or u <= limit (sign -1).
u32 clipPolygon(const PolyVertex* in, u32 count, f32 limit, f32 sign, PolyVertex* out) {
    u32 n = 0;
    for (u32 i = 0; i < count; ++i) {
        const PolyVertex& a = in[i];
        const PolyVertex& b = in[(i + 1) % count];
        const f32 da = (a.u - limit) * sign, db = (b.u - limit) * sign;
        if (da >= 0.0f && n < kMaxPolygon) {
            out[n++] = a;
        }
        if ((da >= 0.0f) != (db >= 0.0f) && n < kMaxPolygon) {
            const f32 t = da / (da - db);
            out[n++] = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.u + (b.u - a.u) * t};
        }
    }
    return n;
}

} // namespace

bool UiModels::ready() {
    if (m_loaded) {
        return true;
    }
    if (m_failed || !m_assets || !m_assets->isOpen()) {
        return false;
    }
    m_failed = true; // once: a pack without the models stays without them
    const std::span<const u8> blob = m_assets->blob(pack::kUiModelsAsset);
    if (blob.size() < sizeof(pack::UiModelsHeader)) {
        VD_LOG_WARN("UI models: ui.vdpk has no %s (re-bake it: vd_baker bake --only ui)", pack::kUiModelsAsset);
        return false;
    }
    std::memcpy(&m_header, blob.data(), sizeof(m_header));
    const pack::UiModelsHeader& h = m_header;
    if (h.version != pack::kUiModelsVersion || !section(blob, h.models, m_models) ||
        !section(blob, h.sequences, m_sequences) || !section(blob, h.bones, m_bones) ||
        !section(blob, h.vertices, m_vertices) || !section(blob, h.batches, m_batches) ||
        !section(blob, h.emitters, m_emitters) || !section(blob, h.tracks, m_tracks) ||
        !section(blob, h.ranges, m_ranges) || !section(blob, h.keys, m_keys) || !section(blob, h.texels, m_texels) ||
        !section(blob, h.globalSequences, m_globalSequences) ||
        m_models.size() != static_cast<usize>(UiModelId::Count)) {
        VD_LOG_WARN("UI models: %s unreadable (version %u, expected %u)", pack::kUiModelsAsset, h.version,
                    pack::kUiModelsVersion);
        return false;
    }
    for (const pack::UiModelInfo& m : m_models) {
        if (u64{m.firstSequence} + m.sequenceCount > m_sequences.size() ||
            u64{m.firstBone} + m.boneCount > m_bones.size() || u64{m.firstBatch} + m.batchCount > m_batches.size() ||
            u64{m.firstEmitter} + m.emitterCount > m_emitters.size() ||
            u64{m.firstGlobalSequence} + m.globalSequenceCount > m_globalSequences.size()) {
            VD_LOG_WARN("UI models: a model out of its tables");
            return false;
        }
    }
    for (const pack::UiModelTrack& t : m_tracks) {
        if (u64{t.firstKey} + t.keyCount > m_keys.size() || u64{t.firstRange} + t.rangeCount > m_ranges.size() ||
            t.keyCount == 0) {
            VD_LOG_WARN("UI models: a track out of its tables");
            return false;
        }
    }
    // The 1-D textures as pieces of linear color between the texel centers where the slope changes (the client's
    // bilinear magnification), clamped beyond the first and last texels, equal neighbours merged.
    m_segments.clear();
    m_batchSegments.assign(m_batches.size(), 0);
    for (usize b = 0; b < m_batches.size(); ++b) {
        const pack::UiModelBatch& batch = m_batches[b];
        if (u64{batch.firstVertex} + batch.vertexCount > m_vertices.size() ||
            u64{batch.firstTexel} + batch.texelCount > m_texels.size()) {
            VD_LOG_WARN("UI models: a batch out of its tables");
            return false;
        }
        if (batch.texture != 0 || batch.texelCount == 0) {
            continue;
        }
        const u32 n = batch.texelCount;
        const auto texel = [&](u32 i, f32* out) { unpack(m_texels[batch.firstTexel + i], out); };
        const auto center = [&](u32 i) { return (static_cast<f32>(i) + 0.5f) / static_cast<f32>(n); };
        const usize first = m_segments.size();
        Segment s{};
        s.u0 = -kInf;
        s.u1 = center(0);
        texel(0, s.c0);
        texel(0, s.c1);
        m_segments.push_back(s);
        for (u32 i = 0; i + 1 < n; ++i) {
            Segment piece{};
            piece.u0 = center(i);
            piece.u1 = center(i + 1);
            texel(i, piece.c0);
            texel(i + 1, piece.c1);
            m_segments.push_back(piece);
        }
        s.u0 = center(n - 1);
        s.u1 = kInf;
        texel(n - 1, s.c0);
        texel(n - 1, s.c1);
        m_segments.push_back(s);
        // Merge a piece into the previous one when both are constant and equal, or lie on the same line.
        usize kept = first;
        for (usize i = first + 1; i < m_segments.size(); ++i) {
            Segment& last = m_segments[kept];
            const Segment& next = m_segments[i];
            const bool constant = std::memcmp(last.c0, last.c1, sizeof(last.c0)) == 0 &&
                                  std::memcmp(next.c0, next.c1, sizeof(next.c0)) == 0 &&
                                  std::memcmp(last.c1, next.c0, sizeof(next.c0)) == 0;
            bool collinear = false;
            if (!constant && std::isfinite(last.u0) && std::isfinite(next.u1)) {
                collinear = true;
                for (u32 c = 0; c < 4; ++c) {
                    const f32 slopeA = (last.c1[c] - last.c0[c]) / (last.u1 - last.u0);
                    const f32 slopeB = (next.c1[c] - next.c0[c]) / (next.u1 - next.u0);
                    collinear = collinear && std::fabs(slopeA - slopeB) < 1e-4f;
                }
            }
            if (constant || collinear) {
                last.u1 = next.u1;
                std::memcpy(last.c1, next.c1, sizeof(last.c1));
            } else {
                m_segments[++kept] = next;
            }
        }
        m_segments.resize(kept + 1);
        m_batchSegments[b] = static_cast<u32>(first << 16 | (m_segments.size() - first));
    }
    m_loaded = true;
    m_failed = false;
    return true;
}

u32 UiModels::sequence(Id model, u16 animationId) const {
    if (!m_loaded) {
        return ~0u;
    }
    const pack::UiModelInfo& m = m_models[static_cast<u32>(model)];
    for (u32 i = 0; i < m.sequenceCount; ++i) {
        if (m_sequences[m.firstSequence + i].animationId == animationId) {
            return i;
        }
    }
    return ~0u;
}

f32 UiModels::sequenceMs(Id model, u32 sequence) const {
    if (!m_loaded || sequence >= m_models[static_cast<u32>(model)].sequenceCount) {
        return 0.0f;
    }
    const pack::UiModelSequence& s = m_sequences[m_models[static_cast<u32>(model)].firstSequence + sequence];
    return static_cast<f32>(s.end - s.start);
}

// A track's value at `time` on the timeline of the model's sequence `modelSequence` (formats::sampleM2Track's rule:
// the sequence's key range, clamped outside it; a global sequence loops all its keys).
void UiModels::sample(u32 track, u32 modelSequence, u32 time, u32 globalMs, const pack::UiModelInfo& model, f32* out,
                      u32 components, const f32* fallback) const {
    if (track == pack::kUiModelNoTrack || track >= m_tracks.size()) {
        std::memcpy(out, fallback, components * sizeof(f32));
        return;
    }
    const pack::UiModelTrack& t = m_tracks[track];
    const pack::UiModelKey* keys = m_keys.data() + t.firstKey;
    u32 first = 0, last = t.keyCount - 1u;
    if (t.globalSequence >= 0) {
        const u32 g = static_cast<u32>(t.globalSequence);
        const u32 duration = g < model.globalSequenceCount ? m_globalSequences[model.firstGlobalSequence + g] : 0;
        time = duration > 0 ? globalMs % duration : 0;
    } else if (modelSequence < t.rangeCount) {
        const pack::UiModelRange& r = m_ranges[t.firstRange + modelSequence];
        first = std::min<u32>(r.first, last);
        last = std::clamp<u32>(r.last, first, last);
    }
    if (time <= keys[first].time || first == last) {
        std::memcpy(out, keys[first].value, components * sizeof(f32));
        return;
    }
    if (time >= keys[last].time) {
        std::memcpy(out, keys[last].value, components * sizeof(f32));
        return;
    }
    u32 k = first;
    while (k + 1 < last && keys[k + 1].time <= time) {
        ++k;
    }
    const f32* a = keys[k].value;
    if (t.interpolation == 0) {
        std::memcpy(out, a, components * sizeof(f32));
        return;
    }
    f32 b[4];
    std::memcpy(b, keys[k + 1].value, sizeof(b));
    const u32 span = keys[k + 1].time - keys[k].time;
    const f32 f = span ? static_cast<f32>(time - keys[k].time) / static_cast<f32>(span) : 0.0f;
    if (components == 4) { // quaternions: the shorter way, normalized
        if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0.0f) {
            for (f32& c : b) {
                c = -c;
            }
        }
        f32 length = 0.0f;
        for (u32 c = 0; c < 4; ++c) {
            out[c] = a[c] + (b[c] - a[c]) * f;
            length += out[c] * out[c];
        }
        const f32 inv = length > 0.0f ? 1.0f / std::sqrt(length) : 1.0f;
        for (u32 c = 0; c < 4; ++c) {
            out[c] *= inv;
        }
        return;
    }
    for (u32 c = 0; c < components; ++c) {
        out[c] = a[c] + (b[c] - a[c]) * f;
    }
}

// One bone's matrix: parent * T(pivot + translation) * R * S * T(-pivot).
void UiModels::boneMatrix(const pack::UiModelInfo& model, u32 bone, u32 sequence, u32 time, u32 globalMs,
                          f32* m) const {
    static constexpr f32 kZero[3] = {0.0f, 0.0f, 0.0f};
    static constexpr f32 kOne[3] = {1.0f, 1.0f, 1.0f};
    static constexpr f32 kNoTurn[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const pack::UiModelBone& b = m_bones[model.firstBone + bone];
    f32 t[3], q[4], s[3];
    sample(b.translation, sequence, time, globalMs, model, t, 3, kZero);
    sample(b.rotation, sequence, time, globalMs, model, q, 4, kNoTurn);
    sample(b.scale, sequence, time, globalMs, model, s, 3, kOne);
    f32 r[9];
    rotation(q, r);
    for (u32 row = 0; row < 3; ++row) {
        for (u32 col = 0; col < 3; ++col) {
            m[row * 4 + col] = r[row * 3 + col] * s[col];
        }
        m[row * 4 + 3] = b.pivot[row] + t[row] -
                         (m[row * 4] * b.pivot[0] + m[row * 4 + 1] * b.pivot[1] + m[row * 4 + 2] * b.pivot[2]);
    }
    if (b.parent >= 0 && static_cast<u32>(b.parent) < bone) {
        f32 parent[12];
        boneMatrix(model, static_cast<u32>(b.parent), sequence, time, globalMs, parent);
        multiply(parent, m, m);
    }
}

void UiModels::drawModel(DrawList& draw, Id id, u32 sequence, f32 timeMs, f32 globalMs, const Placement& at) const {
    if (!m_loaded) {
        return;
    }
    const pack::UiModelInfo& model = m_models[static_cast<u32>(id)];
    if (sequence >= model.sequenceCount || model.boneCount > 16) {
        return;
    }
    const pack::UiModelSequence& seq = m_sequences[model.firstSequence + sequence];
    const u32 time = seq.start + static_cast<u32>(std::clamp(timeMs, 0.0f, static_cast<f32>(seq.end - seq.start)));
    const u32 global = static_cast<u32>(std::max(globalMs, 0.0f));
    f32 bones[16][12];
    for (u32 i = 0; i < model.boneCount; ++i) {
        boneMatrix(model, i, sequence, time, global, bones[i]);
    }
    draw.pushClip({at.frameX, at.frameY, at.frameW, at.frameH}); // the frame's viewport
    for (u32 i = 0; i < model.batchCount; ++i) {
        drawBatch(draw, model, model.firstBatch + i, sequence, time, global, at, bones);
    }
    draw.popClip();
}

void UiModels::drawBatch(DrawList& draw, const pack::UiModelInfo& model, u32 batchIndex, u32 sequence, u32 time,
                         u32 globalMs, const Placement& at, const f32 (*bones)[12]) const {
    static constexpr f32 kOne[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    static constexpr f32 kZero[3] = {0.0f, 0.0f, 0.0f};
    static constexpr f32 kNoTurn[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const pack::UiModelBatch& b = m_batches[batchIndex];
    f32 color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    sample(b.color, sequence, time, globalMs, model, color, 3, kOne);
    f32 alpha = 1.0f, weight = 1.0f;
    sample(b.alpha, sequence, time, globalMs, model, &alpha, 1, kOne);
    sample(b.weight, sequence, time, globalMs, model, &weight, 1, kOne);
    color[3] = alpha * weight * at.alpha;
    if (color[3] <= 0.002f) {
        return;
    }
    const bool additive = b.blend == kM2BlendAdd || b.blend == kM2BlendNoAlphaAdd;
    // Model space -> logical pixels (y down).
    const auto place = [&](const pack::UiModelVertex& v, f32& x, f32& y) {
        const u32 bone = v.bone < model.boneCount ? v.bone : 0u;
        const f32* m = bones[bone];
        const f32 mx = m[0] * v.position[0] + m[1] * v.position[1] + m[2] * v.position[2] + m[3];
        const f32 my = m[4] * v.position[0] + m[5] * v.position[1] + m[6] * v.position[2] + m[7];
        x = at.x + mx * at.scale;
        y = at.y - my * at.scale;
    };

    if (b.texture != 0) {
        // An atlas sprite: each triangle a quad with its last corner twice.
        const std::optional<Sprite> sprite = draw.assets()->sprite(b.texture);
        if (!sprite) {
            return;
        }
        const f32 inv = 1.0f / draw.assets()->atlasSize();
        const u32 p = params(sprite->page, UiMode::Atlas) | (additive ? 1u << 24 : 0u);
        const u32 packed = pack(color);
        const f32 cx = at.frameX + at.frameW * 0.5f, cy = at.frameY + at.frameH * 0.5f;
        for (u32 i = 0; i + 2 < b.vertexCount; i += 3) {
            UiVertex q[4];
            for (u32 k = 0; k < 3; ++k) {
                const pack::UiModelVertex& v = m_vertices[b.firstVertex + i + k];
                place(v, q[k].x, q[k].y);
                q[k].u = (sprite->x + v.uv[0] * sprite->width) * inv;
                q[k].v = (sprite->y + v.uv[1] * sprite->height) * inv;
                q[k].color = packed;
                q[k].params = p;
            }
            if (at.round <= 0.0f) {
                q[3] = q[2];
                draw.customQuad(q);
                continue;
            }
            // The round button's viewport: the triangle cut by the circle (a polygon of kCircleSides around it).
            UiVertex a[kMaxCirclePolygon], c[kMaxCirclePolygon];
            u32 n = 3;
            std::copy(q, q + 3, a);
            for (u32 side = 0; side < kCircleSides && n >= 3; ++side) {
                const f32 angle = (static_cast<f32>(side) + 0.5f) * (6.2831853f / static_cast<f32>(kCircleSides));
                const f32 nx = std::cos(angle), ny = std::sin(angle);
                u32 m = 0;
                for (u32 k = 0; k < n; ++k) {
                    const UiVertex& p0 = a[k];
                    const UiVertex& p1 = a[(k + 1) % n];
                    const f32 d0 = at.round - ((p0.x - cx) * nx + (p0.y - cy) * ny);
                    const f32 d1 = at.round - ((p1.x - cx) * nx + (p1.y - cy) * ny);
                    if (d0 >= 0.0f && m < kMaxCirclePolygon) {
                        c[m++] = p0;
                    }
                    if ((d0 >= 0.0f) != (d1 >= 0.0f) && m < kMaxCirclePolygon) {
                        const f32 t = d0 / (d0 - d1);
                        c[m] = p0;
                        c[m].x = p0.x + (p1.x - p0.x) * t;
                        c[m].y = p0.y + (p1.y - p0.y) * t;
                        c[m].u = p0.u + (p1.u - p0.u) * t;
                        c[m].v = p0.v + (p1.v - p0.v) * t;
                        ++m;
                    }
                }
                std::copy(c, c + m, a);
                n = m;
            }
            for (u32 k = 1; k + 1 < n; k += 2) {
                const UiVertex quad[4] = {a[0], a[k], a[k + 1], a[k + 2 < n ? k + 2 : k + 1]};
                draw.customQuad(quad);
            }
        }
        return;
    }

    // A 1-D texture moved by its texture coordinate animation: uv' = C + R (uv + T - C), C = (0.5, 0.5) (the
    // translation before the turn about the texture's center: the only order in which UI-Cooldown-Indicator's
    // keys sweep each quarter clean), then clamped as the client's sampler does (u: the texture's ends; v: the rows
    // are all the same). Each triangle is cut where the texture's color stops being linear; the pieces get the
    // texture's color in their corners (exact for a magnified bilinear texture).
    const u32 segmentInfo = m_batchSegments[batchIndex];
    const u32 firstSegment = segmentInfo >> 16, segmentCount = segmentInfo & 0xFFFFu;
    if (segmentCount == 0) {
        return;
    }
    f32 t[3], q[4];
    sample(b.uvTranslation, sequence, time, globalMs, model, t, 3, kZero);
    sample(b.uvRotation, sequence, time, globalMs, model, q, 4, kNoTurn);
    f32 r[9];
    rotation(q, r);
    const bool round = at.round > 0.0f;
    const f32 cx = at.frameX + at.frameW * 0.5f, cy = at.frameY + at.frameH * 0.5f;
    const f32 invRound = round ? 1.0f / (2.0f * at.round) : 0.0f;
    const u32 p = round ? params(0, UiMode::Ring, 255u) : params(0, UiMode::Solid);
    for (u32 i = 0; i + 2 < b.vertexCount; i += 3) {
        PolyVertex tri[3];
        for (u32 k = 0; k < 3; ++k) {
            const pack::UiModelVertex& v = m_vertices[b.firstVertex + i + k];
            place(v, tri[k].x, tri[k].y);
            const f32 du = v.uv[0] + t[0] - 0.5f, dv = v.uv[1] + t[1] - 0.5f;
            tri[k].u = 0.5f + r[0] * du + r[1] * dv;
        }
        for (u32 s = 0; s < segmentCount; ++s) {
            const Segment& seg = m_segments[firstSegment + s];
            if (seg.c0[3] <= 0.0f && seg.c1[3] <= 0.0f) {
                continue; // nothing to see in it
            }
            PolyVertex a[kMaxPolygon], c[kMaxPolygon];
            u32 n = 3;
            std::memcpy(a, tri, sizeof(tri));
            if (std::isfinite(seg.u0)) {
                n = clipPolygon(a, n, seg.u0, 1.0f, c);
                std::memcpy(a, c, n * sizeof(PolyVertex));
            }
            if (n >= 3 && std::isfinite(seg.u1)) {
                n = clipPolygon(a, n, seg.u1, -1.0f, c);
                std::memcpy(a, c, n * sizeof(PolyVertex));
            }
            if (n < 3) {
                continue;
            }
            const bool finite = std::isfinite(seg.u0) && std::isfinite(seg.u1);
            UiVertex poly[kMaxPolygon];
            for (u32 k = 0; k < n; ++k) {
                const f32 f = finite ? std::clamp((a[k].u - seg.u0) / (seg.u1 - seg.u0), 0.0f, 1.0f) : 0.0f;
                f32 rgba[4];
                for (u32 ch = 0; ch < 4; ++ch) {
                    rgba[ch] = (seg.c0[ch] + (seg.c1[ch] - seg.c0[ch]) * f) * color[ch];
                }
                poly[k].x = a[k].x;
                poly[k].y = a[k].y;
                poly[k].u = round ? (a[k].x - (cx - at.round)) * invRound : 0.0f;
                poly[k].v = round ? (a[k].y - (cy - at.round)) * invRound : 0.0f;
                poly[k].color = pack(rgba);
                poly[k].params = p;
            }
            // A fan of quads: (0, k, k + 1, k + 2) are the triangles (0, k, k + 1) and (0, k + 1, k + 2).
            for (u32 k = 1; k + 1 < n; k += 2) {
                const UiVertex quad[4] = {poly[0], poly[k], poly[k + 1], poly[k + 2 < n ? k + 2 : k + 1]};
                draw.customQuad(quad);
            }
        }
    }
}

void UiModels::drawEmitter(DrawList& draw, const pack::UiModelInfo& model, u32 emitterIndex, f32 modelMs,
                           const Placement& at) const {
    const pack::UiModelEmitter& e = m_emitters[emitterIndex];
    if (e.rate <= 0.0f || e.lifespan <= 0.0f || model.sequenceCount == 0) {
        return;
    }
    const std::optional<Sprite> sprite = draw.assets()->sprite(e.texture);
    if (!sprite) {
        return;
    }
    const pack::UiModelSequence& seq = m_sequences[model.firstSequence];
    const f32 loopMs = static_cast<f32>(std::max<u32>(seq.end - seq.start, 1));
    // Particle k is born at k / rate seconds after the model showed; the ones alive now are drawn (oldest first,
    // as the client's ring keeps them), every `stride`-th one when a limit thins them out.
    const f32 now = modelMs * 0.001f;
    const i64 newest = static_cast<i64>(std::floor(now * e.rate));
    const i64 oldest = std::max<i64>(0, static_cast<i64>(std::ceil((now - e.lifespan) * e.rate)));
    const f32 alive = e.rate * e.lifespan;
    const i64 stride = m_particleLimit > 0 && alive > static_cast<f32>(m_particleLimit)
                           ? static_cast<i64>(std::ceil(alive / static_cast<f32>(m_particleLimit)))
                           : 1;
    f32 colors[3][4];
    for (u32 i = 0; i < 3; ++i) {
        unpack(e.colors[i], colors[i]);
    }
    const f32 inv = 1.0f / draw.assets()->atlasSize();
    const f32 cellU = 1.0f / static_cast<f32>(e.columns), cellV = 1.0f / static_cast<f32>(e.rows);
    const f32 u0 = sprite->x * inv, v0 = sprite->y * inv;
    const f32 du = sprite->width * cellU * inv, dv = sprite->height * cellV * inv;
    const bool additive = e.blend == kM2BlendAdd || e.blend == kM2BlendNoAlphaAdd;
    const u32 p = params(sprite->page, UiMode::Atlas) | (additive ? 1u << 24 : 0u);
    const bool twinkles = e.twinklePercent < 1.0f || e.twinkleMin != e.twinkleMax;
    // Particles live in the frame's world (not the model's space): their size and motion are not scaled by the
    // model's scale (unless the emitter inherits it).
    const f32 worldScale = at.worldScale;
    const f32 sizeScale = (e.flags & kParticleInheritScale) ? at.scale : worldScale;
    const u32 bone = e.bone < model.boneCount ? e.bone : 0u;
    const pack::UiModelBone& boneInfo = m_bones[model.firstBone + bone];
    const bool moveOnly = boneInfo.parent < 0 && boneInfo.rotation == pack::kUiModelNoTrack &&
                          boneInfo.scale == pack::kUiModelNoTrack;
    for (i64 k = (oldest + stride - 1) / stride * stride; k <= newest; k += stride) {
        const f32 born = static_cast<f32>(k) / e.rate;
        const f32 age = now - born;
        if (age < 0.0f || age >= e.lifespan) {
            continue;
        }
        const f32 t = age / e.lifespan;
        f32 size = ramp(t, e.midPoint, e.scales[0], e.scales[1], e.scales[2]);
        const u32 key = static_cast<u32>(k);
        if (twinkles) {
            const f32 r = random01(static_cast<u32>(age * 10.0f) + key, emitterIndex + 77u);
            if (e.twinklePercent < r) {
                continue;
            }
            size *= e.twinkleMin + (e.twinkleMax - e.twinkleMin) * r;
        }
        if (size <= 0.0f) {
            continue;
        }
        f32 c[4];
        for (u32 ch = 0; ch < 4; ++ch) {
            c[ch] = ramp(t, e.midPoint, colors[0][ch], colors[1][ch], colors[2][ch]);
        }
        c[3] *= at.alpha;
        if (c[3] <= 0.002f) {
            continue;
        }
        // Where the emitter was when it was born (its bone at that moment of the looping sequence; a bone that only
        // moves, as the autocast's four, without the whole matrix).
        const u32 time = seq.start + static_cast<u32>(std::fmod(born * 1000.0f, loopMs));
        f32 m[12];
        if (moveOnly) {
            static constexpr f32 kZero[3] = {0.0f, 0.0f, 0.0f};
            identity(m);
            f32 moved[3];
            sample(boneInfo.translation, 0, time, static_cast<u32>(born * 1000.0f), model, moved, 3, kZero);
            m[3] = moved[0];
            m[7] = moved[1];
            m[11] = moved[2];
        } else {
            boneMatrix(model, bone, 0, time, static_cast<u32>(born * 1000.0f), m);
        }
        const f32 mx = m[0] * e.position[0] + m[1] * e.position[1] + m[2] * e.position[2] + m[3];
        const f32 my = m[4] * e.position[0] + m[5] * e.position[1] + m[6] * e.position[2] + m[7];
        // Its flight: the emitter's direction (plane and spline: polar and azimuth within their ranges, turned with
        // the bone), seen from the front (Z is depth here: straight up the emitter's Z, as the autocast's, it stays).
        f32 x = at.x + mx * at.scale, y = at.y - my * at.scale;
        if (e.speed != 0.0f && (e.verticalRange != 0.0f || m[2] != 0.0f || m[6] != 0.0f)) {
            const f32 polar = e.verticalRange * (2.0f * random01(key, emitterIndex) - 1.0f);
            const f32 azimuth = e.horizontalRange * (2.0f * random01(key, emitterIndex + 31u) - 1.0f);
            const f32 speed =
                e.speed * (1.0f + e.speedVariation * (2.0f * random01(key, emitterIndex + 57u) - 1.0f));
            const f32 dir[3] = {std::cos(azimuth) * std::sin(polar), std::sin(azimuth) * std::sin(polar),
                                std::cos(polar)};
            x += (m[0] * dir[0] + m[1] * dir[1] + m[2] * dir[2]) * speed * age * worldScale;
            y -= (m[4] * dir[0] + m[5] * dir[1] + m[6] * dir[2]) * speed * age * worldScale;
        }
        const f32 half = size * sizeScale;
        f32 theta = e.spin * age;
        if ((e.flags & kParticleRandomSpinSign) && (key & 1u)) {
            theta = -theta;
        }
        // The autocast's turn stays under 0.1 rad: its series (error under 1e-6 below 0.5 rad).
        const f32 t2 = theta * theta;
        const bool small = t2 < 0.25f;
        const f32 cs = (small ? 1.0f - t2 * (0.5f - t2 * (1.0f / 24.0f)) : std::cos(theta)) * half;
        const f32 sn = (small ? theta * (1.0f - t2 * (1.0f / 6.0f - t2 * (1.0f / 120.0f))) : std::sin(theta)) * half;
        const u32 packed = pack(c);
        UiVertex quad[4];
        const f32 ox[4] = {-1.0f, 1.0f, 1.0f, -1.0f}, oy[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
        for (u32 i = 0; i < 4; ++i) {
            quad[i].x = x + ox[i] * cs - oy[i] * sn;
            quad[i].y = y + ox[i] * sn + oy[i] * cs;
            quad[i].u = u0 + (ox[i] > 0.0f ? du : 0.0f);
            quad[i].v = v0 + (oy[i] > 0.0f ? dv : 0.0f);
            quad[i].color = packed;
            quad[i].params = p;
        }
        draw.customQuad(quad);
    }
}

UiModels::Instance& UiModels::instance(u64 id, u64 nowMs) {
    Instance* free = nullptr;
    Instance* oldest = &m_instances[0];
    for (Instance& i : m_instances) {
        if (i.id == id && i.seenMs + kForgetMs > nowMs) {
            return i;
        }
        if (!free && (i.id == 0 || i.seenMs + kForgetMs <= nowMs)) {
            free = &i;
        }
        if (i.seenMs < oldest->seenMs) {
            oldest = &i;
        }
    }
    Instance& out = free ? *free : *oldest;
    out = {};
    out.id = id;
    out.seenMs = nowMs;
    out.sinceMs = nowMs;
    return out;
}

void UiModels::cooldown(DrawList& draw, u64 id, const Rect& frame, f32 unit, f32 remaining, f32 leftSeconds,
                        u64 nowMs, f32 modelScale) {
    cooldownAt(draw, id, frame, unit, modelScale, 0.0f, remaining, leftSeconds, nowMs);
}

void UiModels::cooldownRound(DrawList& draw, u64 id, f32 cx, f32 cy, f32 radius, f32 remaining, f32 leftSeconds,
                             u64 nowMs) {
    // The model's square (36 interface units at its 0.75 scale) over the circle's bounding square, masked by it.
    const Rect frame{cx - radius, cy - radius, 2.0f * radius, 2.0f * radius};
    cooldownAt(draw, id, frame, 2.0f * radius / kCooldownUnits, 0.75f, radius, remaining, leftSeconds, nowMs);
}

void UiModels::cooldownAt(DrawList& draw, u64 id, const Rect& frame, f32 unit, f32 modelScale, f32 round,
                          f32 remaining, f32 leftSeconds, u64 nowMs) {
    VD_PROFILE_ZONE("UiModels::cooldown");
    if (!ready()) {
        return;
    }
    Placement at;
    at.x = frame.x;
    at.y = frame.y + frame.h;
    at.scale = kUnitsPerModelUnit * modelScale * unit;
    at.frameX = frame.x;
    at.frameY = frame.y;
    at.frameW = frame.w;
    at.frameH = frame.h;
    at.round = round;
    const bool idle = remaining <= 0.0f && m_instances.end() == std::find_if(m_instances.begin(), m_instances.end(),
                                                                                [&](const Instance& i) {
                                                                                    return i.id == id && i.id != 0;
                                                                                });
    if (idle) {
        return; // no cooldown and none ending: nothing to remember
    }
    Instance& state = instance(id, nowMs);
    const u64 lastSeen = state.seenMs;
    state.seenMs = nowMs;
    if (remaining > 0.0f) {
        remaining = std::min(remaining, 1.0f);
        // A new cooldown (none before, or more left than a moment ago): its start.
        if (state.lastShare <= 0.0f || remaining > state.lastShare + 1e-4f || nowMs - lastSeen > kForgetMs / 2) {
            state.firstMs = nowMs;
            state.firstShare = remaining;
            state.endMs = 0;
        }
        if (leftSeconds >= 0.0f) {
            state.endMs = nowMs + static_cast<u64>(leftSeconds * 1000.0f + 0.5f);
        } else if (nowMs > state.firstMs && state.firstShare > remaining) {
            const f32 perMs = (state.firstShare - remaining) / static_cast<f32>(nowMs - state.firstMs);
            state.endMs = nowMs + static_cast<u64>(remaining / perMs);
        }
        state.lastShare = remaining;
        state.flashMs = 0;
        const u32 stand = sequence(Id::Cooldown, kAnimStand);
        drawModel(draw, Id::Cooldown, stand, (1.0f - remaining) * 1000.0f, 0.0f, at);
        return;
    }
    // No cooldown now: one that was running ran out (its Death: CooldownFrame_OnUpdateModel's "stopping"), or was
    // cut short (CooldownFrame_SetTimer with start 0: Hide).
    if (state.lastShare > 0.0f) {
        state.lastShare = 0.0f;
        state.flashMs =
            state.endMs != 0 && nowMs + kFlashToleranceMs >= state.endMs ? std::min(nowMs, state.endMs) : 0;
    }
    const u32 death = sequence(Id::Cooldown, kAnimDeath);
    if (state.flashMs != 0 && death != ~0u) {
        const f32 age = static_cast<f32>(nowMs - state.flashMs);
        if (age < sequenceMs(Id::Cooldown, death)) {
            drawModel(draw, Id::Cooldown, death, age, 0.0f, at); // OnAnimFinished hides it after
            return;
        }
    }
    state = {}; // hidden: the widget keeps nothing here until its next cooldown
}

void UiModels::autocast(DrawList& draw, u64 id, const Rect& frame, f32 unit, f32 modelScale, u64 nowMs) {
    VD_PROFILE_ZONE("UiModels::autocast");
    if (!ready()) {
        return;
    }
    Instance& state = instance(id, nowMs);
    if (state.seenMs + kAutocastGapMs < nowMs) {
        state.sinceMs = nowMs; // hidden a while: it starts over
    }
    state.seenMs = nowMs;
    const pack::UiModelInfo& model = m_models[static_cast<u32>(Id::AutoCast)];
    Placement at;
    at.x = frame.x;
    at.y = frame.y + frame.h;
    at.scale = kUnitsPerModelUnit * modelScale * unit;
    at.worldScale = kUnitsPerModelUnit * unit;
    at.frameX = frame.x;
    at.frameY = frame.y;
    at.frameW = frame.w;
    at.frameH = frame.h;
    const f32 modelMs = static_cast<f32>(nowMs - state.sinceMs);
    draw.pushClip(frame);
    for (u32 i = 0; i < model.emitterCount; ++i) {
        drawEmitter(draw, model, model.firstEmitter + i, modelMs, at);
    }
    draw.popClip();
}

void UiModels::minimapPing(DrawList& draw, f32 cx, f32 cy, f32 unit, f32 ageMs, f32 alpha) {
    VD_PROFILE_ZONE("UiModels::minimapPing");
    if (!ready() || ageMs < 0.0f || alpha <= 0.0f) {
        return;
    }
    const f32 half = kPingUnits * 0.5f * unit;
    Placement at;
    at.x = cx - half;
    at.y = cy + half;
    at.scale = kUnitsPerModelUnit * kPingModelScale * unit;
    at.frameX = cx - half;
    at.frameY = cy - half;
    at.frameW = at.frameH = 2.0f * half;
    at.alpha = alpha;
    // Minimap_OnLoad's SetSequence(0) (Stand) on a model that starts with its Birth: the arrows close in, then the
    // Stand loops (the ring's ripple).
    const u32 birth = sequence(Id::MinimapPing, kAnimBirth);
    const u32 stand = sequence(Id::MinimapPing, kAnimStand);
    const f32 birthMs = birth != ~0u ? sequenceMs(Id::MinimapPing, birth) : 0.0f;
    if (birth != ~0u && ageMs < birthMs) {
        drawModel(draw, Id::MinimapPing, birth, ageMs, ageMs, at);
    } else if (stand != ~0u) {
        const f32 loop = std::max(sequenceMs(Id::MinimapPing, stand), 1.0f);
        drawModel(draw, Id::MinimapPing, stand, std::fmod(ageMs - birthMs, loop), ageMs, at);
    }
}

} // namespace vd::ui
