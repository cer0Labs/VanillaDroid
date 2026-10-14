// Phase 15 (options): the macros as WoW.exe 1.12.1 keeps them (game/Macros): two tabs of MAX_MACROS sorted by name,
// ids that never change (the character's from 0x1000001), the action button's packing (ACTION_BUTTON_MACRO 0x40 /
// _CMACRO 0x41, read back as packed & 0xBFFFFFFF), macros-cache.txt's format, the first /cast line and the lines a
// press runs.
#include "core/Hash.h"
#include "game/GameData.h"
#include "game/Macros.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using namespace vd;
using game::MacroStore;
using game::MacroTab;

std::vector<std::string> lines(std::string_view body) {
    std::vector<std::string> out;
    game::forEachMacroLine(body, [&out](std::string_view line) { out.emplace_back(line); });
    return out;
}

const game::GameData* gameData() {
    static pack::PackReader db;
    static game::GameData data;
    static int state = 0;
    if (state == 0) {
        const char* dir = std::getenv("VD_PACKS_DIR");
        state = dir && db.open(std::filesystem::path(dir) / "db.vdpk").ok() && data.init(db) ? 1 : 2;
    }
    return state == 1 ? &data : nullptr;
}

} // namespace

TEST_CASE("Macros: two tabs of 18 sorted by name, ids that stay") {
    MacroStore store;
    CHECK(store.count(MacroTab::Account) == 0);
    const i32 b = store.create(MacroTab::Account, "Bandage", "INV_Misc_Bandage_15", "/use Heavy Linen Bandage");
    const i32 a = store.create(MacroTab::Account, "attack", "Ability_Warrior_Charge", "/cast Charge\n/cast Rend");
    REQUIRE(a >= 0);
    REQUIRE(b >= 0);
    // stricmp's order: "attack" before "Bandage", whatever the case; the ids are the creation order.
    REQUIRE(store.slot(0) != nullptr);
    CHECK(store.slot(0)->name == "attack");
    CHECK(store.slot(0)->id == 2);
    CHECK(store.slot(1)->name == "Bandage");
    CHECK(store.slot(1)->id == 1);
    CHECK(store.slot(2) == nullptr);
    // The character's tab: slots 18.., ids from 0x1000001.
    const i32 c = store.create(MacroTab::Character, "Mount", "Ability_Mount_RidingHorse", "/use Brown Horse Bridle");
    CHECK(c == 18);
    CHECK(store.slot(18)->id == game::kCharacterMacroIdBase + 1);
    CHECK(MacroStore::tabOf(18) == MacroTab::Character);
    // A rename sorts again and keeps the id.
    const i32 renamed = store.edit(0, "zzz", {}, false, {});
    CHECK(renamed == 1);
    CHECK(store.slot(1)->id == 2);
    CHECK(store.slot(1)->icon == "Ability_Warrior_Charge");
    CHECK(store.slot(1)->body == "/cast Charge\n/cast Rend");
    // Deleting leaves the others' ids alone; a new one takes the next id, never a freed one.
    store.remove(0);
    CHECK(store.count(MacroTab::Account) == 1);
    CHECK(store.find(1) == nullptr);
    CHECK(store.find(2) != nullptr);
    CHECK(store.create(MacroTab::Account, "Again", "Spell_Holy_Heal", "") >= 0);
    CHECK(store.find(3) != nullptr);
    // No name: refused (CreateMacro() failed, no name specified); a full tab refuses too.
    CHECK(store.create(MacroTab::Account, "  ", "Spell_Holy_Heal", "") == -1);
    for (u32 i = store.count(MacroTab::Account); i < game::kMacrosPerTab; ++i) {
        CHECK(store.create(MacroTab::Account, "m" + std::to_string(i), "Spell_Holy_Heal", "") >= 0);
    }
    CHECK(store.count(MacroTab::Account) == 18);
    CHECK(store.create(MacroTab::Account, "one too many", "Spell_Holy_Heal", "") == -1);
}

TEST_CASE("Macros: limits of the name and the body") {
    MacroStore store;
    // MacroPopupEditBox letters="16" and MacroFrameText letters="255", never in the middle of a character.
    REQUIRE(store.create(MacroTab::Account, "abcdefghijklmnopqrstu", "X", std::string(300, 'a')) >= 0);
    CHECK(store.slot(0)->name == "abcdefghijklmnop");
    CHECK(store.slot(0)->body.size() == 255);
    CHECK(game::clampUtf8("ñññññññññ", 16) == "ññññññññ"); // 2 bytes each: 8 fit
    CHECK(game::clampUtf8("aññññññññ", 16).size() == 15); // 'a' and seven: the eighth would be split
    CHECK(game::macroBodyText("/say hi\r\n/dance") == "/say hi\n/dance");
}

TEST_CASE("Macros: action buttons hold the id, the character's with ACTION_BUTTON_CMACRO") {
    const game::ActionButton account = game::macroButton(5);
    CHECK(account.type == net::kActionMacro); // 0x40
    CHECK(account.action == 5);
    CHECK(account.packed() == 0x40000005u);
    CHECK(game::macroIdOf(account) == 5);
    const game::ActionButton character = game::macroButton(game::kCharacterMacroIdBase + 3);
    CHECK(character.type == net::kActionClientMacro); // 0x41
    CHECK(character.action == 3);
    CHECK(game::macroIdOf(character) == game::kCharacterMacroIdBase + 3);
    CHECK(game::macroTabOf(game::kCharacterMacroIdBase + 3) == MacroTab::Character);
    CHECK(game::macroTabOf(5) == MacroTab::Account);
    CHECK(game::macroIdOf({net::kActionSpell, 133}) == 0);
    CHECK(game::macroIdOf({net::kActionItem, 6948}) == 0);
}

TEST_CASE("Macros: macros-cache.txt as WoW.exe writes and reads it") {
    MacroStore store;
    store.create(MacroTab::Account, "Heal", "Spell_Holy_Heal", "/cast Lesser Heal(Rank 2)\n\n/say Healing %t");
    store.create(MacroTab::Character, "Mine", "INV_Pick_02", "/w Arthel ore here");
    const std::string account = store.format(MacroTab::Account);
    CHECK(account == "MACRO 1 \"Heal\" Spell_Holy_Heal\r\n/cast Lesser Heal(Rank 2)\r\n/say Healing %t\r\nEND\r\n");
    const std::string character = store.format(MacroTab::Character);
    CHECK(character == "MACRO 16777217 \"Mine\" INV_Pick_02\r\n/w Arthel ore here\r\nEND\r\n");

    MacroStore read;
    CHECK(read.parse(MacroTab::Account, account) == 1);
    CHECK(read.parse(MacroTab::Character, character) == 1);
    REQUIRE(read.find(1) != nullptr);
    CHECK(read.find(1)->name == "Heal");
    CHECK(read.find(1)->icon == "Spell_Holy_Heal");
    CHECK(read.find(1)->iconHash == assetPathHash("Interface\\Icons\\Spell_Holy_Heal.blp"));
    CHECK(read.find(1)->body == "/cast Lesser Heal(Rank 2)\n/say Healing %t");
    REQUIRE(read.find(game::kCharacterMacroIdBase + 1) != nullptr);
    // A new macro after reading takes the next id after the highest read.
    CHECK(read.create(MacroTab::Account, "Next", "X", "") >= 0);
    CHECK(read.find(2) != nullptr);
    // A character macro file written with small ids gets the base added (the client's reader), and "\n" line ends,
    // unknown lines and a missing END are borne.
    MacroStore old;
    CHECK(old.parse(MacroTab::Character, "junk\nMACRO 7 \"Old\" INV_Misc_QuestionMark\n/dance\nEND\nMACRO 8 \"Cut\" X\n") ==
          1);
    REQUIRE(old.slot(18) != nullptr);
    CHECK(old.slot(18)->id == game::kCharacterMacroIdBase + 7);
    CHECK(old.slot(18)->body == "/dance");
}

TEST_CASE("Macros: the lines a press runs and the first cast") {
    CHECK(lines("/cast Fireball\n\n  \n/say hi\r\nhello") == std::vector<std::string>{"/cast Fireball", "/say hi", "hello"});
    CHECK(game::macroCastText("/target Hogger\n/cast Fireball(Rank 3)\n/cast Frostbolt") == "Fireball(Rank 3)");
    CHECK(game::macroCastText("/SPELL Frost Armor") == "Frost Armor");
    CHECK(game::macroCastText("/say no cast") == "");
    CHECK(game::macroCastText("/script CastSpellByName(\"Shoot\")") == "Shoot");
    CHECK(game::macroCastText("/castsequence a") == "");
}

TEST_CASE("Macros: the icon's texture and a spell by name (VD_PACKS_DIR)") {
    CHECK(game::macroIconHash("INV_Misc_QuestionMark") ==
          assetPathHash("Interface\\Icons\\INV_Misc_QuestionMark.blp"));
    const game::GameData* data = gameData();
    if (!data) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    // A mage's Fireball ranks 1-3 (Spell.dbc 133, 143, 145): the highest known, or the rank asked for.
    const std::vector<u32> known = {133, 143, 145, 168};
    CHECK(game::knownSpellByName(known, *data, "Fireball") == 145);
    CHECK(game::knownSpellByName(known, *data, "fireball(Rank 2)") == 143);
    CHECK(game::knownSpellByName(known, *data, "Fireball(Rank 9)") == 0);
    CHECK(game::knownSpellByName(known, *data, "Frost Armor") == 168);
    CHECK(game::knownSpellByName(known, *data, "Frostbolt") == 0);
}
