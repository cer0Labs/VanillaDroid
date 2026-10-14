#pragma once

#include "core/Types.h"

#include <array>
#include <span>

namespace vd::game {

// The menus of a unit frame's right click as 1.12's UnitPopup.lua builds them (phase 13): UnitPopupMenus (which
// buttons a menu lists), UnitPopupButtons (their texts, colours, icons and interact distances), UnitPopup_HideButtons
// (which of them show) and UnitPopup_OnUpdate (which of them are enabled), for the frames that open them
// (PlayerFrame, TargetFrame, PartyMemberFrame1..4, PetFrame, FriendsDropDown, the raid roster). Pure rules: the
// caller describes the player, its group and the unit in a UnitPopupState (app/UnitMenuScreens fills it from the
// session) and acts on the button picked (UnitPopup_OnClick). No allocation.

// UnitPopupMenus: the first level of each frame's menu, and the second level of the nested buttons.
enum class UnitPopupMenu : u8 {
    Self,           // PlayerFrame, or the target when it is the player
    Pet,            // PetFrame, or the target when it is the player's pet
    Party,          // PartyMemberFrame1..4, or a target in the player's party
    Player,         // any other player targeted
    Raid,           // a member of the raid roster (RaidFrame's buttons)
    Friend,         // FriendsDropDown: a name in the friends, who or guild lists, a player's name in the chat
    RaidTargetIcon, // the target when it is not a player (a creature): only the icons
    LootMethod,     // second level
    LootThreshold,  // second level
};

// UnitPopupButtons (the ones the 1.12 menus list).
enum class UnitPopupButton : u8 {
    Cancel,
    Trade,
    Inspect,
    Target,
    Duel,
    Whisper,
    Invite,
    Uninvite,
    Promote,
    GuildPromote,
    GuildLeave,
    Leave,
    Follow,
    PetDismiss,
    PetAbandon,
    PetPaperdoll,
    PetRename,
    LootMethod,
    FreeForAll,
    RoundRobin,
    MasterLooter,
    GroupLoot,
    NeedBeforeGreed,
    ResetInstances,
    LootThreshold,
    LootPromote,
    Quality2,
    Quality3,
    Quality4,
    RaidLeader,
    RaidPromote,
    RaidDemote,
    RaidRemove,
    RaidTargetIcon,
    RaidTarget1,
    RaidTarget2,
    RaidTarget3,
    RaidTarget4,
    RaidTarget5,
    RaidTarget6,
    RaidTarget7,
    RaidTarget8,
    RaidTargetNone,
    Count
};

// A button's entry of UnitPopupButtons.
struct UnitPopupButtonInfo {
    const char* text = "";  // its text's key (core/Text): TEXT(TRADE), TEXT(PARTY_INVITE)...
    u8 dist = 0;            // CheckInteractDistance's index (1 inspect, 2 trade, 3 duel, 4 follow); 0: anywhere
    bool nested = false;    // opens a second level (LOOT_METHOD and LOOT_THRESHOLD only for the group's leader)
    bool checkable = false; // the raid target icons
    u32 color = 0;          // its text's colour (RGBA, ui::rgba order), 0: HIGHLIGHT_FONT_COLOR
    u8 icon = 0;            // the raid target icon it shows (1..8, UI-RaidTargetingIcons), 0: none
};
const UnitPopupButtonInfo& unitPopupButtonInfo(UnitPopupButton button);
// RAID_TARGET_1..8: the icon (1..8) a button sets; 0 for RAID_TARGET_NONE and every other button.
u8 raidTargetOfButton(UnitPopupButton button);
bool isRaidTargetButton(UnitPopupButton button); // RAID_TARGET_1..8 and RAID_TARGET_NONE

// CheckInteractDistance(unit, index) in WoW.exe 1.12.1 (0x48ba00): the squared distance between the two units'
// positions against these yards (index 1 inspect, 2 trade, 3 duel, 4 follow).
constexpr std::array<f32, 5> kInteractDistances = {0.0f, 10.0f, 11.11f, 10.0f, 30.0f};

// What the rules read: the player, its group, the unit the menu is about (`dropdownMenu.unit`), the target (the
// raid target icons' rules look at "target" whatever the menu is about) and the name (FRIEND's menus have no unit).
struct UnitPopupState {
    // The player and its group.
    bool inParty = false;   // GetNumPartyMembers() > 0 or GetNumRaidMembers() > 0
    bool leader = false;    // IsPartyLeader()
    bool assistant = false; // IsRaidOfficer(): an assistant of a raid (the leader is not)
    bool playerDead = false;        // UnitIsDead("player")
    bool playerDeadOrGhost = false; // UnitIsDeadOrGhost("player")
    bool fullControl = true;        // HasFullControl()
    bool canShowResetInstances = false; // CanShowResetInstances()
    bool guildLeader = false;       // IsGuildLeader()
    bool guildFrameVisible = false; // GuildFrame:IsVisible(): the social window shows its guild tab
    u8 lootMethod = 0;              // GetLootMethod() (net::LootMethod)
    u8 lootThreshold = 2;           // GetLootThreshold()
    bool petCanBeAbandoned = false; // PetCanBeAbandoned(): the hunter's pet (UNIT_FLAG_PET_ABANDON)
    bool petCanBeRenamed = false;   // PetCanBeRenamed() (UNIT_FLAG_PET_RENAME)

    // The unit (none for FRIEND).
    bool hasUnit = true;
    bool canCooperate = false;   // UnitCanCooperate("player", unit)
    bool unitConnected = true;   // UnitIsConnected(unit)
    bool unitDeadOrGhost = false;
    bool unitIsMasterLooter = false; // GetLootMethod()'s master looter is that unit
    // CheckInteractDistance(unit, i) for i = 1..4 (index 0 unused).
    std::array<bool, 5> inDistance{true, true, true, true, true};
    u8 unitRaidTarget = 0; // GetRaidTargetIndex(unit): 1..8, 0 none (the second level's check)

    // The target.
    bool targetExists = false;
    bool targetInGroup = false;     // UnitPlayerOrPetInParty("target") or UnitPlayerOrPetInRaid("target")
    bool targetIsPlayer = false;    // UnitIsPlayer("target")
    bool targetCanCooperate = false;
    bool targetIsSelf = false;      // UnitIsUnit("target", "player")
    u8 targetRaidTarget = 0;        // GetRaidTargetIndex("target") (the first level's check)

    // The name (UnitName(unit), or the name FRIEND's menu was opened for).
    bool hasName = true;
    bool nameIsSelf = false;    // dropdownMenu.name == UnitName("player")
    bool nameInParty = false;   // == UnitName("party1".."party4")
    u8 raidRank = 0;            // RAID: GetRaidRosterInfo's rank (2 leader, 1 assistant, 0)
};

// One button of a menu as the client shows it.
struct UnitPopupItem {
    UnitPopupButton button = UnitPopupButton::Cancel;
    bool enabled = true;   // UnitPopup_OnUpdate
    bool nested = false;   // hasArrow
    bool checkable = false; // !notCheckable (every second-level button, the icons of the first)
    bool checked = false;
};
constexpr u32 kUnitPopupMaxItems = 12; // the longest menu (PARTY, RAID_TARGET_ICON) and a margin

// UnitPopup_ShowMenu's first level: the buttons of `menu` that UnitPopup_HideButtons shows, in order, with
// UnitPopup_OnUpdate's enabled state. Returns how many were written; 0 when only CANCEL would show (the menu does
// not open at all).
u32 unitPopupItems(UnitPopupMenu menu, const UnitPopupState& s, std::span<UnitPopupItem> out);
// The second level a nested button opens (LOOT_METHOD, LOOT_THRESHOLD, RAID_TARGET_ICON): every button of its list
// (the client skips the first level's hide rules there), checked where the group's method / threshold or the
// unit's icon is.
u32 unitPopupSubItems(UnitPopupButton nested, const UnitPopupState& s, std::span<UnitPopupItem> out);
// The menu that a nested button opens (LootMethod, LootThreshold, RaidTargetIcon); Self for any other.
UnitPopupMenu nestedMenu(UnitPopupButton button);

// TargetFrameDropDown_Initialize: the target's menu (the player, its pet, a player of its party or another
// player, and the raid target icons alone for anything else).
UnitPopupMenu targetFrameMenu(bool targetIsSelf, bool targetIsPet, bool targetIsPlayer, bool targetInParty);

// SetRaidTargetIcon (TargetFrame.lua): picking the icon the unit already wears takes it off (0), any other puts
// it on. Returns the icon to set (0..8, 0 = none).
constexpr u8 toggledRaidTarget(u8 current, u8 picked) {
    return current != 0 && current == picked ? 0 : picked;
}

} // namespace vd::game
