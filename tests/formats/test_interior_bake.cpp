// Phase 11 (f11/interiors): the WMO lights (MOLT), fogs (MFOG), group fogs and lights (MOGP, MOLR), doodad groups
// (MODR) and the M2 lights, parsed and baked from the real client (VD_WOW_DIR).
#include "bake/ModelBaker.h"
#include "formats/M2.h"
#include "formats/Wmo.h"
#include "pack/ModelFormat.h"

#include "SharedMpq.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

namespace {

using vd::f32;
using vd::u16;
using vd::u32;
using vd::u8;

const char* const kGoldshireInn = "World\\wmo\\Azeroth\\Buildings\\GoldshireInn\\GoldshireInn.wmo";

vd::Result<vd::formats::WmoRoot> readWmo(const vd::formats::MpqArchiveSet& mpq, const std::string& path,
                                         std::vector<vd::formats::WmoGroup>& groups) {
    vd::Result<std::vector<u8>> bytes = mpq.read(path);
    if (!bytes) {
        return bytes.error();
    }
    vd::Result<vd::formats::WmoRoot> root = vd::formats::parseWmoRoot(bytes.value());
    for (u32 g = 0; root && g < root.value().groups.size(); ++g) {
        vd::Result<std::vector<u8>> groupBytes = mpq.read(vd::formats::wmoGroupPath(path, g));
        if (!groupBytes) {
            return groupBytes.error();
        }
        vd::Result<vd::formats::WmoGroup> group = vd::formats::parseWmoGroup(groupBytes.value());
        if (!group) {
            return group.error();
        }
        groups.push_back(std::move(group.value()));
    }
    return root;
}

template <typename T>
const T* section(const std::vector<u8>& blob, u32 offset) {
    return reinterpret_cast<const T*>(blob.data() + offset);
}

vd::pack::ModelLightingHeader lightingOf(const std::vector<u8>& blob) {
    vd::pack::ModelHeader header;
    std::memcpy(&header, blob.data(), sizeof(header));
    REQUIRE(header.lightingOffset != 0);
    REQUIRE(header.lightingOffset + sizeof(vd::pack::ModelLightingHeader) <= blob.size());
    vd::pack::ModelLightingHeader lighting;
    std::memcpy(&lighting, blob.data() + header.lightingOffset, sizeof(lighting));
    return lighting;
}

} // namespace

TEST_CASE("WMO lights and fogs: Goldshire's inn") {
    VD_REQUIRE_MPQ(mpq);
    std::vector<vd::formats::WmoGroup> groups;
    vd::Result<vd::formats::WmoRoot> root = readWmo(*mpq, kGoldshireInn, groups);
    REQUIRE(root.ok());
    const vd::formats::WmoRoot& inn = root.value();
    // Ten lamps: nine lanterns and candles (pale yellow, fading from 5 to 7 yards) and two warmer ones.
    REQUIRE(inn.lights.size() == 10);
    const vd::formats::WmoLight& lantern = inn.lights[4];
    CHECK(lantern.type == vd::formats::WmoLightType::Omni);
    CHECK(lantern.attenuated);
    CHECK(lantern.color == 0xFFFFFFA0u); // BGRA: (255, 255, 160)
    CHECK(lantern.position[0] == doctest::Approx(-9.7f).epsilon(0.01));
    CHECK(lantern.position[2] == doctest::Approx(13.1f).epsilon(0.01));
    CHECK(lantern.attenStart == doctest::Approx(5.0f).epsilon(0.01));
    CHECK(lantern.attenEnd == doctest::Approx(6.97f).epsilon(0.01));
    // Two fogs: the second, warm and short, everywhere in the groups that list it (the common room).
    REQUIRE(inn.fogs.size() == 2);
    CHECK((inn.fogs[1].flags & vd::formats::kWmoFogInfinite) != 0);
    CHECK(inn.fogs[1].end == doctest::Approx(83.33f).epsilon(0.01));
    CHECK(inn.fogs[1].startScale == doctest::Approx(0.25f));
    CHECK(inn.fogs[1].color == 0xFFFDCF9Eu);
    REQUIRE(groups.size() == 12);
    CHECK(groups[5].fogIds[0] == 1);
    CHECK(groups[2].fogIds[0] == 0);
    // The outside shell lists the lanterns hanging on the walls, the stairs group the fire's light.
    REQUIRE(groups[4].lightRefs.size() == 9);
    CHECK(groups[4].lightRefs[8] == 8);
    REQUIRE(groups[3].lightRefs.size() == 1);
    CHECK(groups[3].lightRefs[0] == 9);
    CHECK(groups[5].doodadRefs.size() == 362);
}

TEST_CASE("WMO bake: the lighting section of Goldshire's inn") {
    VD_REQUIRE_MPQ(mpq);
    std::vector<vd::formats::WmoGroup> groups;
    vd::Result<vd::formats::WmoRoot> root = readWmo(*mpq, kGoldshireInn, groups);
    REQUIRE(root.ok());
    vd::Result<std::vector<u8>> baked = vd::bake::bakeWmo(root.value(), groups);
    REQUIRE(baked.ok());
    const std::vector<u8>& blob = baked.value();
    const vd::pack::ModelLightingHeader h = lightingOf(blob);
    CHECK(h.lightCount == 10);
    CHECK(h.fogCount == 2);
    REQUIRE(h.groupCount == groups.size());
    REQUIRE(h.doodadCount == root.value().doodads.size());
    const auto* lights = section<vd::pack::ModelLight>(blob, h.lightsOffset);
    CHECK(lights[4].color == 0xFFA0FFFFu); // RGBA
    CHECK(lights[4].attenEnd == doctest::Approx(6.97f).epsilon(0.01));
    const auto* fogs = section<vd::pack::ModelFog>(blob, h.fogsOffset);
    CHECK(fogs[1].flags == vd::pack::kModelFogInfinite);
    CHECK(fogs[1].color == 0xFF9ECFFDu);
    const auto* lighting = section<vd::pack::ModelGroupLighting>(blob, h.groupsOffset);
    const auto* refs = section<u16>(blob, h.lightRefsOffset);
    const auto* cells = section<u32>(blob, h.cellsOffset);
    const auto* doodadGroups = section<u16>(blob, h.doodadGroupsOffset);
    for (u32 g = 0; g < h.groupCount; ++g) {
        CAPTURE(g);
        const vd::pack::ModelGroupLighting& l = lighting[g];
        CHECK(l.firstLightRef + l.lightRefCount <= h.lightRefCount);
        for (u32 r = l.firstLightRef; r < l.firstLightRef + l.lightRefCount; ++r) {
            CHECK(refs[r] < h.lightCount);
        }
        const bool interior = (groups[g].flags & vd::formats::kWmoGroupInterior) &&
                              !(groups[g].flags & vd::formats::kWmoGroupExteriorLit);
        if (interior && !groups[g].colors.empty()) {
            // A light grid of cells of 3 yards or more over the whole group, every cell lit.
            REQUIRE(l.cellSize >= 3.0f);
            const u32 count = u32{l.gridSize[0]} * l.gridSize[1] * l.gridSize[2];
            REQUIRE(count > 0);
            REQUIRE(l.firstCell + count <= h.cellCount);
            for (u32 c = l.firstCell; c < l.firstCell + count; ++c) {
                CHECK((cells[c] >> 24) == 0xFF);
            }
            CHECK(l.gridMin[0] + l.cellSize * l.gridSize[0] >= groups[g].boundsMax[0] - 1.0f);
            CHECK(l.average != 0);
        } else {
            CHECK(l.cellSize == 0.0f);
        }
    }
    // Lit by batch (MOGP's transition / interior / exterior counts), in the vertex alpha: the outside shell (no vertex
    // colors) all outdoor light, the common room's interior batches none and its one exterior batch all, the
    // fireplace's glow (three transition batches) some of each.
    vd::pack::ModelHeader header;
    std::memcpy(&header, blob.data(), sizeof(header));
    const auto* parts = section<vd::pack::ModelPart>(blob, header.partsOffset);
    const auto* indices = section<u16>(blob, header.indicesOffset);
    const auto* vertices = section<vd::pack::ModelVertex>(blob, header.verticesOffset);
    u32 shellIndoor = 0, roomIndoor = 0, roomOutdoor = 0, glowBlend = 0;
    for (u32 p = 0; p < header.partCount; ++p) {
        for (u32 i = parts[p].firstIndex; i < parts[p].firstIndex + parts[p].indexCount; ++i) {
            const u8 outdoor = vertices[indices[i]].color[3];
            shellIndoor += parts[p].group == 4 && outdoor != 255 ? 1 : 0;
            roomIndoor += parts[p].group == 5 && outdoor == 0 ? 1 : 0;
            roomOutdoor += parts[p].group == 5 && outdoor == 255 ? 1 : 0;
            glowBlend += parts[p].group == 0 && outdoor > 0 && outdoor < 255 ? 1 : 0;
        }
    }
    CHECK(shellIndoor == 0);
    CHECK(roomIndoor > 0);
    CHECK(roomOutdoor > 0);
    CHECK(glowBlend > 0);

    // The common room's barrel (doodad 0) belongs to it, a true interior; every doodad a group lists has a group.
    CHECK(doodadGroups[0] == 5);
    for (u32 g = 0; g < groups.size(); ++g) {
        for (u16 d : groups[g].doodadRefs) {
            REQUIRE(d < h.doodadCount);
            CHECK(doodadGroups[d] != vd::pack::kModelNoGroup);
        }
    }
}

TEST_CASE("WMO fogs: the Great Forge's orange haze") {
    VD_REQUIRE_MPQ(mpq);
    vd::Result<std::vector<u8>> bytes = mpq->read("World\\wmo\\KhazModan\\Cities\\Ironforge\\ironforge.wmo");
    REQUIRE(bytes.ok());
    vd::Result<vd::formats::WmoRoot> root = vd::formats::parseWmoRoot(bytes.value());
    REQUIRE(root.ok());
    REQUIRE(root.value().fogs.size() == 2);
    const vd::formats::WmoFog& forge = root.value().fogs[1];
    CHECK(forge.color == 0xFFE78543u); // BGRA: (231, 133, 67)
    CHECK(forge.innerRadius == doctest::Approx(108.6f).epsilon(0.01));
    CHECK(forge.outerRadius == doctest::Approx(165.0f).epsilon(0.01));
    CHECK(forge.end == doctest::Approx(805.6f).epsilon(0.01));
    CHECK(root.value().lights.size() == 209);
}

TEST_CASE("M2 lights: a campfire lights its stones, a NoOmni brazier has none") {
    VD_REQUIRE_MPQ(mpq);
    vd::Result<std::vector<u8>> bytes = mpq->read("World\\AZEROTH\\ELWYNN\\PASSIVEDOODADS\\Campfire\\ElwynnCampfire.m2");
    REQUIRE(bytes.ok());
    vd::Result<vd::formats::M2Model> model = vd::formats::parseM2(bytes.value());
    REQUIRE(model.ok());
    REQUIRE(model.value().lights.size() == 1);
    const vd::formats::M2Light& fire = model.value().lights[0];
    CHECK(fire.type == vd::formats::kM2LightPoint);
    CHECK(fire.position[2] == doctest::Approx(0.37f).epsilon(0.02));
    REQUIRE(!fire.diffuseColor.values.empty());
    CHECK(fire.diffuseColor.values[0][0] == doctest::Approx(0.71f).epsilon(0.01));
    CHECK(fire.diffuseIntensity.times.size() > 2); // it flickers
    CHECK(vd::formats::sampleM2Track(fire.attenuationEnd, 0, 0, 0.0f) == doctest::Approx(1.667f).epsilon(0.01));

    vd::Result<std::vector<u8>> baked = vd::bake::bakeM2(model.value());
    REQUIRE(baked.ok());
    const vd::pack::ModelLightingHeader h = lightingOf(baked.value());
    REQUIRE(h.lightCount == 1);
    const auto* light = section<vd::pack::ModelLight>(baked.value(), h.lightsOffset);
    CHECK(light->type == vd::pack::kModelLightOmni);
    CHECK((light->color & 0xFF) == 181); // R of (0.71, 0.2, 0)
    CHECK(light->attenEnd == doctest::Approx(1.667f).epsilon(0.01));
    CHECK(light->keyCount > 2);
    CHECK(light->loopMs > 0);
    REQUIRE(light->firstKey + light->keyCount <= h.lightKeyCount);
    const auto* keys = section<vd::pack::ModelLightKey>(baked.value(), h.lightKeysOffset);
    for (u32 k = 1; k < light->keyCount; ++k) {
        CHECK(keys[light->firstKey + k].time >= keys[light->firstKey + k - 1].time);
        CHECK(keys[light->firstKey + k].time <= light->loopMs);
    }

    vd::Result<std::vector<u8>> plain =
        mpq->read("World\\Generic\\ORC\\PASSIVE DOODADS\\BRAZIERS\\SmallBrazierNoOmni01.m2");
    REQUIRE(plain.ok());
    vd::Result<vd::formats::M2Model> noOmni = vd::formats::parseM2(plain.value());
    REQUIRE(noOmni.ok());
    CHECK(noOmni.value().lights.empty());
}
