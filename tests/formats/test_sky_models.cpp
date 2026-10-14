// Phase 11: the sky models baked from Environments\Stars (pack/SkyFormat.h) and the skyboxes the WMOs name
// (MOSB), against the original client (VD_WOW_DIR).
#include "bake/ModelBaker.h"
#include "bake/SkyBaker.h"
#include "core/Hash.h"
#include "formats/M2.h"
#include "formats/Wmo.h"
#include "pack/ModelFormat.h"
#include "pack/SkyFormat.h"

#include "SharedMpq.h"

#include <doctest/doctest.h>

#include <cstring>
#include <set>
#include <vector>

namespace {

using vd::f32;
using vd::u32;
using vd::u64;
using vd::u8;

vd::Result<std::vector<u8>> bakeSky(const vd::formats::MpqArchiveSet& mpq, const char* path) {
    vd::Result<std::vector<u8>> bytes = mpq.read(path);
    if (!bytes) {
        return bytes.error();
    }
    vd::Result<vd::formats::M2Model> model = vd::formats::parseM2(bytes.value());
    if (!model) {
        return model.error();
    }
    return vd::bake::bakeSkyModel(model.value());
}

template <typename T> std::vector<T> section(const std::vector<u8>& blob, u32 offset, u32 count) {
    std::vector<T> out(count);
    if (count > 0) {
        std::memcpy(out.data(), blob.data() + offset, count * sizeof(T));
    }
    return out;
}

} // namespace

TEST_CASE("Sky models: the stars keep their layers' opacities, merged by texture") {
    VD_REQUIRE_MPQ(mpq);
    vd::Result<std::vector<u8>> blob = bakeSky(*mpq, "Environments\\Stars\\Stars.m2");
    REQUIRE(blob);
    vd::pack::SkyModelHeader header;
    std::memcpy(&header, blob.value().data(), sizeof(header));
    CHECK(header.version == vd::pack::kSkyModelFormatVersion);
    CHECK(header.indexCount == 240);
    CHECK(header.rotationLoop == 0); // the stars do not turn
    CHECK(header.radius == doctest::Approx(24.88f).epsilon(0.01));
    // Seven batches with two textures: two parts, Stars.blp and Stars2.blp, alpha blended.
    const auto parts = section<vd::pack::SkyPart>(blob.value(), header.partsOffset, header.partCount);
    REQUIRE(parts.size() == 2);
    CHECK(parts[0].texture == vd::assetPathHash("Environments\\Stars\\Stars.blp"));
    CHECK(parts[1].texture == vd::assetPathHash("Environments\\Stars\\Stars2.blp"));
    CHECK(parts[0].blend == static_cast<u8>(vd::pack::ModelBlend::Alpha));
    CHECK(parts[0].indexCount + parts[1].indexCount == 240);
    // Their transparency weights (1, 0.5, 0.75, 0.65, 0.25, 0.35) in the vertices' alpha.
    const auto vertices =
        section<vd::pack::SkyVertex>(blob.value(), header.verticesOffset, header.vertexCount);
    std::set<u32> alphas;
    for (const vd::pack::SkyVertex& v : vertices) {
        alphas.insert(v.color[3]);
    }
    CHECK(alphas == std::set<u32>{64, 89, 128, 166, 191, 255});
}

TEST_CASE("Sky models: DeathClouds turns once every 133 seconds") {
    VD_REQUIRE_MPQ(mpq);
    vd::Result<std::vector<u8>> blob = bakeSky(*mpq, "Environments\\Stars\\DeathClouds.m2");
    REQUIRE(blob);
    vd::pack::SkyModelHeader header;
    std::memcpy(&header, blob.value().data(), sizeof(header));
    CHECK(header.partCount == 1);
    CHECK(header.indexCount == 1728);
    CHECK(header.rotationLoop == 133267);
    const auto keys =
        section<vd::pack::SkyRotationKey>(blob.value(), header.rotationKeysOffset, header.rotationKeyCount);
    REQUIRE(keys.size() == 19);
    CHECK(keys[0].time == 0);
    CHECK(keys[1].time == 8341);
    CHECK(keys[1].rotation[2] == doctest::Approx(-0.1951f).epsilon(0.001)); // about Z
    CHECK(keys[1].rotation[3] == doctest::Approx(0.9808f).epsilon(0.001));
}

TEST_CASE("Sky models: Stratholme's WMO names its skybox (MOSB), kept in the baked WMO") {
    VD_REQUIRE_MPQ(mpq);
    const std::string path = "World\\wmo\\Dungeon\\LD_Stratholme\\Stratholme_A.wmo";
    vd::Result<std::vector<u8>> bytes = mpq->read(path);
    REQUIRE(bytes);
    vd::Result<vd::formats::WmoRoot> root = vd::formats::parseWmoRoot(bytes.value());
    REQUIRE(root);
    CHECK(root.value().skybox == "ENVIRONMENTS\\STARS\\STRATHOLMESKYBOX.MDX");
    std::vector<vd::formats::WmoGroup> groups;
    for (u32 g = 0; g < root.value().groups.size(); ++g) {
        vd::Result<std::vector<u8>> groupBytes = mpq->read(vd::formats::wmoGroupPath(path, g));
        REQUIRE(groupBytes);
        vd::Result<vd::formats::WmoGroup> group = vd::formats::parseWmoGroup(groupBytes.value());
        REQUIRE(group);
        groups.push_back(std::move(group.value()));
    }
    vd::Result<std::vector<u8>> blob = vd::bake::bakeWmo(root.value(), groups);
    REQUIRE(blob);
    vd::pack::ModelHeader header;
    std::memcpy(&header, blob.value().data(), sizeof(header));
    const u64 skybox = u64{header.reserved2[0]} | (u64{header.reserved2[1]} << 32);
    CHECK(skybox == vd::assetPathHash("sky/environments/stars/stratholmeskybox.m2"));

    // A WMO without MOSB keeps 0: Stormwind.
    vd::Result<std::vector<u8>> stormwind =
        mpq->read("World\\wmo\\Azeroth\\Buildings\\Stormwind\\Stormwind.wmo");
    REQUIRE(stormwind);
    vd::Result<vd::formats::WmoRoot> stormwindRoot = vd::formats::parseWmoRoot(stormwind.value());
    REQUIRE(stormwindRoot);
    CHECK(stormwindRoot.value().skybox.empty());
    CHECK(vd::pack::skyModelPath(stormwindRoot.value().skybox).empty());
}
