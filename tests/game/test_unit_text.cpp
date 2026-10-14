// What the interface says about a unit (parity pass: game/UnitInfo's name colours, game/UnitText): the colour
// classes of TargetFrame_CheckFaction / GameTooltip_UnitColor with the PvP rules, the level the target frame
// shows (the skull of UnitLevel -1) and its colour (TargetFrame_CheckLevel), the PvP badge, and the unit
// tooltip's lines (GameTooltip:SetUnit) from the update fields.
#include "TestData.h"
#include "core/Text.h"
#include "game/CharacterNames.h"
#include "game/Entities.h"
#include "game/Reactions.h"
#include "game/UnitInfo.h"
#include "game/UnitText.h"
#include "net/UpdateFields.gen.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace vd;

namespace {

namespace uf = net::uf;
using game::NameColor;
using game::ObjectType;

// FactionTemplate.dbc rows as the client has them (own group, friends, enemies; 1 players, 2 Alliance, 4
// Horde, 8 monsters).
constexpr u32 kAlliance = 1, kHorde = 2, kStormwind = 11, kMonster = 14, kNeutral = 7;

game::Reactions reactions() {
    game::Reactions r;
    r.setTemplate(kAlliance, 1, 0x3, 0x2, 0xC);
    r.setTemplate(kHorde, 2, 0x5, 0x4, 0xA);
    r.setTemplate(kStormwind, 72, 0x2, 0x2, 0xC);
    r.setTemplate(kMonster, 14, 0x8, 0x0, 0x1);
    r.setTemplate(kNeutral, 7, 0x0, 0x0, 0x0);
    return r;
}

game::Entity unit(u64 guid, ObjectType type, u32 level, u32 faction, u32 unitFlags = 0) {
    game::Entity e;
    e.guid = guid;
    e.type = type;
    e.values.assign(type == ObjectType::Player ? uf::PLAYER_END : uf::UNIT_END, 0);
    e.values[uf::UNIT_FIELD_LEVEL] = level;
    e.values[uf::UNIT_FIELD_FACTIONTEMPLATE] = faction;
    e.values[uf::UNIT_FIELD_FLAGS] = unitFlags;
    e.values[uf::UNIT_FIELD_HEALTH] = 100;
    e.values[uf::UNIT_FIELD_MAXHEALTH] = 100;
    return e;
}

game::Entity player(u64 guid, u32 level, u32 faction, u8 race, bool pvp = false) {
    game::Entity e = unit(guid, ObjectType::Player, level, faction, pvp ? game::kUnitFlagPvp : 0);
    e.values[uf::UNIT_FIELD_BYTES_0] = race;
    return e;
}

void setGuid(game::Entity& e, u16 index, u64 guid) {
    e.values[index] = static_cast<u32>(guid);
    e.values[static_cast<u16>(index + 1)] = static_cast<u32>(guid >> 32);
}

std::vector<std::string> texts(const game::UnitTooltip& tip) {
    std::vector<std::string> out;
    for (const game::TooltipLine& line : tip.lines()) {
        out.emplace_back(line.left);
    }
    return out;
}

} // namespace

TEST_CASE("Unit text: name colours follow TargetFrame_CheckFaction's PvP rules and the tapped gray") {
    const game::Reactions r = reactions();
    const game::Entity me = player(1, 30, kAlliance, 1);
    const game::Entity meFlagged = player(1, 30, kAlliance, 1, true);

    // Players of the other side: who may fight whom.
    const game::Entity orc = player(2, 30, kHorde, 2);
    const game::Entity orcFlagged = player(3, 30, kHorde, 2, true);
    CHECK(game::unitNameColor(me, orc, &r) == NameColor::FriendlyPlayer); // neither may: blue (was red)
    CHECK(game::unitNameColor(me, orcFlagged, &r) == NameColor::Neutral); // only I may: yellow
    CHECK(game::unitNameColor(meFlagged, orc, &r) == NameColor::FriendlyPlayer); // only they may: blue
    CHECK(game::unitNameColor(meFlagged, orcFlagged, &r) == NameColor::Hostile); // both: red
    CHECK(game::mayFight(me, orcFlagged, &r));
    CHECK_FALSE(game::mayFight(me, orc, &r));
    CHECK(game::canAttack(me, orcFlagged, &r));
    CHECK_FALSE(game::canAttack(me, orc, &r));

    // Players of the same side: green when flagged, blue otherwise; myself too.
    CHECK(game::unitNameColor(me, player(4, 30, kAlliance, 3, true), &r) == NameColor::Friendly);
    CHECK(game::unitNameColor(me, player(5, 30, kAlliance, 3), &r) == NameColor::FriendlyPlayer);
    CHECK(game::unitNameColor(me, me, &r) == NameColor::FriendlyPlayer);
    CHECK(game::unitNameColor(meFlagged, meFlagged, &r) == NameColor::Friendly);

    // Free-for-all areas: both flagged, whatever the side.
    game::Entity ffaMe = me, ffaFriend = player(6, 30, kAlliance, 3);
    ffaMe.values[uf::PLAYER_FLAGS] = game::kPlayerFlagFreeForAllPvp;
    ffaFriend.values[uf::PLAYER_FLAGS] = game::kPlayerFlagFreeForAllPvp;
    CHECK(game::unitNameColor(ffaMe, ffaFriend, &r) == NameColor::Hostile);
    CHECK(game::unitNameColor(me, ffaFriend, &r) == NameColor::FriendlyPlayer);

    // A duel: both ways attackable.
    game::Entity duelMe = me, duelFriend = player(7, 30, kAlliance, 3);
    setGuid(duelMe, uf::PLAYER_DUEL_ARBITER, 0xF11000000000ABCDull);
    setGuid(duelFriend, uf::PLAYER_DUEL_ARBITER, 0xF11000000000ABCDull);
    duelMe.values[uf::PLAYER_DUEL_TEAM] = 1;
    duelFriend.values[uf::PLAYER_DUEL_TEAM] = 2;
    CHECK(game::unitNameColor(duelMe, duelFriend, &r) == NameColor::Hostile);

    // What players control carries their side and flag: an unflagged orc's pet is blue, a flagged one's red
    // to me.
    game::Entity pet = unit(8, ObjectType::Unit, 30, kHorde, game::kUnitFlagPlayerControlled);
    CHECK(game::unitNameColor(me, pet, &r) == NameColor::FriendlyPlayer);
    CHECK_FALSE(game::canAttack(me, pet, &r));
    pet.values[uf::UNIT_FIELD_FLAGS] |= game::kUnitFlagPvp;
    CHECK(game::unitNameColor(meFlagged, pet, &r) == NameColor::Hostile);

    // Creatures: their reaction, gray when someone else tapped them.
    game::Entity wolf = unit(9, ObjectType::Unit, 5, kMonster);
    CHECK(game::unitNameColor(me, wolf, &r) == NameColor::Hostile);
    wolf.values[uf::UNIT_DYNAMIC_FLAGS] = game::kDynFlagTapped;
    CHECK(game::unitNameColor(me, wolf, &r) == NameColor::Tapped);
    wolf.values[uf::UNIT_DYNAMIC_FLAGS] = game::kDynFlagTapped | game::kDynFlagTappedByPlayer;
    CHECK(game::unitNameColor(me, wolf, &r) == NameColor::Hostile);
    CHECK(game::unitNameColor(me, unit(10, ObjectType::Unit, 5, kStormwind), &r) == NameColor::Friendly);
    CHECK(game::unitNameColor(me, unit(11, ObjectType::Unit, 5, kNeutral), &r) == NameColor::Neutral);
    CHECK(game::unitNameColor(me, unit(12, ObjectType::Unit, 5, 999), &r) == NameColor::Neutral); // unknown

    // How each view paints the classes: UnitReactionColor behind the target's name, FACTION_BAR_COLORS in
    // tooltips.
    CHECK(game::targetNameBackground(NameColor::Hostile) == game::tooltipColor(255, 0, 0));
    CHECK(game::targetNameBackground(NameColor::Neutral) == game::tooltipColor(255, 255, 0));
    CHECK(game::targetNameBackground(NameColor::Friendly) == game::tooltipColor(0, 255, 0));
    CHECK(game::targetNameBackground(NameColor::FriendlyPlayer) == game::tooltipColor(0, 0, 255));
    CHECK(game::targetNameBackground(NameColor::Tapped) == game::tooltipColor(128, 128, 128));
    CHECK(game::tooltipNameColor(NameColor::Hostile) == game::tooltipColor(204, 77, 56));
    CHECK(game::tooltipNameColor(NameColor::Friendly) == game::tooltipColor(0, 153, 26));
    CHECK(game::tooltipNameColor(NameColor::FriendlyPlayer) == game::kTooltipWhite);
}

TEST_CASE("Unit text: the target frame's level, skull and level colour (TargetFrame_CheckLevel, UnitLevel)") {
    const game::Reactions r = reactions();
    const game::Entity me = player(1, 12, kAlliance, 1);

    // Friendly units always show their level, in gold: a level 55 guard to a level 12 player (the phone's
    // bug).
    const game::Entity guard = unit(2, ObjectType::Unit, 55, kStormwind, game::kUnitFlagPvp);
    CHECK(game::shownLevel(me, guard, 0, &r) == 55);
    CHECK(game::levelColor(me, guard, &r) == game::kTooltipGold);
    const game::Entity friendlyPlayer = player(3, 60, kAlliance, 3);
    CHECK(game::shownLevel(me, friendlyPlayer, 0, &r) == 60);
    CHECK(game::levelColor(me, friendlyPlayer, &r) == game::kTooltipGold);

    // Hostile or attackable units 10 or more levels above: "??" (the skull).
    CHECK(game::shownLevel(me, unit(4, ObjectType::Unit, 21, kMonster), 0, &r) == 21);
    CHECK(game::shownLevel(me, unit(5, ObjectType::Unit, 22, kMonster), 0, &r) == -1);
    CHECK(game::shownLevel(me, unit(6, ObjectType::Unit, 25, kNeutral), 0, &r) == -1); // neutral, attackable
    // An unflagged enemy player is not attackable, but hostile all the same.
    CHECK(game::shownLevel(me, player(7, 22, kHorde, 2), 0, &r) == -1);
    CHECK(game::shownLevel(me, player(8, 21, kHorde, 2), 0, &r) == 21);
    // World bosses always; a dead enemy keeps its level and colour.
    CHECK(game::shownLevel(me, unit(9, ObjectType::Unit, 5, kMonster), 3, &r) == -1);
    CHECK(game::shownLevel(me, unit(10, ObjectType::Unit, 60, kStormwind), 3, &r) == -1);
    game::Entity corpse = unit(11, ObjectType::Unit, 17, kMonster);
    corpse.values[uf::UNIT_FIELD_HEALTH] = 0;
    CHECK(game::shownLevel(me, corpse, 0, &r) == 17);
    CHECK(game::levelColor(me, corpse, &r) == game::difficultyColor(game::Difficulty::Impossible));

    // GetDifficultyColor with the server's gray level (Formulas.h GetGrayLevel: 6 at level 12, 35 at 45).
    using D = game::Difficulty;
    CHECK(game::levelDifficulty(17, 12) == D::Impossible);
    CHECK(game::levelDifficulty(15, 12) == D::VeryDifficult);
    CHECK(game::levelDifficulty(10, 12) == D::Difficult);
    CHECK(game::levelDifficulty(7, 12) == D::Standard);
    CHECK(game::levelDifficulty(6, 12) == D::Trivial);
    CHECK(game::levelDifficulty(1, 5) == D::Standard); // nothing is gray up to level 5
    CHECK(game::levelDifficulty(36, 45) == D::Standard);
    CHECK(game::levelDifficulty(35, 45) == D::Trivial);
    CHECK(game::difficultyColor(D::Difficult) == game::tooltipColor(255, 255, 0));
    CHECK(game::levelColor(me, unit(12, ObjectType::Unit, 13, kMonster), &r) ==
          game::difficultyColor(D::Difficult));
    // An unflagged enemy player can't be attacked: gold.
    CHECK(game::levelColor(me, player(13, 13, kHorde, 2), &r) == game::kTooltipGold);

    // The art: elites, rare elites and world bosses Elite, rares Rare (TargetFrame_CheckClassification).
    CHECK(game::classification(1) == game::Classification::Elite);
    CHECK(game::classification(2) == game::Classification::RareElite);
    CHECK(game::classification(3) == game::Classification::WorldBoss);
    CHECK(game::classification(4) == game::Classification::Rare);
    CHECK(game::classification(9) == game::Classification::Normal);
    CHECK(game::isElite(2));
    CHECK_FALSE(game::isElite(4));
}

TEST_CASE("Unit text: the PvP badge (PlayerFrame_UpdatePvPStatus, TargetFrame_CheckFaction)") {
    const game::Reactions r = reactions();
    CHECK(game::pvpBadge(player(1, 10, kAlliance, 1), &r) == game::PvpBadge::None);
    CHECK(game::pvpBadge(player(1, 10, kAlliance, 1, true), &r) == game::PvpBadge::Alliance);
    CHECK(game::pvpBadge(player(2, 10, kHorde, 2, true), &r) == game::PvpBadge::Horde);
    CHECK(game::pvpBadge(player(2, 10, kHorde, 5, true), nullptr) ==
          game::PvpBadge::Horde); // by race, no table
    CHECK(game::pvpBadge(unit(3, ObjectType::Unit, 55, kStormwind, game::kUnitFlagPvp), &r) ==
          game::PvpBadge::Alliance);
    CHECK(game::pvpBadge(unit(4, ObjectType::Unit, 5, kMonster, game::kUnitFlagPvp), &r) ==
          game::PvpBadge::None);
    game::Entity ffa = player(5, 10, kAlliance, 1);
    ffa.values[uf::PLAYER_FLAGS] = game::kPlayerFlagFreeForAllPvp;
    CHECK(game::pvpBadge(ffa, &r) == game::PvpBadge::FreeForAll);
}

TEST_CASE("Unit text: the unit tooltip's lines from the update fields (GameTooltip:SetUnit)") {
    if (!test::loadTexts()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    const game::Reactions r = reactions();
    const game::Entity me = player(1, 12, kAlliance, 1);
    game::UnitTooltip tip;

    // An elite creature: its name in the hostile colour, "Level 11 Elite Humanoid".
    game::UnitNames hogger;
    hogger.name = "Hogger";
    hogger.rank = 1;
    hogger.creatureType = "Humanoid";
    game::buildUnitTooltip(me, unit(2, ObjectType::Unit, 11, kMonster), hogger, &r, tip);
    CHECK(texts(tip) == std::vector<std::string>{"Hogger", "Level 11 Elite Humanoid"});
    CHECK(tip.lines()[0].leftColor == game::tooltipNameColor(NameColor::Hostile));
    CHECK(tip.lines()[1].leftColor == game::kTooltipWhite);

    // A player with a guild, flagged: green, "<guild>", "Level 14 Night Elf Priest", "PvP".
    game::UnitNames aeryn;
    aeryn.name = "Aeryn";
    aeryn.guild = "Los Errantes";
    aeryn.race = "Night Elf";
    aeryn.cls = "Priest";
    game::buildUnitTooltip(me, player(3, 14, kAlliance, 4, true), aeryn, &r, tip);
    CHECK(texts(tip) ==
          std::vector<std::string>{"Aeryn", "<Los Errantes>", "Level 14 Night Elf Priest", "PvP"});
    CHECK(tip.lines()[0].leftColor == game::tooltipNameColor(NameColor::Friendly));

    // An enemy player far above, unflagged: white, "Level ?? Orc Warrior". With a PvP rank: its title first.
    game::UnitNames ugha;
    ugha.name = "Ugha";
    ugha.race = "Orc";
    ugha.cls = "Warrior";
    game::Entity orc = player(4, 40, kHorde, 2);
    game::buildUnitTooltip(me, orc, ugha, &r, tip);
    CHECK(texts(tip) == std::vector<std::string>{"Ugha", "Level ?? Orc Warrior"});
    CHECK(tip.lines()[0].leftColor == game::kTooltipWhite);
    orc.values[uf::PLAYER_BYTES_3] = 7u << 24; // PLAYER_BYTES_3_OFFSET_HONOR_RANK: Sergeant
    game::buildUnitTooltip(me, orc, ugha, &r, tip);
    CHECK(texts(tip)[0] == "Sergeant Ugha");

    // A world boss: "Level ?? Dragonkin". A creature without a type: "Level 5".
    game::UnitNames boss;
    boss.name = "Onyxia";
    boss.rank = 3;
    boss.creatureType = "Dragonkin";
    game::buildUnitTooltip(me, unit(5, ObjectType::Unit, 63, kMonster), boss, &r, tip);
    CHECK(texts(tip) == std::vector<std::string>{"Onyxia", "Level ?? Dragonkin"});
    game::UnitNames plain;
    plain.name = "Trigger";
    game::buildUnitTooltip(me, unit(6, ObjectType::Unit, 5, kNeutral), plain, &r, tip);
    CHECK(texts(tip) == std::vector<std::string>{"Trigger", "Level 5"});

    // A skinnable corpse: "Level 7 Corpse", "Skinnable".
    game::UnitNames wolfNames;
    wolfNames.name = "Timber Wolf";
    wolfNames.creatureType = "Beast";
    game::Entity wolf = unit(7, ObjectType::Unit, 7, kMonster, game::kUnitFlagSkinnable);
    wolf.values[uf::UNIT_FIELD_HEALTH] = 0;
    game::buildUnitTooltip(me, wolf, wolfNames, &r, tip);
    CHECK(texts(tip) == std::vector<std::string>{"Timber Wolf", "Level 7 Corpse", "Skinnable"});

    // A friendly NPC with its title, flagged as the city's guards are; a creature tapped by someone else is
    // gray.
    game::UnitNames farley;
    farley.name = "Innkeeper Farley";
    farley.subname = "Innkeeper";
    farley.creatureType = "Humanoid";
    game::buildUnitTooltip(me, unit(8, ObjectType::Unit, 30, kStormwind, game::kUnitFlagPvp), farley, &r,
                           tip);
    CHECK(texts(tip) ==
          std::vector<std::string>{"Innkeeper Farley", "Innkeeper", "Level 30 Humanoid", "PvP"});
    CHECK(tip.lines()[0].leftColor == game::tooltipNameColor(NameColor::Friendly));
    game::Entity kobold = unit(9, ObjectType::Unit, 2, kMonster);
    kobold.values[uf::UNIT_DYNAMIC_FLAGS] = game::kDynFlagTapped;
    hogger.name = "Kobold Vermin";
    hogger.rank = 0;
    game::buildUnitTooltip(me, kobold, hogger, &r, tip);
    CHECK(texts(tip) == std::vector<std::string>{"Kobold Vermin", "Level 2 Humanoid"});
    CHECK(tip.lines()[0].leftColor == game::tooltipNameColor(NameColor::Tapped));

    // A hunter's pet: "<owner>'s Pet"; a name not known yet: "Unknown".
    game::UnitNames petNames;
    petNames.name = "Wolfie";
    petNames.owner = "Arthel";
    petNames.creatureType = "Beast";
    game::Entity pet = unit(10, ObjectType::Unit, 12, kAlliance, game::kUnitFlagPlayerControlled);
    setGuid(pet, uf::UNIT_FIELD_SUMMONEDBY, 1);
    game::buildUnitTooltip(me, pet, petNames, &r, tip);
    CHECK(texts(tip) == std::vector<std::string>{"Wolfie", "Arthel's Pet", "Level 12 Beast"});
    game::buildUnitTooltip(me, unit(11, ObjectType::Unit, 3, kMonster), {}, &r, tip);
    CHECK(texts(tip) == std::vector<std::string>{"Unknown", "Level 3"});

    // Not a unit: nothing.
    game::Entity chest;
    chest.type = ObjectType::GameObject;
    game::buildUnitTooltip(me, chest, plain, &r, tip);
    CHECK(tip.empty());
    text::unload();
}

TEST_CASE("Unit text: our keys and the client's in ui.vdpk (English, Spanish); CreatureType.dbc names") {
    if (!test::loadTexts()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    constexpr const char* kKeys[] = {"VD_UNIT_GUILD",
                                     "VD_UNIT_CLEAR_TARGET",
                                     "VD_UNIT_MENU_AFK",
                                     "VD_UNIT_MENU_DND",
                                     "VD_REST_STATE_RESTED",
                                     "VD_REST_STATE_NORMAL",
                                     "PLAYER_LEVEL",
                                     "UNIT_TYPE_LEVEL_TEMPLATE",
                                     "UNIT_TYPE_PLUS_LEVEL_TEMPLATE",
                                     "UNIT_TYPE_LETHAL_LEVEL_TEMPLATE",
                                     "UNIT_LEVEL_TEMPLATE",
                                     "UNIT_PLUS_LEVEL_TEMPLATE",
                                     "UNIT_LETHAL_LEVEL_TEMPLATE",
                                     "UNIT_LEVEL_DEAD_TEMPLATE",
                                     "UNIT_LETHAL_LEVEL_DEAD_TEMPLATE",
                                     "UNIT_SKINNABLE",
                                     "PVP_ENABLED",
                                     "UNITNAME_TITLE",
                                     "UNITNAME_TITLE_PET",
                                     "UNITNAME_TITLE_CHARM",
                                     "UNIT_PVP_NAME",
                                     "UNKNOWNOBJECT",
                                     "PVPFFA",
                                     "FACTION_ALLIANCE",
                                     "FACTION_HORDE",
                                     "NEWBIE_TOOLTIP_PVPFFA",
                                     "NEWBIE_TOOLTIP_ALLIANCE",
                                     "NEWBIE_TOOLTIP_HORDE",
                                     "EXHAUST_TOOLTIP1",
                                     "EXHAUST_TOOLTIP3",
                                     "LOOT_METHOD",
                                     "LOOT_THRESHOLD",
                                     "LOOT_PROMOTE",
                                     "PARTY_LEAVE",
                                     "CANCEL",
                                     "DEAD"};
    for (const char* key : kKeys) {
        INFO(key);
        CHECK(text::has(key));
    }
    char buffer[256];
    const std::string_view rested = text::format(buffer, "EXHAUST_TOOLTIP1", text::get("VD_REST_STATE_RESTED"), 200);
    CHECK(rested.starts_with("|cffffd200Rested|r\n|cffffffff200% "));
    CHECK(rested.ends_with("|r"));
    REQUIRE(text::load(std::filesystem::path(std::getenv("VD_PACKS_DIR")) / "ui.vdpk", "esES"));
    CHECK(text::get("VD_UNIT_CLEAR_TARGET") == "Quitar objetivo");
    CHECK(text::format(buffer, "UNIT_TYPE_PLUS_LEVEL_TEMPLATE", 11, "Humanoide") ==
          (test::esESClientTexts() ? "Nivel 11 Élite Humanoide" : "Nivel 11 Humanoide élite")); // phase 21
    for (const char* key : kKeys) {
        INFO(key);
        CHECK(text::has(key));
    }
    text::unload();

    pack::PackReader db;
    const char* dir = std::getenv("VD_PACKS_DIR");
    if (!dir || !db.open(std::filesystem::path(dir) / "db.vdpk").ok()) {
        return;
    }
    REQUIRE(game::loadCharacterNames(db));
    CHECK(game::creatureTypeName(1) == "Beast");
    CHECK(game::creatureTypeName(2) == "Dragonkin");
    CHECK(game::creatureTypeName(7) == "Humanoid");
    CHECK(game::creatureTypeName(game::kCreatureTypeNotSpecified) == "Not specified");
    CHECK(game::creatureTypeName(99).empty());
    game::unloadCharacterNames();
}
