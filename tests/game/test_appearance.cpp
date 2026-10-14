#include "core/Hash.h"
#include "game/Appearance.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace {

using vd::u16;
using vd::u32;

// Baked packs from $VD_PACKS_DIR (CTest points it at <repo>/packs); the test skips itself without them.
struct Packs {
    vd::pack::PackReader db, textures, actors;
    bool open() {
        const char* dir = std::getenv("VD_PACKS_DIR");
        if (!dir) {
            return false;
        }
        const std::filesystem::path root(dir);
        return db.open(root / "db.vdpk").ok() && textures.open(root / "textures.vdpk").ok() &&
               actors.open(root / "actors.vdpk").ok();
    }
};

bool has(const std::vector<u16>& list, u16 value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

} // namespace

TEST_CASE("Appearance: characters, humanoid NPCs and creatures from the client tables") {
    Packs packs;
    if (!packs.open()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    vd::game::AppearanceDb db;
    REQUIRE(db.init(packs.db, packs.textures, packs.actors).ok());

    // A naked human male: race model, composed skin (base first), default geosets.
    vd::game::CharacterLook naked;
    naked.race = 1;
    naked.gender = 0;
    naked.hairStyle = 1;
    const vd::game::ActorLook look = db.character(naked);
    CHECK(look.model == vd::assetPathHash("character/human/male/humanmale.m2"));
    REQUIRE_FALSE(look.skin.empty());
    CHECK(look.skin.front().region == vd::game::SkinRegion::Base);
    CHECK(look.skin.front().texture == vd::assetPathHash("character/human/male/humanmaleskin00_00.blp"));
    CHECK(look.textures[3] != 0); // hair texture
    for (u16 geoset : {u16{0}, u16{401}, u16{501}, u16{702}, u16{1301}}) {
        CAPTURE(geoset);
        CHECK(has(look.geosets, geoset));
    }
    CHECK_FALSE(has(look.geosets, 1302)); // no robe
    CHECK(std::is_sorted(look.geosets.begin(), look.geosets.end()));

    // Dressed from what NPCs wear: a chest piece brings its textures, boots their geoset.
    vd::game::CharacterLook dressed = naked;
    dressed.items[vd::game::kSlotChest] = db.npcItems(vd::game::kSlotChest).front();
    dressed.items[vd::game::kSlotMainHand] = db.weaponDisplays().front();
    const vd::game::ActorLook clothed = db.character(dressed, true);
    CHECK(clothed.skin.size() >= look.skin.size());
    REQUIRE(clothed.attachments.size() == 1);
    CHECK(clothed.attachments[0].model != 0);

    // Deputy Willem-like NPCs: a humanoid display with a baked skin (CreatureDisplayInfoExtra).
    u32 humanoid = 0;
    for (u32 display = 1; display < 20000 && humanoid == 0; ++display) {
        const vd::game::ActorLook npc = db.creature(display);
        if (npc.model == vd::assetPathHash("character/human/male/humanmale.m2") && npc.skin.empty() &&
            npc.textures[1] != 0) {
            humanoid = display;
        }
    }
    CHECK(humanoid != 0);

    // A goblin NPC (Gelvas Grimegate, Darkmoon Faire): the display's own model, the character goblin whose body takes
    // the baked skin, not ChrRaces' creature Goblin.mdx (texture variations only: it was drawn all white).
    const vd::game::ActorLook goblin = db.creature(14854);
    CHECK(goblin.model == vd::assetPathHash("character/goblin/male/goblinmale.m2"));
    CHECK(goblin.textures[1] == vd::assetPathHash("textures/bakednpctextures/188fb664223cf2694616be0781ce4ca.blp"));

    // A wolf: creature model with its skin variation in the model's folder.
    const vd::game::ActorLook wolf = db.creature(165);
    CHECK(wolf.model == vd::assetPathHash("creature/wolf/wolf.m2"));
    CHECK(wolf.textures[5] == vd::assetPathHash("creature/wolf/wolfskintimber.blp"));
    CHECK(wolf.geosets.empty()); // every part
}
