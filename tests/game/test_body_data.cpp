// Phase 14 (bodyfx): the client tables of the units' body effects (game/BodyData) on real rows of the 1.12.1
// client: CreatureModelData, UnitBloodLevels / UnitBlood, TerrainType, FootprintTextures, the HARDCODED
// effects of SpellVisualEffectName, the cold areas of AreaTable and the sequence boxes of bodyfx.vdpk.
#include "core/Hash.h"
#include "fx/EffectSet.h"
#include "game/AreaData.h"
#include "game/BodyData.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>

using namespace vd;
using namespace vd::game;

namespace {

bool openPack(pack::PackReader& pack, const char* name) {
    const char* dir = std::getenv("VD_PACKS_DIR");
    return dir && pack.open(std::filesystem::path(dir) / name).ok();
}

u64 model(const char* path) {
    return fx::effectModelHash(path);
}

} // namespace

TEST_CASE("Body data: CreatureModelData, blood, ground and breath of the 1.12.1 client (VD_PACKS_DIR)") {
    pack::PackReader db;
    if (!openPack(db, "db.vdpk")) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    BodyData data;
    REQUIRE(data.init(db));

    // CreatureModelData 49 (HumanMale): red blood (level 1), the base footprint 12 x 10 inches, 2.031 yd
    // tall.
    const BodyModel* human = data.model(model("Character\\Human\\Male\\HumanMale.mdx"));
    REQUIRE(human);
    CHECK(human->bloodLevel == 1);
    CHECK(human->footprint == assetPathHash("textures\\Footsteps\\BaseFootprint.blp"));
    CHECK(human->footprintLength == doctest::Approx(12.0f / 36.0f));
    CHECK(human->footprintWidth == doctest::Approx(10.0f / 36.0f));
    CHECK(human->collisionHeight == doctest::Approx(2.031f).epsilon(0.001));
    CHECK((human->flags & (kBodyNoFootstepSprays | kBodyNoBreath)) == 0);
    // A wolf's paws (FootprintTextures 7); a fire elemental has no blood nor prints.
    const BodyModel* wolf = data.model(model("Creature\\Wolf\\Wolf.mdx"));
    REQUIRE(wolf);
    CHECK(wolf->footprint == assetPathHash("textures\\Footsteps\\PawFootprint.blp"));
    const BodyModel* fire = data.model(model("Creature\\FireElemental\\FireElemental.mdx"));
    REQUIRE(fire);
    CHECK(fire->bloodLevel == 0);
    CHECK(fire->footprint == 0);
    CHECK(data.bloodSpurt(fire->bloodLevel, true, false).model == 0);
    // A ghost neither sprays nor breathes (flags 3).
    const BodyModel* ghost = data.model(model("Creature\\Ghost\\Ghost.mdx"));
    REQUIRE(ghost);
    CHECK((ghost->flags & kBodyNoFootstepSprays) != 0);
    CHECK((ghost->flags & kBodyNoBreath) != 0);

    // UnitBlood 1 (red): in front 109 / 164, behind 534 / 55; violence 1 makes it green (UnitBlood 2), 0
    // none.
    CHECK(data.bloodSpurt(1, true, false).model == model("Particles\\BloodSpurts\\BloodSpurt.mdl"));
    CHECK(data.bloodSpurt(1, true, true).model == model("Particles\\BloodSpurts\\BloodSpurtLarge.mdl"));
    CHECK(data.bloodSpurt(1, false, false).model == model("Particles\\BloodSpurts\\BloodSpurt.mdx"));
    CHECK(data.bloodSpurt(1, false, true).model == model("Particles\\BloodSpurts\\BloodSpurtLarge.mdx"));
    CHECK(data.bloodSpurt(1, true, false, 1).model == model("Particles\\BloodSpurts\\BloodSpurtGreen.mdl"));
    CHECK(data.bloodSpurt(1, true, false, 0).model == 0);
    // Insects and the undead bleed green (level 2), golems and demons black (level 3).
    CHECK(data.bloodSpurt(2, true, true).model == model("Particles\\BloodSpurts\\BloodSpurtGreenLarge.mdl"));
    CHECK(data.bloodSpurt(3, false, true).model == model("Particles\\BloodSpurts\\BloodSpurtBlackLarge.mdx"));

    // TerrainType: only snow (SoundID 4) kicks up a spray; snow and sand (8) keep footprints.
    CHECK(data.footstepSpray(4, false).model == model("Particles\\FootstepSpraySnow.mdl"));
    CHECK(data.footstepSpray(4, true).model == model("Particles\\FootstepSpraySnowWalk.mdl"));
    CHECK(data.footstepSpray(1, false).model == 0); // dirt: none in 1.12
    CHECK(data.keepsFootprints(4));
    CHECK(data.keepsFootprints(8));
    CHECK_FALSE(data.keepsFootprints(1));
    CHECK_FALSE(data.keepsFootprints(6));
    CHECK_FALSE(data.keepsFootprints(0));

    // The HARDCODED effects of SpellVisualEffectName.
    CHECK(data.hardcoded(HardcodedEffect::WaterRunSpray).model == model("Particles\\FootstepSprayWater.mdl"));
    CHECK(data.hardcoded(HardcodedEffect::WaterWalkSpray).model ==
          model("Particles\\FootstepSprayWaterWalk.mdl"));
    CHECK(data.hardcoded(HardcodedEffect::BreathCold).model == model("Particles\\ColdBreath.mdl"));
    CHECK(data.hardcoded(HardcodedEffect::BreathUnderwater).model == model("Particles\\Bubbles.mdl"));
    CHECK(data.hardcoded(HardcodedEffect::MountPoof).model == model("Spells\\DruidMorph_Impact_Base.mdx"));
    CHECK(data.hardcoded(HardcodedEffect::MountPoof).scale == doctest::Approx(2.0f));
    // Phase 16 (alerts): the meeting stone's join (its scale 0 counts as 1).
    CHECK(data.hardcoded(HardcodedEffect::MeetingStoneJoin).model == model("Spells\\Bind_Impact_Base.mdx"));
    CHECK(data.hardcoded(HardcodedEffect::MeetingStoneJoin).scale == doctest::Approx(1.0f));

    // Cold areas: Dun Morogh and its Kharanos (its zone's flag), Winterspring and the Razorfen Downs instance
    // (its own); not Elwynn, nor the Barrens' subzone of the same name.
    AreaData areas;
    REQUIRE(areas.init(db));
    CHECK(BodyData::coldArea(areas, 1));
    CHECK(BodyData::coldArea(areas, 131));
    CHECK(BodyData::coldArea(areas, 618));
    CHECK(BodyData::coldArea(areas, 722));
    CHECK_FALSE(BodyData::coldArea(areas, 12));
    CHECK_FALSE(BodyData::coldArea(areas, 1316));
    CHECK_FALSE(BodyData::coldArea(areas, 0));
}

TEST_CASE("Body data: the sequence boxes of bodyfx.vdpk (VD_PACKS_DIR)") {
    pack::PackReader bodyfx;
    if (!openPack(bodyfx, "bodyfx.vdpk")) {
        MESSAGE("bodyfx.vdpk missing (vd_baker bake --only bodyfx): skipping");
        return;
    }
    BodyData data;
    REQUIRE(data.openBounds(bodyfx));
    // HumanMale: Stand (0) is about 0.9 yd long and 1.1 wide; Run (5) stretches to 1.8; an animation it lacks
    // gets Stand's.
    const u64 human = model("Character\\Human\\Male\\HumanMale.mdx");
    const pack::BodyBoundsBox* stand = data.bounds(human, 0);
    REQUIRE(stand);
    CHECK(stand->animation == 0);
    CHECK(stand->min[0] == doctest::Approx(-0.58f).epsilon(0.02));
    CHECK(stand->max[0] == doctest::Approx(0.34f).epsilon(0.02));
    CHECK(stand->max[1] - stand->min[1] == doctest::Approx(1.08f).epsilon(0.03));
    const pack::BodyBoundsBox* run = data.bounds(human, 5);
    REQUIRE(run);
    CHECK(run->animation == 5);
    CHECK(run->max[0] - run->min[0] == doctest::Approx(1.81f).epsilon(0.03));
    CHECK(data.bounds(human, 999) == stand);
    // A wolf is long: its Stand box is 3.8 yd from nose to tail.
    const pack::BodyBoundsBox* wolf = data.bounds(model("Creature\\Wolf\\Wolf.mdx"), 0);
    REQUIRE(wolf);
    CHECK(wolf->max[0] - wolf->min[0] > 3.0f);
    CHECK(data.bounds(model("Spells\\Frostbolt.mdx"), 0) == nullptr); // no unit's model
}
