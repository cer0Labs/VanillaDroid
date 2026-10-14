#include "game/UnitPopup.h"

#include <iterator>

namespace vd::game {
namespace {

using B = UnitPopupButton;

constexpr u32 color(f32 r, f32 g, f32 b) {
    const auto c = [](f32 v) { return static_cast<u32>(v * 255.0f + 0.5f); };
    return c(r) | (c(g) << 8) | (c(b) << 16) | (255u << 24);
}

// UnitPopupButtons, in UnitPopupButton order. The raid target icons' colours are the table's; ITEM_QUALITY_COLORS
// (2..4) for the thresholds.
constexpr UnitPopupButtonInfo kButtons[] = {
    {"CANCEL"},
    {"TRADE", 2},
    {"INSPECT", 1},
    {"TARGET"},
    {"DUEL", 3},
    {"WHISPER"},
    {"PARTY_INVITE"},
    {"PARTY_UNINVITE"},
    {"PARTY_PROMOTE"},
    {"GUILD_PROMOTE"},
    {"GUILD_LEAVE"},
    {"PARTY_LEAVE"},
    {"FOLLOW", 4},
    {"PET_DISMISS"},
    {"PET_ABANDON"},
    {"PET_PAPERDOLL"},
    {"PET_RENAME"},
    {"LOOT_METHOD", 0, true},
    {"LOOT_FREE_FOR_ALL"},
    {"LOOT_ROUND_ROBIN"},
    {"LOOT_MASTER_LOOTER"},
    {"LOOT_GROUP_LOOT"},
    {"LOOT_NEED_BEFORE_GREED"},
    {"RESET_INSTANCES"},
    {"LOOT_THRESHOLD", 0, true},
    {"LOOT_PROMOTE"},
    {"ITEM_QUALITY2_DESC", 0, false, false, color(0.12f, 1.0f, 0.0f)},
    {"ITEM_QUALITY3_DESC", 0, false, false, color(0.0f, 0.44f, 0.87f)},
    {"ITEM_QUALITY4_DESC", 0, false, false, color(0.64f, 0.21f, 0.93f)},
    {"NEW_LEADER"},
    {"PROMOTE"},
    {"DEMOTE"},
    {"REMOVE"},
    {"RAID_TARGET_ICON", 0, true},
    {"RAID_TARGET_1", 0, false, true, color(1.0f, 0.92f, 0.0f), 1},
    {"RAID_TARGET_2", 0, false, true, color(0.98f, 0.57f, 0.0f), 2},
    {"RAID_TARGET_3", 0, false, true, color(0.83f, 0.22f, 0.9f), 3},
    {"RAID_TARGET_4", 0, false, true, color(0.04f, 0.95f, 0.0f), 4},
    {"RAID_TARGET_5", 0, false, true, color(0.7f, 0.82f, 0.875f), 5},
    {"RAID_TARGET_6", 0, false, true, color(0.0f, 0.71f, 1.0f), 6},
    {"RAID_TARGET_7", 0, false, true, color(1.0f, 0.24f, 0.168f), 7},
    {"RAID_TARGET_8", 0, false, true, color(0.98f, 0.98f, 0.98f), 8},
    {"NONE", 0, false, true},
};
static_assert(std::size(kButtons) == static_cast<usize>(B::Count), "one entry per button");

// UnitPopupMenus.
constexpr B kSelf[] = {B::LootMethod, B::LootThreshold, B::LootPromote,    B::Leave,
                       B::ResetInstances, B::RaidTargetIcon, B::Cancel};
constexpr B kPet[] = {B::PetPaperdoll, B::PetRename, B::PetAbandon, B::PetDismiss, B::Cancel};
constexpr B kParty[] = {B::Whisper, B::Promote, B::LootPromote, B::Uninvite, B::Inspect,
                        B::Trade,   B::Follow,  B::Duel,        B::RaidTargetIcon, B::Cancel};
constexpr B kPlayer[] = {B::Whisper, B::Inspect, B::Invite, B::Trade, B::Follow, B::Duel, B::RaidTargetIcon, B::Cancel};
constexpr B kRaid[] = {B::RaidLeader, B::RaidPromote, B::RaidDemote, B::RaidRemove, B::Cancel};
constexpr B kFriend[] = {B::Whisper, B::Invite, B::Target, B::GuildPromote, B::GuildLeave, B::Cancel};
constexpr B kRaidTargets[] = {B::RaidTarget1, B::RaidTarget2, B::RaidTarget3, B::RaidTarget4, B::RaidTarget5,
                              B::RaidTarget6, B::RaidTarget7, B::RaidTarget8, B::RaidTargetNone};
constexpr B kLootMethods[] = {B::FreeForAll, B::RoundRobin, B::MasterLooter, B::GroupLoot, B::NeedBeforeGreed,
                              B::Cancel};
constexpr B kLootThresholds[] = {B::Quality2, B::Quality3, B::Quality4, B::Cancel};

std::span<const B> menuButtons(UnitPopupMenu menu) {
    switch (menu) {
    case UnitPopupMenu::Self: return kSelf;
    case UnitPopupMenu::Pet: return kPet;
    case UnitPopupMenu::Party: return kParty;
    case UnitPopupMenu::Player: return kPlayer;
    case UnitPopupMenu::Raid: return kRaid;
    case UnitPopupMenu::Friend: return kFriend;
    case UnitPopupMenu::RaidTargetIcon: return kRaidTargets;
    case UnitPopupMenu::LootMethod: return kLootMethods;
    case UnitPopupMenu::LootThreshold: return kLootThresholds;
    }
    return {};
}

bool isRaidTargetPrefixed(B b) { // strsub(value, 1, 12) == "RAID_TARGET_": the nested button too
    return b == B::RaidTargetIcon || isRaidTargetButton(b);
}

// UnitPopup_HideButtons for one button of `menu`.
bool shown(UnitPopupMenu menu, B b, const UnitPopupState& s) {
    const bool coop = s.hasUnit && s.canCooperate;
    switch (b) {
    case B::Trade:
    case B::Follow:
    case B::Duel:
    case B::Inspect: return coop;
    case B::Invite:
        return s.hasUnit ? coop : !(s.nameInParty || s.nameIsSelf);
    case B::Whisper: return !(s.hasUnit && !coop); // its second rule (the player's own name) is never reached
    case B::Promote:
    case B::Uninvite: return s.inParty && s.leader;
    case B::GuildPromote: return s.guildLeader && !s.nameIsSelf && s.guildFrameVisible;
    case B::GuildLeave: return s.nameIsSelf && s.guildFrameVisible;
    case B::Leave:
    case B::LootThreshold:
    case B::LootMethod: return s.inParty;
    case B::FreeForAll:
    case B::RoundRobin:
    case B::MasterLooter:
    case B::GroupLoot:
    case B::NeedBeforeGreed: {
        const u8 method = static_cast<u8>(static_cast<u8>(b) - static_cast<u8>(B::FreeForAll));
        return s.inParty && (s.leader || s.lootMethod == method);
    }
    case B::LootPromote:
        return s.inParty && s.leader && s.lootMethod == 2 && !s.unitIsMasterLooter;
    case B::ResetInstances: return s.canShowResetInstances && !(s.inParty && !s.leader);
    case B::RaidLeader: return s.leader && s.raidRank != 2 && s.hasName;
    case B::RaidPromote: return s.leader && s.raidRank == 0 && s.hasName;
    case B::RaidDemote: return s.leader && s.raidRank == 1 && s.hasName;
    case B::RaidRemove: return (s.leader || s.assistant) && s.raidRank != 2;
    case B::PetPaperdoll:
    case B::PetAbandon: return s.petCanBeAbandoned;
    case B::PetRename: return s.petCanBeAbandoned && s.petCanBeRenamed;
    case B::PetDismiss: return !s.petCanBeAbandoned;
    default: break;
    }
    if (isRaidTargetPrefixed(b)) {
        if (!s.inParty || (!s.leader && !s.assistant)) {
            return false;
        }
        // Not on enemy players (ERR_INVALID_RAID_TARGET): the target's rule, whichever unit the menu is about.
        return menu == UnitPopupMenu::Self || !s.targetExists || s.targetInGroup || !s.targetIsPlayer ||
               s.targetCanCooperate || s.targetIsSelf;
    }
    return true;
}

// UnitPopup_OnUpdate for one shown button.
bool enabled(B b, const UnitPopupState& s) {
    const u8 dist = unitPopupButtonInfo(b).dist;
    if (dist > 0 && (!s.hasUnit || !s.inDistance[dist])) {
        return false;
    }
    switch (b) {
    case B::Trade:
    case B::Duel: return !s.playerDeadOrGhost && s.fullControl && !s.unitDeadOrGhost;
    case B::Leave: return s.inParty;
    case B::Invite: return !(s.inParty && !s.leader && !s.assistant);
    case B::Uninvite:
    case B::Promote: return s.inParty && s.leader;
    case B::Whisper: return !s.hasUnit || s.unitConnected;
    case B::Inspect: return !s.playerDeadOrGhost;
    case B::Follow: return !s.playerDead;
    case B::LootPromote: return s.inParty && s.leader && s.lootMethod == 2 && !s.unitIsMasterLooter;
    default: return true;
    }
}

} // namespace

const UnitPopupButtonInfo& unitPopupButtonInfo(UnitPopupButton button) {
    return kButtons[button < B::Count ? static_cast<u32>(button) : 0];
}

u8 raidTargetOfButton(UnitPopupButton button) {
    return button >= B::RaidTarget1 && button <= B::RaidTarget8
               ? static_cast<u8>(static_cast<u8>(button) - static_cast<u8>(B::RaidTarget1) + 1)
               : 0;
}

bool isRaidTargetButton(UnitPopupButton button) {
    return button >= B::RaidTarget1 && button <= B::RaidTargetNone;
}

UnitPopupMenu nestedMenu(UnitPopupButton button) {
    switch (button) {
    case B::LootMethod: return UnitPopupMenu::LootMethod;
    case B::LootThreshold: return UnitPopupMenu::LootThreshold;
    case B::RaidTargetIcon: return UnitPopupMenu::RaidTargetIcon;
    default: return UnitPopupMenu::Self;
    }
}

u32 unitPopupItems(UnitPopupMenu menu, const UnitPopupState& s, std::span<UnitPopupItem> out) {
    u32 count = 0;
    bool any = false;
    const std::span<const B> buttons = menuButtons(menu);
    for (u32 index = 0; index < buttons.size() && count < out.size(); ++index) {
        const B b = buttons[index];
        if (!shown(menu, b, s)) {
            continue;
        }
        any = any || b != B::Cancel;
        const UnitPopupButtonInfo& info = unitPopupButtonInfo(b);
        UnitPopupItem& item = out[count++];
        item = UnitPopupItem{};
        item.button = b;
        item.enabled = enabled(b, s);
        // The loot settings open only for the group's leader: the others just read them.
        item.nested = info.nested && ((b != B::LootMethod && b != B::LootThreshold) || (s.inParty && s.leader));
        item.checkable = info.checkable;
        // The first level checks GetRaidTargetIndex("target") against the button's place in the list: only the
        // raid target icons' own menu (a creature targeted) lines them up.
        item.checked = info.checkable && isRaidTargetPrefixed(b) && s.targetRaidTarget == index + 1;
    }
    return any ? count : 0;
}

u32 unitPopupSubItems(UnitPopupButton nested, const UnitPopupState& s, std::span<UnitPopupItem> out) {
    const UnitPopupMenu menu = nestedMenu(nested);
    if (menu == UnitPopupMenu::Self) {
        return 0;
    }
    u32 count = 0;
    const std::span<const B> buttons = menuButtons(menu);
    for (u32 index = 0; index < buttons.size() && count < out.size(); ++index) {
        const B b = buttons[index];
        UnitPopupItem& item = out[count++];
        item = UnitPopupItem{};
        item.button = b;
        item.checkable = true; // the second level never sets notCheckable
        switch (menu) {
        case UnitPopupMenu::LootMethod:
            item.checked = b != B::Cancel && static_cast<u8>(static_cast<u8>(b) - static_cast<u8>(B::FreeForAll)) ==
                                                 s.lootMethod;
            break;
        case UnitPopupMenu::LootThreshold:
            item.checked = b != B::Cancel && static_cast<u8>(static_cast<u8>(b) - static_cast<u8>(B::Quality2) + 2) ==
                                                 s.lootThreshold;
            break;
        default: item.checked = isRaidTargetButton(b) && s.unitRaidTarget == index + 1; break;
        }
    }
    return count;
}

UnitPopupMenu targetFrameMenu(bool targetIsSelf, bool targetIsPet, bool targetIsPlayer, bool targetInParty) {
    if (targetIsSelf) {
        return UnitPopupMenu::Self;
    }
    if (targetIsPet) {
        return UnitPopupMenu::Pet;
    }
    if (targetIsPlayer) {
        return targetInParty ? UnitPopupMenu::Party : UnitPopupMenu::Player;
    }
    return UnitPopupMenu::RaidTargetIcon;
}

} // namespace vd::game
