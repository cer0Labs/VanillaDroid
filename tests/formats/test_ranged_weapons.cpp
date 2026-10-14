// Phase 13 (combat): weapons where WoW.exe 1.12.1 attaches them (0x47A0C0 / 0x47A070): the ranged one only drawn
// (sheath state 2), a bow in the left hand and the "right" ranged weapons (guns, crossbows, wands, thrown) in the
// right; staves (sheath type 2) on the large weapon points 30 / 31; and the race models have those points.
#include "core/Hash.h"
#include "formats/M2.h"
#include "game/Appearance.h"
#include "pack/PackReader.h"

#include "SharedMpq.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

using vd::u32;
using vd::u8;
namespace attach = vd::game::attach;

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

// The first weapon display whose model file starts with `prefix` (lower case).
u32 displayOf(const vd::game::AppearanceDb& db, const char* prefix) {
    for (const u32 display : db.weaponDisplays()) {
        std::string model(db.itemModel(display));
        std::transform(model.begin(), model.end(), model.begin(), [](char c) {
            return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        });
        if (model.rfind(prefix, 0) == 0) {
            return display;
        }
    }
    return 0;
}

// Where the look hangs the item of `slot` (~0u: nowhere).
u32 pointOf(const vd::game::ActorLook& look, vd::game::EquipSlot slot) {
    for (const vd::game::AttachedModel& a : look.attachments) {
        if (a.slot == slot) {
            return a.attachment;
        }
    }
    return ~0u;
}

} // namespace

TEST_CASE("Ranged weapons: in the hands only drawn, the bow in the left") {
    Packs packs;
    if (!packs.open()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    vd::game::AppearanceDb db;
    REQUIRE(db.init(packs.db, packs.textures, packs.actors).ok());
    vd::game::CharacterLook dwarf;
    dwarf.race = 3;
    const struct {
        const char* prefix;
        u8 inventoryType;
        u32 hand;
    } kRanged[] = {{"bow_1h", vd::game::kInventoryRanged, attach::kHandLeft},
                   {"firearm_", vd::game::kInventoryRangedRight, attach::kHandRight},
                   {"bow_2h_crossbow", vd::game::kInventoryRangedRight, attach::kHandRight},
                   {"wand_", vd::game::kInventoryRangedRight, attach::kHandRight},
                   {"thrown_", vd::game::kInventoryThrown, attach::kHandRight}};
    for (const auto& r : kRanged) {
        CAPTURE(r.prefix);
        vd::game::CharacterLook look = dwarf;
        look.items[vd::game::kSlotRanged] = displayOf(db, r.prefix);
        REQUIRE(look.items[vd::game::kSlotRanged] != 0);
        look.inventoryTypes[vd::game::kSlotRanged] = r.inventoryType;
        CHECK(pointOf(db.character(look, true, true), vd::game::kSlotRanged) == r.hand);
        // Put away: no 1.12 ranged item has a sheath type, so it is not shown (sheath state 0 or 1).
        CHECK(pointOf(db.character(look, true, false), vd::game::kSlotRanged) == ~0u);
        CHECK(pointOf(db.character(look, false, false), vd::game::kSlotRanged) == ~0u);
    }
    // Drawing the ranged weapon puts the melee one away: a sword on the hip, a staff across the back (30).
    vd::game::CharacterLook armed = dwarf;
    armed.items[vd::game::kSlotRanged] = displayOf(db, "bow_1h");
    armed.inventoryTypes[vd::game::kSlotRanged] = vd::game::kInventoryRanged;
    armed.items[vd::game::kSlotMainHand] = displayOf(db, "sword_1h");
    armed.inventoryTypes[vd::game::kSlotMainHand] = 13;
    armed.sheaths[vd::game::kSlotMainHand] = 3;
    CHECK(pointOf(db.character(armed, false, true), vd::game::kSlotMainHand) == attach::kHipLeft);
    CHECK(pointOf(db.character(armed, false, false), vd::game::kSlotMainHand) == attach::kHandRight);
    armed.items[vd::game::kSlotMainHand] = displayOf(db, "stave_2h");
    armed.inventoryTypes[vd::game::kSlotMainHand] = 17;
    armed.sheaths[vd::game::kSlotMainHand] = 2;
    CHECK(pointOf(db.character(armed, true, false), vd::game::kSlotMainHand) == attach::kLargeWeaponLeft);
    // A creature shooting: its virtual ranged item (UNIT_VIRTUAL_ITEM_SLOT_DISPLAY + 2) in its hands.
    const u32 bow = displayOf(db, "bow_1h");
    for (u32 display = 1; display < 5000; ++display) {
        const vd::game::ActorLook humanoid = db.creature(display);
        if (humanoid.model == vd::assetPathHash("character/human/male/humanmale.m2")) {
            const vd::game::ActorLook archer =
                db.creature(display, {0, 0, bow}, {0, 0, vd::game::kInventoryRanged}, true, true);
            CHECK(pointOf(archer, vd::game::kSlotRanged) == attach::kHandLeft);
            CHECK(pointOf(db.creature(display, {0, 0, bow}, {0, 0, vd::game::kInventoryRanged}, true, false),
                          vd::game::kSlotRanged) == ~0u);
            break;
        }
    }
}

TEST_CASE("Race models have the sheath points 1.12 uses (26, 27, 28, 30, 31, 32, 33)") {
    VD_REQUIRE_MPQ(mpq);
    for (const char* race : {"Human", "Orc", "Dwarf", "NightElf", "Scourge", "Tauren", "Gnome", "Troll"}) {
        for (const char* gender : {"Male", "Female"}) {
            const std::string path = std::string("Character\\") + race + "\\" + gender + "\\" + race + gender + ".m2";
            CAPTURE(path);
            vd::Result<std::vector<u8>> bytes = mpq->read(path);
            REQUIRE(bytes.ok());
            vd::Result<vd::formats::M2Model> model = vd::formats::parseM2(bytes.value());
            REQUIRE(model.ok());
            for (const u32 id : {26u, 27u, 28u, 30u, 31u, 32u, 33u, 1u, 2u}) {
                CAPTURE(id);
                CHECK(std::any_of(model.value().attachments.begin(), model.value().attachments.end(),
                                  [&](const vd::formats::M2Attachment& a) { return a.id == id; }));
            }
        }
    }
}
