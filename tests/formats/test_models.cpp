#include "bake/ModelBaker.h"
#include "core/Hash.h"
#include "formats/Adt.h"
#include "formats/M2.h"
#include "formats/Wmo.h"
#include "pack/ModelFormat.h"

#include "SharedMpq.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace {

using vd::f32;
using vd::u16;
using vd::u32;
using vd::u8;
using vd::usize;

vd::Result<vd::formats::WmoRoot> readWmo(const vd::formats::MpqArchiveSet& mpq, const std::string& path,
                                         std::vector<vd::formats::WmoGroup>* groups) {
    vd::Result<std::vector<u8>> bytes = mpq.read(path);
    if (!bytes) {
        return bytes.error();
    }
    vd::Result<vd::formats::WmoRoot> root = vd::formats::parseWmoRoot(bytes.value());
    if (root && groups) {
        for (u32 g = 0; g < root.value().groups.size(); ++g) {
            vd::Result<std::vector<u8>> groupBytes = mpq.read(vd::formats::wmoGroupPath(path, g));
            if (!groupBytes) {
                return groupBytes.error();
            }
            vd::Result<vd::formats::WmoGroup> group = vd::formats::parseWmoGroup(groupBytes.value());
            if (!group) {
                return group.error();
            }
            groups->push_back(std::move(group.value()));
        }
    }
    return root;
}

template <typename T>
const T* section(const std::vector<u8>& blob, u32 offset) {
    return reinterpret_cast<const T*>(blob.data() + offset);
}

} // namespace

TEST_CASE("WMO parser reads Stormwind") {
    VD_REQUIRE_MPQ(mpq);
    vd::Result<vd::formats::WmoRoot> root =
        readWmo(*mpq, "World\\wmo\\Azeroth\\Buildings\\Stormwind\\Stormwind.wmo", nullptr);
    REQUIRE(root.ok());
    CHECK(root.value().groups.size() == 306);
    CHECK(root.value().materials.size() == 194);
    CHECK(root.value().portals.size() == 341);
    REQUIRE(root.value().doodadSets.size() == 1);
    CHECK(root.value().doodadSets[0].name == "Set_$DefaultGlobal");
    CHECK(root.value().doodads.size() >= root.value().doodadSets[0].count);
    CHECK(root.value().materials[0].texture1 == "DUNGEONS\\TEXTURES\\WALLS\\MM_STRMWND_WALL_01.BLP");
    CHECK(vd::formats::m2Path(root.value().doodads[0].model) ==
          "WORLD\\GENERIC\\HUMAN\\PASSIVE DOODADS\\DRAPERY\\DRAPERY01.m2");
    CHECK(vd::formats::wmoGroupPath("A\\B.wmo", 7) == "A\\B_007.wmo");
}

TEST_CASE("WMO liquids: Stormwind canals are water, the Great Forge is magma") {
    // Kinds come from the tiles' legacy type (every MOGP liquid field is 15 in 1.12). Ironforge also has
    // two small water groups; vertices of empty tiles hold height 0 and are never drawn.
    VD_REQUIRE_MPQ(mpq);
    struct Case {
        const char* path;
        vd::formats::WmoLiquidKind kind;
    };
    const Case cases[] = {
        {"World\\wmo\\Azeroth\\Buildings\\Stormwind\\Stormwind.wmo", vd::formats::WmoLiquidKind::Water},
        {"World\\wmo\\KhazModan\\Cities\\Ironforge\\ironforge.wmo", vd::formats::WmoLiquidKind::Magma},
    };
    for (const Case& c : cases) {
        const std::string path = c.path;
        CAPTURE(path);
        std::vector<vd::formats::WmoGroup> groups;
        vd::Result<vd::formats::WmoRoot> root = readWmo(*mpq, c.path, &groups);
        REQUIRE(root.ok());
        u32 found = 0;
        for (const vd::formats::WmoGroup& g : groups) {
            if (vd::formats::wmoLiquidKind(root.value(), g) != c.kind) {
                continue;
            }
            ++found;
            CHECK(g.liquid.heights.size() == g.liquid.vertsX * g.liquid.vertsY);
            CHECK(g.liquid.tiles.size() == g.liquid.tilesX * g.liquid.tilesY);
            // The surface lies inside the group, but its grid is snapped to whole liquid tiles.
            CHECK(g.liquid.corner[0] >= g.boundsMin[0] - vd::formats::kWmoLiquidTileSize);
            CHECK(g.liquid.corner[1] >= g.boundsMin[1] - vd::formats::kWmoLiquidTileSize);
            for (u32 t = 0; t < g.liquid.tiles.size(); ++t) {
                if ((g.liquid.tiles[t] & 0x0F) != vd::formats::kWmoLiquidTileEmpty) {
                    const f32 h = g.liquid.heights[(t / g.liquid.tilesX) * g.liquid.vertsX + t % g.liquid.tilesX];
                    CHECK(h >= g.boundsMin[2] - 1.0f);
                    CHECK(h <= g.boundsMax[2] + 1.0f);
                    break;
                }
            }
        }
        CHECK(found > 0);

        // Baked: a liquid material of that kind, drawn by some part.
        vd::Result<std::vector<u8>> blob = vd::bake::bakeWmo(root.value(), groups);
        REQUIRE(blob.ok());
        vd::pack::ModelHeader header;
        std::memcpy(&header, blob.value().data(), sizeof(header));
        const auto* materials = section<vd::pack::ModelMaterial>(blob.value(), header.materialsOffset);
        const auto* parts = section<vd::pack::ModelPart>(blob.value(), header.partsOffset);
        // Cities exceed 65536 vertices: one vertex space with 32-bit indices.
        REQUIRE(header.vertexCount > 0x10000);
        REQUIRE((header.flags & vd::pack::kModelIndices32) != 0);
        const auto* indices = section<u32>(blob.value(), header.indicesOffset);
        for (u32 i = 0; i < header.indexCount; ++i) {
            REQUIRE(indices[i] < header.vertexCount);
        }
        u32 liquidIndices = 0;
        for (u32 p = 0; p < header.partCount; ++p) {
            const vd::pack::ModelMaterial& m = materials[parts[p].material];
            if ((m.flags & vd::pack::kModelMaterialLiquid) && m.color == static_cast<u32>(c.kind)) {
                CHECK(parts[p].indexCount % 6 == 0); // whole tiles
                liquidIndices += parts[p].indexCount;
            }
        }
        CHECK(liquidIndices > 0);
    }
}

TEST_CASE("WMO bake: Stormwind's unreachable cathedral drafts are never drawn (B1)") {
    // Groups 109, 110 and 223 are rough copies of the cathedral floating over the city: unreachable (MOGP 0x80),
    // interior, without portals, neither exterior nor "always draw". The 1.12 client never draws them, nor their
    // doodads; the streets (0x8000 "always draw") stay.
    VD_REQUIRE_MPQ(mpq);
    std::vector<vd::formats::WmoGroup> groups;
    vd::Result<vd::formats::WmoRoot> root =
        readWmo(*mpq, "World\\wmo\\Azeroth\\Buildings\\Stormwind\\Stormwind.wmo", &groups);
    REQUIRE(root.ok());
    for (const u32 g : {109u, 110u, 223u}) {
        CAPTURE(g);
        CHECK((groups[g].flags & vd::formats::kWmoGroupUnreachable) != 0);
        CHECK(groups[g].portalCount == 0);
        CHECK(vd::formats::wmoGroupNeverDrawn(groups[g].flags));
    }
    u32 neverDrawn = 0;
    u32 alwaysDrawn = 0;
    for (const vd::formats::WmoGroup& g : groups) {
        neverDrawn += vd::formats::wmoGroupNeverDrawn(g.flags) ? 1 : 0;
        alwaysDrawn += (g.flags & vd::formats::kWmoGroupAlwaysDraw) ? 1 : 0;
    }
    CHECK(neverDrawn == 4); // and 86, a draft of the keep high over it
    CHECK(alwaysDrawn > 50);

    vd::Result<std::vector<u8>> blob = vd::bake::bakeWmo(root.value(), groups);
    REQUIRE(blob.ok());
    vd::pack::ModelHeader header;
    std::memcpy(&header, blob.value().data(), sizeof(header));
    const auto* baked = section<vd::pack::ModelGroup>(blob.value(), header.groupsOffset);
    CHECK(vd::pack::modelGroupNeverDrawn(baked[223].flags));
    CHECK_FALSE(vd::pack::modelGroupNeverDrawn(baked[1].flags));
    CHECK((baked[1].flags & vd::pack::kModelGroupAlwaysDraw) != 0);
    // Doodads only the drafts refer to are baked without a model; those a drawn group also refers to keep theirs.
    const auto* doodads = section<vd::pack::ModelDoodad>(blob.value(), header.doodadsOffset);
    std::vector<u8> drawnRef(root.value().doodads.size(), 0);
    for (const vd::formats::WmoGroup& g : groups) {
        for (const u16 ref : g.doodadRefs) {
            drawnRef[ref] |= vd::formats::wmoGroupNeverDrawn(g.flags) ? 0 : 1;
        }
    }
    u32 checked = 0;
    for (const u32 g : {109u, 110u, 223u}) {
        for (const u16 ref : groups[g].doodadRefs) {
            CHECK((doodads[ref].model == 0) == (drawnRef[ref] == 0));
            ++checked;
        }
    }
    CHECK(checked > 0);
    CHECK(doodads[0].model != 0);
}

TEST_CASE("M2 parser and bake: Elwynn tree") {
    VD_REQUIRE_MPQ(mpq);
    vd::Result<std::vector<u8>> bytes = mpq->read("World\\Azeroth\\Elwynn\\PassiveDoodads\\Trees\\ElwynnTreeCanopy01.m2");
    REQUIRE(bytes.ok());
    vd::Result<vd::formats::M2Model> model = vd::formats::parseM2(bytes.value());
    REQUIRE(model.ok());
    CHECK(model.value().vertices.size() == 141);
    CHECK(model.value().triangles.size() == 366);
    REQUIRE(model.value().batches.size() == 2);
    CHECK(model.value().materials[1].blend == vd::formats::M2Blend::AlphaKey);

    vd::Result<std::vector<u8>> blob = vd::bake::bakeM2(model.value());
    REQUIRE(blob.ok());
    vd::pack::ModelHeader header;
    std::memcpy(&header, blob.value().data(), sizeof(header));
    CHECK(header.kind == static_cast<u32>(vd::pack::ModelKind::M2));
    CHECK(header.vertexCount == 141);
    CHECK(header.indexCount == 366);
    CHECK(header.groupCount == 1);
    REQUIRE(header.partCount == 2);
    const auto* parts = section<vd::pack::ModelPart>(blob.value(), header.partsOffset);
    const auto* materials = section<vd::pack::ModelMaterial>(blob.value(), header.materialsOffset);
    CHECK(materials[parts[0].material].blend == static_cast<u8>(vd::pack::ModelBlend::Opaque)); // trunk first
    CHECK(materials[parts[1].material].blend == static_cast<u8>(vd::pack::ModelBlend::AlphaKey));
    CHECK(materials[parts[1].material].texture ==
          vd::assetPathHash("world/generic/human/passive doodads/stormwind/elwynntreetallcanopy_new01.blp"));
    const auto* indices = section<u16>(blob.value(), header.indicesOffset);
    for (u32 i = 0; i < header.indexCount; ++i) {
        REQUIRE(indices[i] < header.vertexCount);
    }
}

TEST_CASE("M2 bake leaves out the batches the client never shows (Orgrimmar's bonfire embers)") {
    VD_REQUIRE_MPQ(mpq);
    // Phase 10: drawn, its invisible 60-yard mesh covered the Valley of Strength with flat untextured blocks.
    vd::Result<std::vector<u8>> bytes =
        mpq->read("World\\Kalimdor\\Orgrimmar\\PassiveDoodads\\OrgrimmarBonfire\\OrgrimmarFloatingEmbers.m2");
    REQUIRE(bytes.ok());
    vd::Result<vd::formats::M2Model> model = vd::formats::parseM2(bytes.value());
    REQUIRE(model.ok());
    REQUIRE(!model.value().batches.empty());
    CHECK(!model.value().particles.empty());
    u32 invisible = 0;
    vd::Result<std::vector<u8>> blob = vd::bake::bakeM2(model.value(), &invisible);
    REQUIRE(blob.ok());
    CHECK(invisible == model.value().batches.size());
    vd::pack::ModelHeader header;
    std::memcpy(&header, blob.value().data(), sizeof(header));
    CHECK(header.vertexCount == 0); // its effects alone
    CHECK(header.partCount == 0);

    // An ordinary doodad keeps every batch.
    vd::Result<std::vector<u8>> tree = mpq->read("World\\Azeroth\\Elwynn\\PassiveDoodads\\Trees\\ElwynnTreeCanopy01.m2");
    REQUIRE(tree.ok());
    vd::Result<vd::formats::M2Model> treeModel = vd::formats::parseM2(tree.value());
    REQUIRE(treeModel.ok());
    invisible = 0;
    REQUIRE(vd::bake::bakeM2(treeModel.value(), &invisible).ok());
    CHECK(invisible == 0);
}

TEST_CASE("WMO bake keeps every rendered triangle, grouped by material") {
    VD_REQUIRE_MPQ(mpq);
    std::vector<vd::formats::WmoGroup> groups;
    vd::Result<vd::formats::WmoRoot> root =
        readWmo(*mpq, "World\\wmo\\Azeroth\\Buildings\\GoldshireInn\\GoldshireInn.wmo", &groups);
    REQUIRE(root.ok());
    vd::Result<std::vector<u8>> blob = vd::bake::bakeWmo(root.value(), groups);
    REQUIRE(blob.ok());
    vd::pack::ModelHeader header;
    std::memcpy(&header, blob.value().data(), sizeof(header));
    CHECK(header.kind == static_cast<u32>(vd::pack::ModelKind::Wmo));
    CHECK(header.groupCount == groups.size());
    CHECK((header.flags & vd::pack::kModelIndices32) == 0); // small building: 16-bit indices
    CHECK(header.doodadCount == root.value().doodads.size());

    usize batchIndices = 0;
    for (const vd::formats::WmoGroup& g : groups) {
        for (const vd::formats::WmoBatch& b : g.batches) {
            batchIndices += b.indexCount;
        }
    }
    CHECK(header.indexCount == batchIndices);

    const auto* parts = section<vd::pack::ModelPart>(blob.value(), header.partsOffset);
    const auto* materials = section<vd::pack::ModelMaterial>(blob.value(), header.materialsOffset);
    const auto* indices = section<u16>(blob.value(), header.indicesOffset);
    const auto* vertices = section<vd::pack::ModelVertex>(blob.value(), header.verticesOffset);
    // Every group's triangles survive the page layout: same vertices (compared by position sum).
    std::vector<double> bakedSum(groups.size(), 0.0);
    u32 nextIndex = 0;
    for (u32 p = 0; p < header.partCount; ++p) {
        REQUIRE(parts[p].group < groups.size());
        CHECK(parts[p].firstIndex == nextIndex); // parts tile the index buffer in order
        nextIndex += parts[p].indexCount;
        if (p > 0 && parts[p].vertexOffset == parts[p - 1].vertexOffset) {
            CHECK(materials[parts[p].material].blend >= materials[parts[p - 1].material].blend); // opaque first
        }
        for (u32 i = 0; i < parts[p].indexCount; ++i) {
            const u32 v = parts[p].vertexOffset + indices[parts[p].firstIndex + i];
            REQUIRE(v < header.vertexCount);
            bakedSum[parts[p].group] += vertices[v].position[0] + vertices[v].position[1] + vertices[v].position[2];
        }
    }
    for (u32 g = 0; g < groups.size(); ++g) {
        double sum = 0.0;
        for (const vd::formats::WmoBatch& b : groups[g].batches) {
            for (u32 i = b.firstIndex; i < b.firstIndex + b.indexCount; ++i) {
                const auto& position = groups[g].positions[groups[g].indices[i]];
                sum += position[0] + position[1] + position[2];
            }
        }
        CHECK(bakedSum[g] == doctest::Approx(sum).epsilon(1e-6));
    }
}

TEST_CASE("Placement transform matches the MODF extents") {
    VD_REQUIRE_MPQ(mpq);
    // Every WMO placed on the Goldshire tile: the model's bounding box through our transform must land on
    // the world box the file stores for the placement. This pins down the rotation convention.
    vd::Result<std::vector<u8>> bytes = mpq->read(vd::formats::adtPath("Azeroth", 31, 49));
    REQUIRE(bytes.ok());
    vd::Result<vd::formats::Adt> adt = vd::formats::parseAdt(bytes.value());
    REQUIRE(adt.ok());
    REQUIRE_FALSE(adt.value().wmoPlacements.empty());
    // World box of the model's bounding box through a placement transform.
    auto transformedBox = [](const std::array<f32, 12>& m, const vd::formats::WmoRoot& root, f32 (&min)[3],
                             f32 (&max)[3]) {
        for (u32 axis = 0; axis < 3; ++axis) {
            min[axis] = FLT_MAX;
            max[axis] = -FLT_MAX;
        }
        for (u32 corner = 0; corner < 8; ++corner) {
            const f32 v[3] = {(corner & 1) ? root.boundsMax[0] : root.boundsMin[0],
                              (corner & 2) ? root.boundsMax[1] : root.boundsMin[1],
                              (corner & 4) ? root.boundsMax[2] : root.boundsMin[2]};
            for (u32 row = 0; row < 3; ++row) {
                const f32 w = m[row * 4] * v[0] + m[row * 4 + 1] * v[1] + m[row * 4 + 2] * v[2] + m[row * 4 + 3];
                min[row] = std::min(min[row], w);
                max[row] = std::max(max[row], w);
            }
        }
    };
    u32 checked = 0;
    u32 exactMatches = 0;
    u32 wrongExactMatches = 0;
    for (const vd::formats::AdtWmoPlacement& p : adt.value().wmoPlacements) {
        const std::string& path = adt.value().wmos[p.nameId];
        CAPTURE(path);
        vd::Result<vd::formats::WmoRoot> root = readWmo(*mpq, path, nullptr);
        REQUIRE(root.ok());
        const std::array<f32, 3> a = vd::bake::placementToWorld({p.extentsMin[0], p.extentsMin[1], p.extentsMin[2]});
        const std::array<f32, 3> b = vd::bake::placementToWorld({p.extentsMax[0], p.extentsMax[1], p.extentsMax[2]});
        auto inside = [&](const f32 (&min)[3], const f32 (&max)[3], bool& exact) {
            bool ok = true;
            exact = true;
            for (u32 axis = 0; axis < 3; ++axis) {
                const f32 lo = std::min(a[axis], b[axis]);
                const f32 hi = std::max(a[axis], b[axis]);
                ok &= min[axis] > lo - 1.0f && max[axis] < hi + 1.0f;
                exact &= std::abs(min[axis] - lo) < 1.0f && std::abs(max[axis] - hi) < 1.0f;
            }
            return ok;
        };
        // The stored extents also cover doodads sticking out of the building (fences, carts): the model box
        // must lie inside them, and often match them exactly.
        f32 min[3], max[3];
        transformedBox(vd::bake::placementTransform(p.position, p.rotation, 1.0f), root.value(), min, max);
        bool exact = false;
        CHECK(inside(min, max, exact));
        exactMatches += exact ? 1 : 0;
        // A wrong convention (yaw off by 90 degrees) must not reproduce the stored boxes.
        const f32 wrongRotation[3] = {p.rotation[0], p.rotation[1] + 90.0f, p.rotation[2]};
        transformedBox(vd::bake::placementTransform(p.position, wrongRotation, 1.0f), root.value(), min, max);
        inside(min, max, exact);
        wrongExactMatches += exact ? 1 : 0;
        ++checked;
    }
    CHECK(checked > 0);
    CHECK(exactMatches >= checked / 4);
    CHECK(wrongExactMatches == 0);
}
