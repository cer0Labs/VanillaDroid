// Parity pass (audio): the tables of the sounds phase 9 did not play, checked on real rows of the 1.12.1 client
// (VD_PACKS_DIR), and the rules that pick them (no packs).
#include "audio/SoundData.h"
#include "audio/SoundRules.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>

using namespace vd;
using namespace vd::audio;

namespace {

bool openDb(pack::PackReader& db) {
    const char* dir = std::getenv("VD_PACKS_DIR");
    return dir && db.open(std::filesystem::path(dir) / "db.vdpk").ok();
}

} // namespace

TEST_CASE("Sound rules: vocal errors by result, strides, landings") {
    // Casting: the power the character lacks, the cooldown by caster or not, range, targets.
    CHECK(vocalForCastFailure("NO_POWER", 0) == vocal::kNoMana);
    CHECK(vocalForCastFailure("NO_POWER", 1) == vocal::kNoRage);
    CHECK(vocalForCastFailure("NO_POWER", 3) == vocal::kNoEnergy);
    CHECK(vocalForCastFailure("NOT_READY", 0) == vocal::kSpellCooling);
    CHECK(vocalForCastFailure("NOT_READY", 1) == vocal::kAbilityCooling);
    CHECK(vocalForCastFailure("OUT_OF_RANGE", 0) == vocal::kOutOfRange);
    CHECK(vocalForCastFailure("BAD_TARGETS", 0) == vocal::kInvalidTarget);
    CHECK(vocalForCastFailure("BAD_IMPLICIT_TARGETS", 0) == vocal::kNoTarget);
    CHECK(vocalForCastFailure("ITEM_NOT_READY", 0) == vocal::kItemCooling);
    CHECK(vocalForCastFailure("NO_AMMO", 0) == vocal::kNoAmmo);
    CHECK(vocalForCastFailure("LINE_OF_SIGHT", 0) == vocal::kNone); // the original says nothing
    CHECK(vocalForCastFailure("", 0) == vocal::kNone);
    // Items, loot, vendors, the bank.
    CHECK(vocalForInventoryError("INVENTORY_FULL") == vocal::kInventoryFull);
    CHECK(vocalForInventoryError("BAG_FULL") == vocal::kBagFull);
    CHECK(vocalForInventoryError("CANT_EQUIP_LEVEL_I") == vocal::kNoEquipLevel);
    CHECK(vocalForInventoryError("CANT_DROP_SOULBOUND") == vocal::kSoulboundNoDrop);
    CHECK(vocalForInventoryError("NOT_ENOUGH_MONEY") == vocal::kNotEnoughMoney);
    CHECK(vocalForInventoryError("OK") == vocal::kNone);
    CHECK(vocalForLootError("DIDNT_KILL") == vocal::kLootDidntKill);
    CHECK(vocalForLootError("TOO_FAR") == vocal::kLootTooFar);
    CHECK(vocalForLootError("LOCKED") == vocal::kLootLocked);
    CHECK(vocalForLootError("STUNNED") == vocal::kNone);
    CHECK(vocalForBuyError("NOT_ENOUGHT_MONEY") == vocal::kNotEnoughMoney);
    CHECK(vocalForBankSlotError("INSUFFICIENT_FUNDS") == vocal::kBankCantAfford);
    CHECK(vocalForBankSlotError("OK") == vocal::kNone);
    // Footsteps: about two a cycle on foot, the mount's four hooves (about six a second at a run).
    CHECK(strideLength(7.0f, false) == doctest::Approx(2.3f));
    CHECK(strideLength(2.5f, false) == doctest::Approx(1.4f));
    CHECK(11.2f / strideLength(11.2f, true) == doctest::Approx(6.0f));
    CHECK(strideLength(1.0f, true) == doctest::Approx(0.6f));
    // Impacts: stone creatures, a player's mail and plate.
    CHECK(SoundData::impactColumn(1, 0) == 8);
    CHECK(SoundData::impactColumn(0, 3) == 1);
    CHECK(SoundData::impactColumn(0, 4) == 2);
    CHECK(SoundData::impactColumn(0, 2) == 0); // leather: flesh
    CHECK(SoundData::impactColumn(0, 0) == 0);
}

// The parity tables' field numbers (SoundData.cpp), checked on rows whose meaning their names give.
TEST_CASE("Sound tables (parity): real rows of the 1.12.1 client (VD_PACKS_DIR)") {
    pack::PackReader db;
    if (!openDb(db)) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    SoundData data;
    REQUIRE(data.init(db));

    // CreatureSoundData's last columns: 23 the loop, 24 the impact type, 27-29 the warlock pets' lines; no NPC
    // sounds (the infernal's loop, once read as its NPC sounds, gave every infernal a stranger's greeting).
    const CreatureSounds* infernal = data.creatureSounds(169); // Infernal.mdx -> CreatureSoundData 24
    REQUIRE(infernal);
    CHECK(infernal->id == 24);
    CHECK(infernal->loop == 1454); // FireElementalLoop
    CHECK(infernal->impactType == 1);
    const CreatureSounds* golem = data.creatureSounds(473); // GolemStone.mdx -> 84
    REQUIRE(golem);
    CHECK(golem->impactType == 1);
    CHECK(golem->fidget[2] == 471); // StoneGolemStand2
    const CreatureSounds* imp = data.creatureSounds(268); // Grell.mdx (the imp) -> 11
    REQUIRE(imp);
    CHECK(imp->petAttack == 9097); // A_IMP_KILL
    CHECK(imp->petOrder == 9098);  // A_IMP_ORDER
    CHECK(imp->petDismiss == 9096); // A_Imp_Dismiss
    // Phase 14: four fidgets (14-17) and four custom attacks (18-21), as WoW.exe reads them for $FD1..$FD4 and
    // $AH0..$AH3: the wolf's columns 18-21 all name WolfCustomAttack0, its model's $AH0-$AH2 play them.
    const CreatureSounds* wolf = data.creatureSounds(161); // Wolf.mdx -> 43
    REQUIRE(wolf);
    CHECK(wolf->fidget[0] == 1018); // WolfFidget2
    CHECK(wolf->fidget[3] == 0);
    CHECK(wolf->customAttack[0] == 3176); // WolfCustomAttack0
    CHECK(wolf->customAttack[2] == 3176);
    CHECK(wolf->customAttack[3] == 3176);
    CHECK(wolf->jumpStart == 0); // empty in 1.12
    // NPC sounds come from CreatureDisplayInfo only: a human male citizen's (display 3167 -> NPCSounds 48).
    const NpcSounds* citizen = data.npcSounds(3167);
    REQUIRE(citizen);
    CHECK(citizen->greeting == 5971);
    CHECK(citizen->farewell == 5972);
    CHECK(data.npcSounds(169) == nullptr);

    // The character's body: splashes, exploration.
    CHECK(data.splashSound(1) == 1096);        // CharacterSplashSoundMedium
    CHECK(data.splashSound(3) == 1090);        // CharacterSplashSoundSmall (dwarves)
    CHECK(data.explorationSound(1) == 4140);   // HumanExploration
    CHECK(data.explorationSound(2) == 4141);   // OrcExploration

    // Deaths: a medium body (the human male, size class 1) on grass (TerrainType sound 6), into water.
    CHECK(data.sizeClass(49) == 1);
    CHECK(data.deathThud(49, 6, false) == 913);  // DeathThudMediumGrass
    CHECK(data.deathThud(49, 3, false) == 915);  // DeathThudMediumStone
    CHECK(data.deathThud(49, 6, true) == 1267);  // DeathThudWaterMedium
    CHECK(data.deathThud(49, 99, false) == 912); // unknown ground: dirt

    // The environment: drowning, lava, slime and fire have a kit's sound; a fall has none (DamageFalling).
    CHECK(data.environmentSound(1) == 5736); // Drowning
    CHECK(data.environmentSound(2) == 0);
    CHECK(data.environmentSound(3) == 1484); // Fire Cast
    CHECK(data.environmentSound(4) == 3373);
    CHECK(data.environmentSound(99) == 0);
    REQUIRE(data.entryByName("DamageFalling"));
    CHECK(data.entryByName("DamageFalling")->id == 3542);

    // Liquids: SoundWaterType by kind and flow.
    CHECK(data.waterSound(0) == 1111);    // RiverStill
    CHECK(data.waterSound(0, 8) == 1113); // RiverFast
    CHECK(data.waterSound(1) == 1114);    // Ocean
    CHECK(data.waterSound(2) == 3072);    // LavaPoolLoop
    CHECK(data.waterSound(3) == 3880);    // SlimeLoop

    // Items: ItemDisplayInfo's group sound index -> ItemGroupSounds (pick up, put down).
    const ItemSounds robe = data.itemSounds(220); // INV_Robe_02: cloth and leather
    CHECK(robe.pickup == 1185);                   // PickUpCloth_Leather
    CHECK(robe.drop == 1202);                     // PutDownClothLeather
    const ItemSounds axe = data.itemSounds(453);  // Axe_1H_Flint_A_01: small metal
    CHECK(axe.pickup == 1191);
    CHECK(axe.drop == 1213);
    CHECK(data.itemSounds(362).pickup == 1183);   // a bag: PickUpBag
    CHECK(data.itemSounds(0).pickup == 0);

    // Weapons drawn and put away: metal and wooden swords, a shield whatever its material.
    CHECK(data.sheatheSound(2, 7, 1, true) == 698);  // SHEATHINGMETALWEAPONSHEATHE
    CHECK(data.sheatheSound(2, 7, 1, false) == 700); // SHEATHINGMETALWEAPONUNSHEATHE
    CHECK(data.sheatheSound(2, 10, 2, true) == 697); // a wooden staff
    CHECK(data.sheatheSound(4, 6, 1, true) == 696);  // SHEATHINGSHIELDSHEATHE
    CHECK(data.sheatheSound(4, 6, 2, false) == 701);

    // Game objects: a chest opens and closes; torches, campfires and cauldrons loop.
    const ObjectSounds* chest = data.objectSounds(10); // Chest01.mdx
    REQUIRE(chest);
    CHECK(chest->open == 1277);  // ChestOpen
    CHECK(chest->close == 1278); // ChestClose
    CHECK(chest->ambient == 0);
    REQUIRE(data.objectSounds(325)); // G_Torch01.mdx
    CHECK(data.objectSounds(325)->ambient == 4694); // TorchLoop (its loop column)
    REQUIRE(data.objectSounds(216)); // Cauldron.mdx
    CHECK(data.objectSounds(216)->ambient == 3379); // CauldronLoop (its custom0 column)
    CHECK(data.objectSounds(394)->open == 3360);    // DeadmineDoorOpen
    CHECK((data.objectSounds(1) == nullptr || data.objectSounds(1)->ambient == 0));

    // The rows the client's code names.
    REQUIRE(data.zoneMusicByName("GhostMusic (DONOTRENAME)"));
    CHECK(data.zoneMusicByName("GhostMusic (DONOTRENAME)")->id == 36);
    CHECK(data.zoneMusicByName("NoSuchSet") == nullptr);
    CHECK(data.entryByName("Underwater (DONOTRENAME)")->id == 4209);
    CHECK(data.entryByName("Ghost (DONOTRENAME)")->id == 4160);
    CHECK(data.entryByName("(DONOTRENAME)Combat Miss 1H")->id == 7080);
    CHECK(data.entryByName("(DONOTRENAME)Combat Miss 2H")->id == 7081);
    CHECK(data.entryByName("(DONOTRENAME)AbsorbGetHit")->id == 3334);
    CHECK((data.entry(3244)->flags & SoundEntry::kFlagLoop) != 0); // CampFireLargeLoop
    CHECK((data.entry(1277)->flags & SoundEntry::kFlagLoop) == 0); // ChestOpen

    // VocalUISounds: the enum values the rules use say what their names say (the human's lines).
    CHECK(data.vocalError(vocal::kInventoryFull, 1, 0) == 1875); // HumanMale_InventoryFull
    CHECK(data.vocalError(vocal::kNoMana, 1, 0) == 1885);        // HumanMale_CantCastNoMana
    CHECK(data.vocalError(vocal::kOutOfRange, 1, 1) == 2037);    // HumanFemale_CantCastOutOfRange
    CHECK(data.vocalError(vocal::kNoRage, 1, 0) == 2626);        // HumanMale_NoRage
    CHECK(data.vocalError(vocal::kNoEnergy, 1, 0) == 2627);      // HumanMale_NoEnergy
    CHECK(data.vocalError(vocal::kSpellCooling, 1, 0) == 1883);  // HumanMale_SpellCooling
    CHECK(data.vocalError(vocal::kAbilityCooling, 1, 0) == 1917); // HumanMale_CantUseAbilityCooling
    CHECK(data.vocalError(vocal::kTargetTooFar, 1, 0) == 1881);  // HumanMale_TargetTooFar
    CHECK(data.vocalError(vocal::kInvalidTarget, 1, 0) == 1882); // HumanMale_InvalidTarget
    CHECK(data.vocalError(vocal::kNoTarget, 1, 0) == 1912);      // HumanMale_CantCastGenericNoTarg
    CHECK(data.vocalError(vocal::kBagFull, 1, 0) == 1897);       // HumanMale_CantPutBagFull
    CHECK(data.vocalError(vocal::kLootDidntKill, 1, 0) == 1899); // HumanMale_CantLootDidntKill
    CHECK(data.vocalError(vocal::kLootTooFar, 1, 0) == 1903);    // HumanMale_CantLootTooFar
    CHECK(data.vocalError(vocal::kNotEnoughMoney, 1, 0) == 1908); // HumanMale_CantAffordNotEnoughMo
    CHECK(data.vocalError(vocal::kTaxiNoMoney, 1, 0) == 1921);   // HumanMale_CantUseTaxiNoMoney
    CHECK(data.vocalError(vocal::kNoAmmo, 1, 1) == 2581);        // HumanFemale_noammo
}
