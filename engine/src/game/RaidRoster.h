#pragma once

#include "core/Types.h"
#include "game/Social.h"

#include <array>
#include <span>

namespace vd::game {

// Phase 16: the rules of the raid tab (Blizzard_RaidUI.lua's RaidGroupFrame_Update,
// RaidGroupButton_OnDragStop, RaidFrame.lua, Fonts.xml's RAID_CLASS_COLORS) over the roster
// game::Social::raidMembers() gives in GetRaidRosterInfo's order. Pure functions (unit tested):
// app/RaidScreens draws with them. No allocation.

constexpr u32 kMaxRaidMembers = 40; // MAX_RAID_MEMBERS
constexpr u32 kRaidGroups = 8;      // NUM_RAID_GROUPS
constexpr u32 kRaidGroupSize = 5;   // MEMBERS_PER_RAID_GROUP
constexpr u32 kRaidSlots = kRaidGroups * kRaidGroupSize;

// RaidGroupFrame_Update's buttons: each member (roster index, raid1 = 0) takes the next free slot of its
// subgroup in roster order; a sixth one of a group ("to prevent errors when the server hiccups") gets no
// button. A slot is group * 5 + place (place 0..4, RaidGroupNSlot1..5).
struct RaidSlots {
    std::array<i8, kMaxRaidMembers> slotOf{}; // member -> slot, -1 without a button
    std::array<i8, kRaidSlots> memberIn{};    // slot -> member, -1 empty ("Empty")
    std::array<u8, kRaidGroups> filled{};     // RaidGroupN.nextIndex - 1: the slots taken in each group
};
void layoutRaidGroups(std::span<const PartyMemberView> roster, RaidSlots& out);

// RaidGroupButton_OnDragStop: a member's button dropped on a slot (TARGET_RAID_SLOT, -1: none). In another
// group, an occupied slot swaps the two (SwapRaidSubgroup), an empty one moves the member there
// (SetRaidSubgroup); in its own group, or nowhere, the button goes back to its slot.
enum class RaidDropKind : u8 { None, Move, Swap };
struct RaidDrop {
    RaidDropKind kind = RaidDropKind::None;
    u8 subgroup = 0; // Move: the subgroup 0..7
    i32 other = -1;  // Swap: the other member (roster index)
};
RaidDrop raidDrop(const RaidSlots& slots, std::span<const PartyMemberView> roster, u32 member, i32 slot);

// RAID_CLASS_COLORS (FrameXML Fonts.xml) for a ChrClasses id, as RGBA (ui::rgba order); 0 for a class it
// lacks (0: not known yet), whose text keeps its font's colour. 1.12's table gives the shaman the paladin's
// pink.
u32 raidClassColor(u8 cls);

// GetRaidRosterInfo's online and isDead (WoW.exe 0x4BB560): a member the client sees is online, and dead at 0
// health (a ghost has 1: not dead); one it does not see takes them from its group status
// (MEMBER_STATUS_ONLINE, MEMBER_STATUS_DEAD). Offline members are never dead.
bool raidRosterDead(const PartyMemberView& m);

// RaidFrame_Update: "Convert To Raid" shows out of a raid, enabled for the leader of a party with someone in
// it (GetPartyMember(1) and IsPartyLeader()).
bool canConvertToRaid(const GroupState& group, u64 player);

} // namespace vd::game
