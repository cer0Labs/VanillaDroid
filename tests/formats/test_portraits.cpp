#include "bake/ActorBaker.h"
#include "formats/M2.h"
#include "pack/ActorFormat.h"

#include "SharedMpq.h"

#include <doctest/doctest.h>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>

// Portraits (unit frames): the M2 cameras of 1.12 (M2Camera before 2.0, 124 bytes) and the framing baked into
// actors.vdpk (pack::ActorPortrait), checked against the client's own models.

namespace {

using vd::f32;
using vd::u32;
using vd::u8;

vd::Result<vd::formats::M2Model> readM2(const vd::formats::MpqArchiveSet& mpq, const char* path) {
    vd::Result<std::vector<u8>> bytes = mpq.read(path);
    if (!bytes) {
        return bytes.error();
    }
    return vd::formats::parseM2(bytes.value());
}

glm::vec3 vec(const f32 v[3]) {
    return glm::vec3(v[0], v[1], v[2]);
}

// The bounds of the vertices (what the actor baker gives actorPortrait()).
void vertexBounds(const vd::formats::M2Model& m, f32 min[3], f32 max[3]) {
    for (u32 i = 0; i < 3; ++i) {
        min[i] = FLT_MAX;
        max[i] = -FLT_MAX;
    }
    for (const vd::formats::M2Vertex& v : m.vertices) {
        for (u32 i = 0; i < 3; ++i) {
            min[i] = std::min(min[i], v.position[i]);
            max[i] = std::max(max[i], v.position[i]);
        }
    }
}

// Half the side of the square a portrait sees around its target (the camera's diagonal field of view over a
// square).
f32 visibleHalf(const vd::pack::ActorPortrait& p) {
    return glm::length(vec(p.position) - vec(p.target)) * std::tan(p.fov * 0.5f) / std::sqrt(2.0f);
}

} // namespace

TEST_CASE("M2 cameras: the portrait camera of race and creature models (1.12 layout)") {
    VD_REQUIRE_MPQ(mpq);
    struct Expect {
        const char* path;
        f32 fov;
        f32 position[3]; // base vectors as authored (the tracks hold one zero key)
        f32 target[3];
    };
    // Values read from the files; the checks below are about what they mean (in front of the face,
    // head-sized).
    const Expect expected[] = {
        {"Character\\Human\\Male\\HumanMale.m2", 0.785f, {0.633f, -0.388f, 1.887f}, {0.063f, 0.034f, 1.864f}},
        {"Character\\Orc\\Female\\OrcFemale.m2",
         0.829f,
         {0.465f, -0.394f, 1.817f},
         {-0.041f, 0.031f, 1.822f}},
        {"Creature\\Wolf\\Wolf.m2", 0.950f, {1.693f, 0.724f, 0.884f}, {1.120f, -0.159f, 0.877f}},
        {"Creature\\Kobold\\Kobold.m2", 1.143f, {0.956f, 0.624f, 0.922f}, {0.472f, -0.188f, 0.761f}},
        {"Creature\\Dragon\\DragonNefarian.m2", 0.979f, {10.872f, 3.409f, 7.990f}, {8.426f, 0.145f, 6.758f}},
    };
    for (const Expect& e : expected) {
        const std::string name = e.path;
        CAPTURE(name);
        vd::Result<vd::formats::M2Model> parsed = readM2(*mpq, e.path);
        REQUIRE(parsed.ok());
        const vd::formats::M2Model& m = parsed.value();
        const vd::formats::M2Camera* camera = m.camera(vd::formats::kM2CameraPortrait);
        REQUIRE(camera != nullptr);
        CHECK(camera->type == vd::formats::kM2CameraPortrait);
        CHECK(camera->fov == doctest::Approx(e.fov).epsilon(0.002));
        CHECK(camera->nearClip == doctest::Approx(0.2222f).epsilon(0.01)); // 8 inches
        CHECK(camera->farClip == doctest::Approx(27.78f).epsilon(0.01));   // 1000 / 36 yards
        for (u32 i = 0; i < 3; ++i) {
            CHECK(camera->positionBase[i] == doctest::Approx(e.position[i]).epsilon(0.002));
            CHECK(camera->targetBase[i] == doctest::Approx(e.target[i]).epsilon(0.002));
        }
        // The tracks only hold the authored zero offset.
        REQUIRE(camera->position.values.size() == 1);
        CHECK(glm::length(glm::vec3(camera->position.values[0][0], camera->position.values[0][1],
                                    camera->position.values[0][2])) < 1e-4f);
        // Through the lookup table (indexed by camera type).
        REQUIRE(!m.cameraLookup.empty());
        CHECK(m.cameraLookup[0] >= 0);

        // Baked: in front of the model (it faces +X), looking back at a point high on it, close enough to
        // show a head, not the whole body.
        f32 min[3], max[3];
        vertexBounds(m, min, max);
        const vd::pack::ActorPortrait p = vd::bake::actorPortrait(m, min, max);
        CHECK((p.flags & vd::pack::kActorPortraitComputed) == 0);
        CHECK(p.position[0] > p.target[0]);
        const f32 height = max[2] - min[2];
        CHECK(p.target[2] > min[2] + height * 0.25f);
        CHECK(visibleHalf(p) < height * 0.45f);
        CHECK(p.fov == doctest::Approx(e.fov).epsilon(0.002));
        CHECK(p.roll == doctest::Approx(0.0f));
    }
}

TEST_CASE("M2 cameras: the paper doll camera and flybys parse too") {
    VD_REQUIRE_MPQ(mpq);
    vd::Result<vd::formats::M2Model> parsed = readM2(*mpq, "Character\\Human\\Male\\HumanMale.m2");
    REQUIRE(parsed.ok());
    const vd::formats::M2Model& m = parsed.value();
    // Race models carry the portrait camera; every camera read has a sane lens.
    for (const vd::formats::M2Camera& c : m.cameras) {
        CHECK(c.fov > 0.1f);
        CHECK(c.fov < 2.0f);
        CHECK(c.farClip > c.nearClip);
    }
    // The head key bone (6) sits under the helm attachment (11): what the computed framing uses.
    const vd::formats::M2Bone* head = nullptr;
    for (const vd::formats::M2Bone& b : m.bones) {
        if (b.keyBoneId == 6) {
            head = &b;
        }
    }
    REQUIRE(head != nullptr);
    CHECK(head->pivot[2] > 1.5f);
    CHECK(head->pivot[2] < 2.1f);
}

TEST_CASE("Baked portrait: models without a camera are framed on their head or top") {
    VD_REQUIRE_MPQ(mpq);
    // The computed framing, tried on models that do have a camera (their own camera removed): it must look at
    // about the same place and show about as much as the artists' camera.
    for (const char* path :
         {"Character\\Human\\Male\\HumanMale.m2", "Character\\Gnome\\Female\\GnomeFemale.m2",
          "Character\\Tauren\\Male\\TaurenMale.m2", "Creature\\Kobold\\Kobold.m2",
          "Creature\\Wolf\\Wolf.m2"}) {
        const std::string name = path;
        CAPTURE(name);
        vd::Result<vd::formats::M2Model> parsed = readM2(*mpq, path);
        REQUIRE(parsed.ok());
        vd::formats::M2Model m = parsed.value();
        f32 min[3], max[3];
        vertexBounds(m, min, max);
        const vd::pack::ActorPortrait authored = vd::bake::actorPortrait(m, min, max);
        m.cameras.clear();
        m.cameraLookup.clear();
        const vd::pack::ActorPortrait computed = vd::bake::actorPortrait(m, min, max);
        CHECK((computed.flags & vd::pack::kActorPortraitComputed) != 0);
        const f32 height = max[2] - min[2];
        const f32 off = glm::length(vec(computed.target) - vec(authored.target));
        MESSAGE(name << ": target off by " << off << " yd (height " << height << "), shows "
                     << visibleHalf(computed) << " against " << visibleHalf(authored));
        CHECK(off < height * 0.25f);
        CHECK(visibleHalf(computed) < visibleHalf(authored) * 2.5f);
        CHECK(visibleHalf(computed) > visibleHalf(authored) * 0.4f);
        CHECK(computed.position[0] > computed.target[0]);
        CHECK(computed.nearClip < glm::length(vec(computed.position) - vec(computed.target)));
    }
    // A creature without a portrait camera at all, baked.
    vd::Result<vd::formats::M2Model> cub = readM2(*mpq, "Creature\\Tiger\\TigerCub.m2");
    REQUIRE(cub.ok());
    CHECK(cub.value().camera(vd::formats::kM2CameraPortrait) == nullptr);
    vd::Result<std::vector<u8>> blob = vd::bake::bakeActor(cub.value());
    REQUIRE(blob.ok());
    vd::pack::ActorModelHeader header;
    std::memcpy(&header, blob.value().data(), sizeof(header));
    CHECK(header.version == vd::pack::kActorFormatVersion);
    REQUIRE(header.portraitOffset != 0);
    REQUIRE(header.portraitOffset + sizeof(vd::pack::ActorPortrait) <= blob.value().size());
    vd::pack::ActorPortrait p;
    std::memcpy(&p, blob.value().data() + header.portraitOffset, sizeof(p));
    CHECK((p.flags & vd::pack::kActorPortraitComputed) != 0);
    CHECK(p.target[2] > header.boundsMin[2]);
    CHECK(p.target[2] <= header.boundsMax[2]);
    CHECK(p.position[0] > p.target[0]);
    CHECK(p.fov > 0.5f);
}

TEST_CASE("Baked portrait: race models keep their camera in actors.vdpk") {
    VD_REQUIRE_MPQ(mpq);
    vd::Result<vd::formats::M2Model> parsed = readM2(*mpq, "Character\\Human\\Male\\HumanMale.m2");
    REQUIRE(parsed.ok());
    vd::Result<std::vector<u8>> blob = vd::bake::bakeActor(parsed.value());
    REQUIRE(blob.ok());
    vd::pack::ActorModelHeader header;
    std::memcpy(&header, blob.value().data(), sizeof(header));
    REQUIRE(header.portraitOffset != 0);
    CHECK(header.portraitOffset % 16 == 0);
    vd::pack::ActorPortrait p;
    std::memcpy(&p, blob.value().data() + header.portraitOffset, sizeof(p));
    CHECK(p.flags == 0);
    CHECK(p.position[0] == doctest::Approx(0.633f).epsilon(0.002));
    CHECK(p.target[2] == doctest::Approx(1.864f).epsilon(0.002));
    CHECK(p.fov == doctest::Approx(0.785f).epsilon(0.002));
    CHECK(p.nearClip == doctest::Approx(0.2222f).epsilon(0.01));
}
