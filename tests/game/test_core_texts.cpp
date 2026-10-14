// The common interface's texts (phase 8, localization): the keys the core screens use exist in the baked strings
// table of ui.vdpk (English, and the Spanish translation), the client's texts the network errors and spell failures
// map to, and the client tables behind the names they show (races, classes, spell foci, forms, areas).
#include "TestData.h"
#include "core/Text.h"
#include "game/CharacterNames.h"
#include "game/GameData.h"
#include "net/GameMessages.h"
#include "net/RealmClient.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace vd;

namespace {

bool openDb(pack::PackReader& db) {
    const char* dir = std::getenv("VD_PACKS_DIR");
    return dir && db.open(std::filesystem::path(dir) / "db.vdpk").ok();
}

// Our own keys (locale/enUS/common.lang) and the client's the core screens ask for by name.
constexpr const char* kCoreKeys[] = {
    // Login, character list, connection.
    "ACCOUNT_NAME", "PASSWORD", "VD_LOGIN_SERVER", "VD_LOGIN_NO_SERVER", "LOGIN", "NEXT",
    "VD_LOGIN_OFFLINE", "REALM_CHARACTERS", "CHARACTER_SELECT_INFO", "CHARACTER_SELECT_INFO_GHOST",
    "ENTER_WORLD", "DELETE", "CREATE", "LOGOUT", "CANCEL",
    "VD_LOGIN_RECONNECTING", "LOGIN_STATE_AUTHENTICATING", "AUTH_WAIT_QUEUE", "GAME_SERVER_LOGIN",
    "CHAR_LOGIN_IN_PROGRESS", "VD_LOGIN_BAD_PORT", "LOGIN_ENTER_NAME", "LOGIN_ENTER_PASSWORD", "VD_LOGIN_ENTER_SERVER",
    "VD_LOGIN_CANT_CONNECT", "REALM_LIST_INVALID", "CHAR_LOGIN_NO_WORLD", "CHAR_LOGIN_FAILED", "DISCONNECTED",
    "AUTH_FAILED", "VD_TRAVEL_LOADING_WORLD", "VD_TRAVEL_LOADING_TRANSFER", "VD_TRAVEL_LOADING_NO_PACK",
    // Gossip, quests, the quest log, loot, chat.
    "GOODBYE", "QUEST_OBJECTIVES", "ACCEPT", "DECLINE", "TURN_IN_ITEMS", "REQUIRED_MONEY", "CONTINUE",
    "COMPLETE_QUEST", "QUEST_REWARDS", "REWARD_CHOICES", "REWARD_CHOOSE", "REWARD_ITEMS_ONLY", "REWARD_ITEMS",
    "EXPERIENCE_COLON", "QUEST_LOG", "QUEST_DESCRIPTION", "QUEST_LOG_COUNT_TEMPLATE", "QUEST_MONSTERS_KILLED",
    "QUEST_ITEMS_NEEDED", "FAILED", "COMPLETE", "ABANDON_QUEST", "CLOSE", "VD_QUEST_LOG_EMPTY", "LOOT",
    "VD_LOOT_NOTHING", "CHAT_MSG_SAY", "SEND_LABEL", "VD_CHAT_INPUT_HINT",
    // Menu, settings, confirmations, death.
    "MAIN_MENU", "RETURN_TO_GAME", "CHARACTER_BUTTON", "VD_MENU_BAGS", "SPELLBOOK_BUTTON", "TALENTS_BUTTON",
    "QUESTLOG_BUTTON", "SKILLS", "REPUTATION", "SOCIAL_BUTTON", "WORLDMAP_BUTTON", "OPTIONS",
    "VD_MENU_LOGOUT_CONFIRM", "UI_SCALE", "VD_SETTINGS_CAMERA_SPEED", "FARCLIP", "VD_SETTINGS_YARDS",
    "VD_SETTINGS_INVERT_CAMERA", "VD_SETTINGS_SHOW_FPS", "SHOW_UNIT_NAMES", "YES", "NO", "ABANDON_QUEST_CONFIRM",
    "QUEST_ACCEPT", "RESURRECT_REQUEST", "CONFIRM_XP_LOSS", "CONFIRM_XP_LOSS_NO_SICKNESS", "GENERIC_MIN",
    "GENERIC_MIN_P1", "DEATH_RELEASE_NOTIMER", "DEATH_RELEASE", "RECOVER_CORPSE", "RECOVER_CORPSE_TIMER",
    "SECONDS_ABBR", "VD_DEATH_CORPSE_DISTANCE",
    // The HUD and its messages.
    "ERR_USE_TOO_FAR", "ERR_BADATTACKPOS", "ERR_BADATTACKFACING", "ERR_CANTATTACK_NOTSTANDING",
    "ERR_INVALID_ATTACK_TARGET", "ERR_LEARN_SPELL_S", "COMBATLOG_XPGAIN_FIRSTPERSON_UNNAMED", "LEVEL_UP",
    "LOOT_ITEM_SELF", "LOOT_ITEM_SELF_MULTIPLE", "LOOT_ITEM_PUSHED_SELF", "LOOT_ITEM_PUSHED_SELF_MULTIPLE",
    "LOOT_ITEM_CREATED_SELF", "LOOT_ITEM_CREATED_SELF_MULTIPLE", "YOU_LOOT_MONEY", "ERR_LOOT_NO_UI",
    "ERR_QUEST_ACCEPTED_S", "ERR_QUEST_OBJECTIVE_COMPLETE_S", "ERR_QUEST_COMPLETE_S", "ERR_QUEST_FAILED_S",
    "ERR_QUEST_ADD_KILL_SII", "ERR_QUEST_ADD_FOUND_SII", "ERR_QUEST_ADD_ITEM_SII", "ERR_QUEST_LOG_FULL",
    "ERR_QUEST_NEED_PREREQS", "ERR_QUEST_FAILED_LOW_LEVEL", "ERR_QUEST_FAILED_WRONG_RACE", "ERR_QUEST_ONLY_ONE_TIMED",
    "ERR_QUEST_ALREADY_ON", "ERR_QUEST_FAILED_MISSING_ITEMS", "ERR_QUEST_FAILED_NOT_ENOUGH_MONEY",
    "ERR_QUEST_FAILED_BAG_FULL_S", "ERR_QUEST_FAILED_MAX_COUNT_S", "ERR_INV_FULL", "ERR_OUT_OF_MANA",
    "ERR_OUT_OF_RAGE", "ERR_OUT_OF_FOCUS", "ERR_OUT_OF_ENERGY", "ERR_OUT_OF_HEALTH",
    "ERR_SPELL_FAILED_ALREADY_AT_FULL_HEALTH", "ERR_SPELL_FAILED_ALREADY_AT_FULL_POWER_S",
    "ERR_SPELL_FAILED_REAGENTS_GENERIC", "SPELL_FAILED_EQUIPPED_ITEM", "SPELL_FAILED_NOT_HERE", "SPELL_FAILED_UNKNOWN",
    "MISS", "DODGE", "PARRY", "BLOCK", "EVADE", "IMMUNE", "RESIST", "ABSORB", "DEAD", "INTERRUPTED", "VD_HUD_FPS",
    "MINUTE_ONELETTER_ABBR", "SECOND_ONELETTER_ABBR", "VD_MONEY_GOLD", "VD_MONEY_SILVER", "VD_MONEY_COPPER", "MANA",
    "RAGE", "FOCUS", "ENERGY", "HAPPINESS"};

} // namespace

TEST_CASE("Core texts: every key of the common interface is in ui.vdpk, worded in English and Spanish") {
    if (!test::loadTexts()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    for (const char* key : kCoreKeys) {
        INFO(key);
        CHECK(text::has(key));
    }
    // The client's wording, and ours.
    char buffer[256];
    CHECK(text::get("ACCEPT") == "Accept");
    CHECK(text::format(buffer, "CHARACTER_SELECT_INFO_GHOST", 60u, "Night Elf Druid") ==
          "Level 60 Night Elf Druid (Ghost)");
    CHECK(text::format(buffer, "QUEST_MONSTERS_KILLED", "Kobold Vermin", 3, 10) == "Kobold Vermin slain: 3/10");
    CHECK(text::format(buffer, "VD_LOGIN_CANT_CONNECT", "192.168.1.20", 3724) ==
          "Unable to connect to 192.168.1.20:3724.");
    CHECK(text::format(buffer, "CONFIRM_XP_LOSS", "5 minutes").find("25% durability") != std::string_view::npos);
    // The network thread's texts: realmd refusals (LOGIN_*) and world session codes (AUTH_*, by their names).
    for (u8 code = 0; code < 0x20; ++code) {
        INFO(code);
        CHECK(text::has(net::realmdResultKey(code)));
    }
    CHECK(text::has(net::realmdResultKey(0x04)));
    CHECK(std::string(net::realmdResultKey(0x05)) == "LOGIN_UNKNOWN_ACCOUNT");
    CHECK(text::has("AUTH_REJECT"));
    CHECK(text::has("CHAR_LOGIN_DISABLED"));
    // Spell failures: SPELL_FAILED_<name> for every SpellCastResult but the few the screens word otherwise.
    for (u32 reason = 0; reason < 146; ++reason) {
        const std::string name = net::spellCastResultName(static_cast<u8>(reason));
        if (name == "DONT_REPORT" || name == "NO_POWER" || name == "ALREADY_AT_FULL_HEALTH" ||
            name == "ALREADY_AT_FULL_POWER" || name == "AUTOTRACK_INTERRUPTED" || name == "HUNGER_SATIATED" ||
            name == "THIRST_SATIATED" || name == "PLAY_TIME") {
            continue; // worded by GameScreens, or SPELL_FAILED_UNKNOWN
        }
        INFO(name);
        CHECK(text::has(std::string_view("SPELL_FAILED_" + name)));
    }

    // The Spanish table: our keys and the client's, translated; what is not falls back to English.
    REQUIRE(text::load(std::filesystem::path(std::getenv("VD_PACKS_DIR")) / "ui.vdpk", "esES"));
    CHECK(text::get("VD_LOGIN_SERVER") == "Servidor");
    CHECK(text::get("ACCEPT") == "Aceptar");
    // Phase 21: the esES client's own wording when ui.vdpk was baked with it, ours otherwise.
    const bool official = test::esESClientTexts();
    CHECK(text::format(buffer, "CHARACTER_SELECT_INFO", 3u, "Human Warrior") ==
          (official ? "Nivel 3 Human Warrior" : "Nivel 3 · Human Warrior"));
    CHECK(text::format(buffer, "VD_MONEY_GOLD", 12u) == "12o");
    // Untranslated by us: English (the loading screens' tips option, which this client does not show; phase 13
    // translated ERR_AUTOFOLLOW_TOO_FAR, phase 15 MAX_FOLLOW_DIST and phase 18 the tutorials, the keys this checked
    // before). The esES client translates it.
    const std::string_view tips = text::get("SHOW_TIPOFTHEDAY_TEXT");
    CHECK((official ? tips.starts_with("Mostrar los consejos ") : tips == "Show Loading Screen Tips"));
    CHECK(text::get("TUTORIAL_TITLE1") == (official ? "Asignadores de misiones" : "Personajes con misiones"));
    for (const char* key : kCoreKeys) {
        INFO(key);
        CHECK(text::has(key));
    }
    text::unload();
}

TEST_CASE("GameData: what cast failures name (reagents, totems, spell foci, forms, areas)") {
    pack::PackReader db;
    if (!openDb(db)) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    game::GameData data;
    REQUIRE(data.init(db));
    // Teleport: Stormwind (3561) consumes a Rune of Teleportation (17031).
    const std::optional<game::SpellRequirements> teleport = data.requirements(3561);
    REQUIRE(teleport.has_value());
    CHECK(teleport->reagents[0] == 17031);
    CHECK(teleport->reagentCounts[0] == 1);
    CHECK(teleport->reagents[1] == 0);
    // Searing Totem (3599) needs the Fire Totem (5176).
    CHECK(data.requirements(3599)->totems[0] == 5176);
    // Claw (1082) is cast in Cat Form (form 1: stance bit 0).
    CHECK((data.requirements(1082)->stances & 1u) != 0);
    CHECK(data.shapeshiftFormName(1) == "Cat Form");
    CHECK(data.shapeshiftFormName(17) == "Battle Stance");
    // Smelt Copper (2657) needs a forge.
    const u32 forge = data.requirements(2657)->spellFocus;
    CHECK(forge != 0);
    CHECK(data.spellFocusName(forge) == "Forge");
    CHECK(data.areaName(12) == "Elwynn Forest");
    CHECK(data.areaName(1519) == "Stormwind City");
    CHECK(data.areaName(0).empty());
    CHECK(data.spellFocusName(999999).empty());
    CHECK_FALSE(data.requirements(999999).has_value());
}
