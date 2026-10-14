#pragma once

#include "core/Types.h"
#include "game/Entities.h"
#include "game/Reactions.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>

namespace vd::game {

// What the update fields say about a unit or object (phase 7): the server's flag values
// (Objects/UnitDefines.h, SharedDefines.h, Objects/Player.h, Objects/GameObjectDefines.h, build 5875).

// UNIT_FIELD_FLAGS.
constexpr u32 kUnitFlagSpawning = 0x00000002;
constexpr u32 kUnitFlagPlayerControlled = 0x00000008; // players, pets, totems, charmed units
constexpr u32 kUnitFlagNotAttackable1 = 0x00000080;
constexpr u32 kUnitFlagImmuneToPlayer = 0x00000100;
constexpr u32 kUnitFlagLooting = 0x00000400;
constexpr u32 kUnitFlagPvp = 0x00001000;
constexpr u32 kUnitFlagNonAttackable2 = 0x00010000;
constexpr u32 kUnitFlagInCombat = 0x00080000;
constexpr u32 kUnitFlagTaxiFlight = 0x00100000;
constexpr u32 kUnitFlagNotSelectable = 0x02000000;
constexpr u32 kUnitFlagSkinnable = 0x04000000;
// UNIT_DYNAMIC_FLAGS (computed per viewer by the server).
constexpr u32 kDynFlagLootable = 0x01;
constexpr u32 kDynFlagTapped = 0x04;
constexpr u32 kDynFlagTappedByPlayer = 0x08; // by this viewer (or its group)
constexpr u32 kDynFlagDead = 0x20;           // feign death / "appears dead" only
// UNIT_NPC_FLAGS.
constexpr u32 kNpcFlagGossip = 0x0001;
constexpr u32 kNpcFlagQuestGiver = 0x0002;
constexpr u32 kNpcFlagVendor = 0x0004;
constexpr u32 kNpcFlagFlightMaster = 0x0008;
constexpr u32 kNpcFlagTrainer = 0x0010;
constexpr u32 kNpcFlagSpiritHealer = 0x0020;
constexpr u32 kNpcFlagSpiritGuide = 0x0040;
constexpr u32 kNpcFlagInnkeeper = 0x0080;
constexpr u32 kNpcFlagBanker = 0x0100;
constexpr u32 kNpcFlagRepair = 0x4000;
// UNIT_FIELD_BYTES_2, byte 0 (SheathState).
constexpr u8 kSheathNone = 0;
constexpr u8 kSheathMelee = 1;
constexpr u8 kSheathRanged = 2;
// PLAYER_FLAGS.
constexpr u32 kPlayerFlagGhost = 0x0010;
constexpr u32 kPlayerFlagResting = 0x0020;
constexpr u32 kPlayerFlagFreeForAllPvp = 0x0080;
// PLAYER_BYTES_2, byte 3 (PLAYER_BYTES_2_OFFSET_REST_STATE): the experience bar's color.
constexpr u8 kRestStateRested = 0x01;
constexpr u8 kRestStateNormal = 0x02;
// PLAYER_FIELD_BYTES, byte 0.
constexpr u8 kPlayerBytesReleaseTimer = 0x08; // the auto-release countdown runs
// CORPSE_FIELD_DYNAMIC_FLAGS.
constexpr u32 kCorpseDynFlagLootable = 0x01;
// GameobjectTypes (GAMEOBJECT_TYPE_ID).
constexpr u32 kGameObjectDoor = 0;
constexpr u32 kGameObjectButton = 1;
constexpr u32 kGameObjectQuestGiver = 2;
constexpr u32 kGameObjectChest = 3;
constexpr u32 kGameObjectGeneric = 5;
constexpr u32 kGameObjectGoober = 10;
// GAMEOBJECT_FLAGS / GAMEOBJECT_DYN_FLAGS.
constexpr u32 kGameObjectFlagNoInteract = 0x10;
constexpr u32 kGameObjectDynActivate = 0x01; // interactable / quest sparkle for this viewer

// Inventory slots: PLAYER_FIELD_INV_SLOT_HEAD + 2 * slot holds the item GUID (Objects/Player.h).
constexpr u8 kInventoryBackpack = 255; // INVENTORY_SLOT_BAG_0: the "bag" of the backpack and equipment slots
constexpr u8 kBagSlotStart = 19;       // bags worn: 19..22
constexpr u8 kBagSlotEnd = 23;
constexpr u8 kBackpackSlotStart = 23; // backpack: 23..38
constexpr u8 kBackpackSlotEnd = 39;
constexpr f32 kInteractionDistance = 5.0f; // INTERACTION_DISTANCE (plus both bounding radii on the server)

// A 64-bit GUID stored in two consecutive fields.
u64 guidValue(const Entity& e, u16 index);

bool isUnit(const Entity& e); // unit or player
// Yards between two objects where they are at `nowMs`: along their splines (`position` is only where the last one
// started: a creature that chased the player and stopped is still there).
f32 distanceAt(const Entity& a, const Entity& b, u32 nowMs);
// Real death: health 0 (UNIT_DYNFLAG_DEAD only marks feigned / scripted deaths).
bool isDead(const Entity& unit);
bool inCombat(const Entity& unit);
// Lootable by this client: UNIT_DYNFLAG_LOOTABLE (the server clears it for whoever may not loot), or the
// corpse object's lootable flag.
bool isLootable(const Entity& e);
bool isSkinnable(const Entity& unit);
// Tapped by someone who is not this client (nor its group): grey name, no loot, no experience.
bool isTappedByOther(const Entity& unit);
bool isTappedByMe(const Entity& unit);
u32 npcFlags(const Entity& unit);
bool isGhost(const Entity& player);
// UnitPlayerControlled: a player, or a unit a player controls (pet, totem, charmed creature).
bool isPlayerControlled(const Entity& unit);
// UnitIsPVP: flagged for PvP (UNIT_FLAG_PVP). UnitIsPVPFreeForAll: a player in a free-for-all area.
bool isPvpFlagged(const Entity& unit);
bool isFreeForAllPvp(const Entity& unit);
// IsResting (PLAYER_FLAGS_RESTING): in an inn or a city, gaining rested experience.
bool isResting(const Entity& player);
// A quest giver whose "!" / "?" mark matters: a unit with the quest giver flag or a quest giver game object.
bool isQuestGiver(const Entity& e);
u32 gameObjectType(const Entity& go);

// How `target` looks to `me` (faction templates; neutral without the table).
Reaction reactionTo(const Entity& me, const Entity& target, const Reactions* reactions);
// Whether `me` may start a fight with `target` (auto attack, harmful spells), as CMSG_ATTACKSWING and
// Unit::IsTargetableBy decide: not friendly, alive, selectable and attackable by players; players (and what
// they control) only by the PvP rules of mayFight.
bool canAttack(const Entity& me, const Entity& target, const Reactions* reactions);
// The client's UnitCanAttack(attacker, target) for colours and levels: enemies by faction alone, whatever
// their health or selection flags (a dead enemy keeps its colours). Between players and what they control,
// the server's PvP rules (WorldObject::IsValidAttackTarget): dueling each other, both in a free-for-all area,
// or not friendly with the target flagged for PvP.
bool mayFight(const Entity& attacker, const Entity& target, const Reactions* reactions);
// The colour class of a unit's name over its head, in tooltips and behind the target frame's name, for every
// view to share (TargetFrame_CheckFaction / GameTooltip_UnitColor in the client's FrameXML). Units players
// control (UnitPlayerControlled) go by who may fight whom: both ways Hostile (red); only `me` may attack it
// Neutral (yellow); flagged for PvP and on `me`'s side Friendly (green); otherwise, whichever faction,
// FriendlyPlayer (blue behind the target frame's name, white in the tooltip). Other units: Tapped (grey) when
// someone else tapped them, else their faction's reaction.
enum class NameColor : u8 { Hostile, Neutral, Friendly, FriendlyPlayer, Tapped };
NameColor unitNameColor(const Entity& me, const Entity& unit, const Reactions* reactions);
// Two players fighting a duel against each other (PLAYER_DUEL_ARBITER / PLAYER_DUEL_TEAM, public fields).
bool inDuelWith(const Entity& me, const Entity& other);

// An item in the player's backpack or bags (the server creates the item objects for their owner).
struct InventoryItem {
    u64 guid = 0;
    u32 entry = 0;
    u32 count = 0;
    // As CMSG_USE_ITEM wants them: bag 255 and slot 23..38 in the backpack, else the bag's inventory slot
    // (19..22) and the slot in it.
    u8 bag = kInventoryBackpack;
    u8 slot = 0;
};
// Calls f(const InventoryItem&) for every item in the backpack and the bags whose objects are known.
template <typename F> void forEachInventoryItem(const EntityStore& store, const Entity& player, F&& f);
// How many of `entry` the player carries (backpack + bags).
u32 inventoryCount(const EntityStore& store, const Entity& player, u32 entry);
// The first stack of `entry`; false if none.
bool findInventoryItem(const EntityStore& store, const Entity& player, u32 entry, InventoryItem& out);

// -------------------------------------------------------------------------------------------------------

template <typename F> void forEachInventoryItem(const EntityStore& store, const Entity& player, F&& f) {
    auto visit = [&](u64 guid, u8 bag, u8 slot) {
        if (guid == 0) {
            return;
        }
        const Entity* item = store.find(guid);
        if (!item || (item->type != ObjectType::Item && item->type != ObjectType::Container)) {
            return;
        }
        InventoryItem info;
        info.guid = guid;
        info.entry = item->value(net::uf::OBJECT_FIELD_ENTRY);
        info.count = item->value(net::uf::ITEM_FIELD_STACK_COUNT);
        info.bag = bag;
        info.slot = slot;
        f(static_cast<const InventoryItem&>(info));
    };
    for (u8 slot = kBackpackSlotStart; slot < kBackpackSlotEnd; ++slot) {
        visit(guidValue(player, static_cast<u16>(net::uf::PLAYER_FIELD_INV_SLOT_HEAD + slot * 2)),
              kInventoryBackpack, slot);
    }
    for (u8 bagSlot = kBagSlotStart; bagSlot < kBagSlotEnd; ++bagSlot) {
        const Entity* bag = store.find(
            guidValue(player, static_cast<u16>(net::uf::PLAYER_FIELD_INV_SLOT_HEAD + bagSlot * 2)));
        if (!bag || bag->type != ObjectType::Container) {
            continue;
        }
        const u32 size = std::min<u32>(bag->value(net::uf::CONTAINER_FIELD_NUM_SLOTS), 36);
        for (u32 i = 0; i < size; ++i) {
            visit(guidValue(*bag, static_cast<u16>(net::uf::CONTAINER_FIELD_SLOT_1 + i * 2)), bagSlot,
                  static_cast<u8>(i));
        }
    }
}

} // namespace vd::game
