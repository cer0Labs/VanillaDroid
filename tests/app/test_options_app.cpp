// Phase 15 (options): the options' app side without Vulkan: every UIOptionsFrame / SoundOptionsFrame setting survives
// the settings file with its 1.12 default (but four changed on purpose, and the file's version that moves
// the older files to them), and BuffFrame's rows without the durations (SHOW_BUFF_DURATIONS "0").
#include "app/AuraLayout.h"
#include "app/Dialogs.h"
#include "app/SettingsFile.h"

#include <doctest/doctest.h>

#include <array>
#include <string>
#include <vector>

using namespace vd;

TEST_CASE("Options: the interface and sound options keep 1.12's defaults and read back from the settings file") {
    const SettingsView d;
    // UIOptionsFrame.lua / the CVars' defaults in WoW.exe, but four deliberate changes from 1.12: auto self
    // cast, NPC names and buff durations on, and a camera that never follows.
    CHECK(d.deselectOnClick);
    CHECK_FALSE(d.assistAttack);
    CHECK(d.autoClearAfk);
    CHECK_FALSE(d.blockTrades);
    CHECK(d.autoSelfCast); // 1.12: off
    CHECK_FALSE(d.statusBarText);
    CHECK(d.showPlayerNames);
    CHECK(d.showNpcNames); // 1.12: off
    CHECK_FALSE(d.showOwnName);
    CHECK(d.showBuffDurations); // 1.12: off
    CHECK(d.showNewbieTips);
    CHECK(d.cameraFollowStyle == 0); // never (1.12: CAMERA_SMART)
    CHECK_FALSE(d.lockActionBars);
    CHECK_FALSE(d.alwaysShowMultiBars);
    CHECK(d.showLootSpam);
    CHECK_FALSE(d.hidePartyInterface);
    CHECK_FALSE(d.showCombatText);
    CHECK(d.combatTextFloatMode == 1);
    CHECK(d.showDamage);
    CHECK(d.soundEffects);
    CHECK(d.musicEnabled);
    CHECK(d.ambienceSounds);
    CHECK(d.errorSpeech);
    CHECK(d.emoteSounds);
    CHECK_FALSE(d.musicLooping);
    CHECK(d.soundAtCharacter);

    SettingsView written;
    written.deselectOnClick = false;
    written.assistAttack = true;
    written.autoClearAfk = false;
    written.blockTrades = true;
    written.autoSelfCast = false;
    written.statusBarText = true;
    written.showPlayerNames = false;
    written.showGuildNames = false;
    written.showPlayerTitles = false;
    written.showNpcNames = false;
    written.showOwnName = true;
    written.showPartyBackground = true;
    written.showBuffDurations = false;
    written.hideOutdoorWorldState = true;
    written.showNewbieTips = false;
    written.cameraFollowStyle = 1;
    written.cameraTerrainTilt = true;
    written.cameraBobbing = true;
    written.cameraWaterCollision = false;
    written.cameraFollowSpeed = 250.0f;
    written.cameraDistanceMaxFactor = 1.5f;
    written.lockActionBars = true;
    written.alwaysShowMultiBars = true;
    written.guildMemberAlert = true;
    written.showLootSpam = false;
    written.hidePartyInterface = true;
    written.showPartyPets = false;
    written.showDispellableDebuffs = false;
    written.showCastableBuffs = false;
    written.showTargetOfTarget = true;
    written.targetOfTargetState = 4;
    written.showCombatText = true;
    written.combatTextFloatMode = 3;
    written.combatTextAuraFade = true;
    written.combatTextDodgeParryMiss = true;
    written.combatTextResistances = true;
    written.combatTextReputation = true;
    written.combatTextReactives = true;
    written.combatTextFriendlyNames = true;
    written.combatTextComboPoints = true;
    written.combatTextMana = true;
    written.combatTextHonorGained = false;
    written.showDamage = false;
    written.logPeriodicEffects = false;
    written.showPetDamage = false;
    written.soundEffects = false;
    written.ambienceSounds = false;
    written.errorSpeech = false;
    written.musicEnabled = false;
    written.soundAtCharacter = false;
    written.emoteSounds = false;
    written.musicLooping = true;
    const std::string text = formatSettings(written);
    SettingsView read;
    parseSettings(text, read);
    CHECK(read == written);
}

TEST_CASE("Options: a settings file older than version 2 takes the new defaults of the four changed from 1.12") {
    // Written before (no version line): every value, the old defaults among them.
    const std::string old = "auto_self_cast = 0\nshow_npc_names = 0\nshow_buff_durations = 0\ncamera_follow_style = 1\n"
                            "show_player_names = 0\n";
    SettingsView read;
    parseSettings(old, read);
    CHECK(read.autoSelfCast);
    CHECK(read.showNpcNames);
    CHECK(read.showBuffDurations);
    CHECK(read.cameraFollowStyle == 0);
    CHECK_FALSE(read.showPlayerNames); // the rest as written
    // From version 2 on, the player's choices.
    SettingsView chosen;
    parseSettings("version = 2\n" + old, chosen);
    CHECK_FALSE(chosen.autoSelfCast);
    CHECK_FALSE(chosen.showNpcNames);
    CHECK_FALSE(chosen.showBuffDurations);
    CHECK(chosen.cameraFollowStyle == 1);
    CHECK(formatSettings(chosen).find("version = 2\n") != std::string::npos);
}

TEST_CASE("Options: BuffFrame without the durations puts its rows 35 units apart") {
    std::vector<HudAura> auras;
    for (u32 i = 0; i < 10; ++i) {
        HudAura a;
        a.iconHash = 1;
        auras.push_back(a);
    }
    HudAura debuff;
    debuff.iconHash = 1;
    debuff.harmful = true;
    auras.push_back(debuff);
    std::array<AuraButtonPlace, 16> places{};
    REQUIRE(buffFrameLayout(auras, 1000.0f, 10.0f, 1.0f, places, false) == 11);
    const auto at = [&](u32 index) -> const AuraButtonPlace& {
        for (u32 i = 0; i < 11; ++i) {
            if (places[i].aura == index) {
                return places[i];
            }
        }
        FAIL("aura not placed");
        return places[0];
    };
    CHECK(at(0).icon.y == doctest::Approx(10.0f));
    CHECK(at(8).icon.y == doctest::Approx(45.0f));  // BuffButton8: 5 under the first row
    CHECK(at(10).icon.y == doctest::Approx(80.0f)); // the debuffs 70 down
    CHECK(at(8).hit.h == doctest::Approx(35.0f));   // the rows tile without the durations' space
    // With them: 45 and 90 (as before).
    REQUIRE(buffFrameLayout(auras, 1000.0f, 10.0f, 1.0f, places, true) == 11);
    CHECK(at(8).icon.y == doctest::Approx(55.0f));
    CHECK(at(10).icon.y == doctest::Approx(100.0f));
}
