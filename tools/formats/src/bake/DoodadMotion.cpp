#include "bake/DoodadMotion.h"

#include <glm/common.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace vd::bake {
namespace {

constexpr u32 kMotionSamples = 64; // poses over the loop (and the global sequences) compared per vertex

template <typename T> bool changes(const formats::M2Track<T>& track, u32 first, u32 last) {
    for (u32 k = first + 1; k <= last && k < track.values.size(); ++k) {
        if (track.values[k] != track.values[first]) {
            return true;
        }
    }
    return false;
}

// Whether a track's value changes over the loop sequence (its key range for that sequence), or at all when it
// runs on a global sequence.
template <typename T> bool animatedOverLoop(const formats::M2Track<T>& track, i32 sequence) {
    if (track.times.size() < 2) {
        return false;
    }
    const u32 last = static_cast<u32>(track.times.size()) - 1;
    if (track.globalSequence >= 0 || sequence < 0 || static_cast<usize>(sequence) >= track.ranges.size()) {
        return changes(track, 0, last);
    }
    const u32 first = std::min(track.ranges[static_cast<usize>(sequence)][0], last);
    return changes(track, first, std::clamp(track.ranges[static_cast<usize>(sequence)][1], first, last));
}

// Time a track is sampled at: on its global sequence, or into the loop sequence's span of the timeline.
template <typename T>
u32 trackTime(const formats::M2Model& model, const formats::M2Track<T>& track, i32 sequence, u32 loopMs,
              u32 globalMs) {
    if (track.globalSequence >= 0) {
        const usize g = static_cast<usize>(track.globalSequence);
        const u32 duration = g < model.globalSequences.size() ? model.globalSequences[g] : 0;
        return duration ? globalMs % duration : 0;
    }
    if (sequence < 0) {
        return 0;
    }
    const formats::M2Sequence& s = model.sequences[static_cast<usize>(sequence)];
    return s.start + std::min(loopMs, s.end - s.start);
}

glm::vec3 transform(const std::array<f32, 12>& m, const formats::Vec3& p) {
    return {m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3], m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
            m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]};
}

glm::vec3 skin(const formats::M2Vertex& v, const std::vector<std::array<f32, 12>>& pose) {
    glm::vec3 out(0.0f);
    u32 sum = 0;
    for (u32 k = 0; k < 4; ++k) {
        if (v.boneWeights[k] == 0 || v.boneIndices[k] >= pose.size()) {
            continue;
        }
        out += (static_cast<f32>(v.boneWeights[k]) / 255.0f) * transform(pose[v.boneIndices[k]], v.position);
        sum += v.boneWeights[k];
    }
    return sum == 0 ? glm::vec3(v.position[0], v.position[1], v.position[2])
                    : out; // unweighted: rigid, bind pose
}

// The bone a vertex mostly follows.
u8 dominantBone(const formats::M2Vertex& v) {
    u32 best = 0;
    for (u32 k = 1; k < 4; ++k) {
        best = v.boneWeights[k] > v.boneWeights[best] ? k : best;
    }
    return v.boneIndices[best];
}

void grow(glm::vec3& min, glm::vec3& max, const glm::vec3& p, f32 radius = 0.0f) {
    min = glm::min(min, p - glm::vec3(radius));
    max = glm::max(max, p + glm::vec3(radius));
}

} // namespace

i32 doodadLoopSequence(const formats::M2Model& model) {
    if (model.sequences.empty()) {
        return -1;
    }
    i32 stand = -1;
    for (usize s = 0; s < model.sequences.size(); ++s) {
        if (model.sequences[s].animationId == 0) {
            if (model.sequences[s].variation == 0) {
                return static_cast<i32>(s);
            }
            stand = stand < 0 ? static_cast<i32>(s) : stand;
        }
    }
    return stand >= 0 ? stand : 0;
}

std::vector<u32> doodadLoopVariations(const formats::M2Model& model) {
    std::vector<u32> out;
    const i32 loop = doodadLoopSequence(model);
    if (loop < 0) {
        return out;
    }
    out.push_back(static_cast<u32>(loop));
    const u16 animation = model.sequences[static_cast<usize>(loop)].animationId;
    for (usize s = 0; s < model.sequences.size(); ++s) {
        if (s != static_cast<usize>(loop) && model.sequences[s].animationId == animation) {
            out.push_back(static_cast<u32>(s));
        }
    }
    return out;
}

void doodadPose(const formats::M2Model& model, i32 sequence, u32 loopMs, u32 globalMs,
                std::vector<std::array<f32, 12>>& out) {
    out.resize(model.bones.size());
    std::vector<glm::mat4> world(model.bones.size(), glm::mat4(1.0f));
    const u32 seq = sequence < 0 ? 0u : static_cast<u32>(sequence);
    for (usize b = 0; b < model.bones.size(); ++b) {
        const formats::M2Bone& bone = model.bones[b];
        const formats::Vec3 t = formats::sampleM2Track(
            bone.translation, seq, trackTime(model, bone.translation, sequence, loopMs, globalMs),
            formats::Vec3{0.0f, 0.0f, 0.0f});
        const formats::Quat r = formats::sampleM2Track(
            bone.rotation, seq, trackTime(model, bone.rotation, sequence, loopMs, globalMs),
            formats::Quat{0.0f, 0.0f, 0.0f, 1.0f});
        const formats::Vec3 s =
            formats::sampleM2Track(bone.scale, seq, trackTime(model, bone.scale, sequence, loopMs, globalMs),
                                   formats::Vec3{1.0f, 1.0f, 1.0f});
        const glm::vec3 pivot(bone.pivot[0], bone.pivot[1], bone.pivot[2]);
        glm::mat4 local = glm::translate(glm::mat4(1.0f), pivot + glm::vec3(t[0], t[1], t[2]));
        local = local * glm::mat4_cast(glm::normalize(glm::quat(r[3], r[0], r[1], r[2])));
        local = glm::scale(local, glm::vec3(s[0], s[1], s[2]));
        local = glm::translate(local, -pivot);
        world[b] = bone.parent >= 0 && static_cast<usize>(bone.parent) < b
                       ? world[static_cast<usize>(bone.parent)] * local
                       : local;
        for (u32 row = 0; row < 3; ++row) {
            for (u32 column = 0; column < 4; ++column) {
                out[b][row * 4 + column] = world[b][column][row];
            }
        }
    }
}

std::vector<i32> doodadBillboardBones(const formats::M2Model& model) {
    std::vector<i32> out(model.bones.size(), -1);
    for (usize b = 0; b < model.bones.size(); ++b) { // parents come first
        const formats::M2Bone& bone = model.bones[b];
        if (bone.flags & kM2BoneBillboardMask) {
            out[b] = static_cast<i32>(b);
        } else if (bone.parent >= 0 && static_cast<usize>(bone.parent) < b) {
            out[b] = out[static_cast<usize>(bone.parent)];
        }
    }
    return out;
}

DoodadMotion analyzeDoodadMotion(const formats::M2Model& model) {
    DoodadMotion out;
    out.loopSequence = doodadLoopSequence(model);
    out.sequences = static_cast<u32>(model.sequences.size());
    out.bones = static_cast<u32>(model.bones.size());
    out.vertices = static_cast<u32>(model.vertices.size());
    if (out.loopSequence >= 0) {
        const formats::M2Sequence& s = model.sequences[static_cast<usize>(out.loopSequence)];
        out.loopMs = s.end > s.start ? s.end - s.start : 0;
    }
    const std::vector<u32> variations = doodadLoopVariations(model);
    out.variations = static_cast<u32>(variations.size());
    // Bones that move (their own tracks, or a parent's) and billboards (themselves or under one).
    std::vector<u8> moving(model.bones.size(), 0);
    const std::vector<i32> billboard = doodadBillboardBones(model);
    u32 longestGlobal = 0;
    for (usize b = 0; b < model.bones.size(); ++b) {
        const formats::M2Bone& bone = model.bones[b];
        out.boneFlags |= bone.flags;
        bool own = false, global = false, sequence = false;
        auto check = [&](const auto& track) {
            bool changes = variations.empty() && animatedOverLoop(track, -1);
            for (const u32 v : variations) {
                changes = changes || animatedOverLoop(track, static_cast<i32>(v));
            }
            if (!changes) {
                return;
            }
            own = true;
            if (track.globalSequence >= 0) {
                global = true;
                if (static_cast<usize>(track.globalSequence) < model.globalSequences.size()) {
                    longestGlobal = std::max(longestGlobal,
                                             model.globalSequences[static_cast<usize>(track.globalSequence)]);
                }
            } else {
                sequence = true;
            }
        };
        check(bone.translation);
        check(bone.rotation);
        check(bone.scale);
        out.animatedBones += own ? 1 : 0;
        out.globalSequenceBones += global ? 1 : 0;
        out.phased = out.phased || sequence;
        const bool hasParent = bone.parent >= 0 && static_cast<usize>(bone.parent) < b;
        moving[b] = own || (hasParent && moving[static_cast<usize>(bone.parent)]);
    }
    std::vector<u8> billboardUsed(model.bones.size(), 0);
    std::vector<u8> candidate(model.vertices.size(), 0);
    for (usize v = 0; v < model.vertices.size(); ++v) {
        const formats::M2Vertex& vertex = model.vertices[v];
        u32 weights = 0;
        for (u32 k = 0; k < 4; ++k) {
            const u8 bone = vertex.boneIndices[k];
            if (vertex.boneWeights[k] == 0 || bone >= model.bones.size()) {
                continue;
            }
            ++weights;
            candidate[v] |= moving[bone];
        }
        const u8 dominant = dominantBone(vertex);
        if (weights > 0 && dominant < model.bones.size() && billboard[dominant] >= 0) {
            candidate[v] |= 2;
            billboardUsed[static_cast<usize>(billboard[dominant])] = 1;
        }
        out.blendedVertices += weights > 1 ? 1 : 0;
        out.billboardVertices += (candidate[v] & 2) ? 1 : 0;
    }
    for (usize b = 0; b < model.bones.size(); ++b) {
        out.billboardBones += billboardUsed[b] ? 1 : 0;
    }
    for (const formats::M2ParticleEmitter& e : model.particles) {
        out.movingEffects += e.bone < moving.size() && moving[e.bone] ? 1 : 0;
    }
    for (const formats::M2RibbonEmitter& r : model.ribbons) {
        out.movingEffects += r.bone < moving.size() && moving[r.bone] ? 1 : 0;
    }

    // Vertex motion: poses over the loop (and the longest global sequence a moving bone runs on) against the
    // first, and the bounds of every pose.
    glm::vec3 min(FLT_MAX), max(-FLT_MAX);
    std::vector<std::array<f32, 12>> pose;
    auto addBounds = [&]() {
        for (usize v = 0; v < model.vertices.size(); ++v) {
            const formats::M2Vertex& vertex = model.vertices[v];
            const u8 dominant = dominantBone(vertex);
            if ((candidate[v] & 2) && dominant < billboard.size()) {
                // Turning about its billboard bone's pivot: the sphere it can reach.
                const formats::M2Bone& bone = model.bones[static_cast<usize>(billboard[dominant])];
                const std::array<f32, 12>& m = pose[static_cast<usize>(billboard[dominant])];
                const glm::vec3 pivot(bone.pivot[0], bone.pivot[1], bone.pivot[2]);
                const f32 scale = glm::length(glm::vec3(m[0], m[4], m[8]));
                const glm::vec3 offset =
                    glm::vec3(vertex.position[0], vertex.position[1], vertex.position[2]) - pivot;
                grow(min, max, transform(m, bone.pivot), glm::length(offset) * scale);
            } else {
                grow(min, max, skin(vertex, pose));
            }
        }
    };
    for (const formats::M2Vertex& vertex : model.vertices) {
        grow(min, max, glm::vec3(vertex.position[0], vertex.position[1], vertex.position[2]));
    }
    doodadPose(model, out.loopSequence, 0, 0, pose);
    addBounds();
    std::vector<glm::vec3> first(model.vertices.size());
    for (usize v = 0; v < model.vertices.size(); ++v) {
        first[v] = skin(model.vertices[v], pose);
        const glm::vec3 bind(model.vertices[v].position[0], model.vertices[v].position[1],
                             model.vertices[v].position[2]);
        out.restOffset = std::max(out.restOffset, glm::length(first[v] - bind));
    }
    std::vector<f32> motion(model.vertices.size(), 0.0f);
    for (usize variation = 0; variation < std::max<usize>(variations.size(), 1); ++variation) {
        const i32 sequence = variations.empty() ? -1 : static_cast<i32>(variations[variation]);
        u32 duration = 0;
        if (sequence >= 0) {
            const formats::M2Sequence& s = model.sequences[static_cast<usize>(sequence)];
            duration = s.end > s.start ? s.end - s.start : 0;
        }
        const u32 period = std::max(duration, longestGlobal);
        if (out.animatedBones == 0 || period == 0) {
            continue;
        }
        for (u32 i = 0; i < kMotionSamples; ++i) {
            const u32 t = static_cast<u32>(static_cast<u64>(period) * i / kMotionSamples);
            doodadPose(model, sequence, duration ? t % duration : 0, t, pose);
            addBounds();
            for (usize v = 0; v < model.vertices.size(); ++v) {
                if (candidate[v] & 1) {
                    motion[v] = std::max(motion[v], glm::length(skin(model.vertices[v], pose) - first[v]));
                }
            }
        }
    }
    for (const f32 m : motion) {
        out.maxMotion = std::max(out.maxMotion, m);
        out.movingVertices += m > kDoodadMotionThreshold ? 1 : 0;
    }
    if (min.x <= max.x) {
        out.boundsMin = {min.x, min.y, min.z};
        out.boundsMax = {max.x, max.y, max.z};
    }
    return out;
}

} // namespace vd::bake
