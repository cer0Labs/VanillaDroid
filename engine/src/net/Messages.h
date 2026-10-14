#pragma once

#include "core/Types.h"
#include "net/ByteBuffer.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

// Parsers for the world messages the client needs, as vmangos-core writes them (build 5875). Each returns
// nullopt on a malformed packet instead of throwing.
namespace vd::net {

// MOVEFLAG_* (src/game/Objects/MovementInfo.h) that change the MovementInfo layout.
constexpr u32 kMoveFlagForward = 0x00000001;
constexpr u32 kMoveFlagJumping = 0x00002000;
constexpr u32 kMoveFlagSwimming = 0x00200000;
constexpr u32 kMoveFlagSplineEnabled = 0x00400000;
constexpr u32 kMoveFlagOnTransport = 0x02000000;
constexpr u32 kMoveFlagSplineElevation = 0x04000000;

struct Position {
    f32 x = 0.0f, y = 0.0f, z = 0.0f, o = 0.0f;
};

// MovementInfo::Write (Object.cpp).
struct MovementInfo {
    u32 flags = 0;
    u32 time = 0;
    Position position;
    u64 transportGuid = 0;
    Position transportPosition;
    f32 pitch = 0.0f;
    u32 fallTime = 0;
    f32 jumpVelocity = 0.0f, jumpSin = 0.0f, jumpCos = 0.0f, jumpXySpeed = 0.0f;
    f32 splineElevation = 0.0f;
};
MovementInfo readMovementInfo(ByteReader& r);
void writeMovementInfo(ByteWriter& w, const MovementInfo& info);

// SMSG_AUTH_RESPONSE: the result, and the queue position when it is AUTH_WAIT_QUEUE.
struct AuthResponse {
    u8 result = 0;
    u32 queuePosition = 0;
};
std::optional<AuthResponse> parseAuthResponse(std::span<const u8> payload);

// SMSG_CHAR_ENUM (Player::BuildEnumData).
struct CharacterInfo {
    u64 guid = 0;
    std::string name;
    u8 race = 0, cls = 0, gender = 0;
    u8 skin = 0, face = 0, hairStyle = 0, hairColor = 0, facialHair = 0;
    u8 level = 0;
    u32 zone = 0, map = 0;
    f32 x = 0.0f, y = 0.0f, z = 0.0f;
    u32 guild = 0;
    u32 flags = 0;
    bool firstLogin = false;
    u32 petDisplayId = 0, petLevel = 0, petFamily = 0;
    std::array<u32, 20> equipmentDisplay{}; // equipment slots + first bag
    std::array<u8, 20> equipmentType{};
};
std::optional<std::vector<CharacterInfo>> parseCharEnum(std::span<const u8> payload);

// SMSG_LOGIN_VERIFY_WORLD.
struct LoginVerifyWorld {
    u32 map = 0;
    Position position;
};
std::optional<LoginVerifyWorld> parseLoginVerifyWorld(std::span<const u8> payload);

// SMSG_ITEM_QUERY_SINGLE_RESPONSE (ItemHandler.cpp HandleItemQuerySingleOpcode, build 5875): the whole item
// template. `found` = false when the server does not know the entry (it answers entry | 0x80000000).
struct ItemQueryInfo {
    u32 entry = 0;
    bool found = false;
    u32 itemClass = 0; // ItemClass (game/ItemDefs.h)
    u32 subclass = 0;  // 0 for consumables (the server hides their subclass)
    std::string name;
    u32 displayId = 0;
    u32 quality = 0;
    u32 inventoryType = 0; // InventoryType (INVTYPE_*: 1 head ... 28 relic)
    u32 sheath = 0;
    // Phase 7: what using or showing the item needs.
    u32 requiredLevel = 0;
    u32 stackable = 0;
    struct Spell {
        u32 id = 0;
        u32 trigger = 0;      // ITEM_SPELLTRIGGER_*: 0 on use, 1 on equip, 2 chance on hit
        i32 charges = 0;      // < 0: the item is used up with its last charge
        i32 cooldownMs = -1;  // -1: the spell's own
        u32 category = 0;     // SpellCategory.dbc
        i32 categoryCooldownMs = -1;
    };
    std::array<Spell, 5> spells{};
    u32 bonding = 0;    // ItemBondingType (game/ItemDefs.h)
    u32 startQuest = 0; // using the item offers this quest
    // Phase 8: everything else the server sends (tooltips, vendors, bags, durability...).
    u32 flags = 0;          // ItemPrototypeFlags (game/ItemDefs.h)
    u32 buyPrice = 0;       // copper, before reputation discounts
    u32 sellPrice = 0;      // copper; 0: vendors do not buy it
    u32 allowableClass = 0; // mask of 1 << (class - 1); all the classes' bits: anyone
    u32 allowableRace = 0;  // mask of 1 << (race - 1)
    u32 itemLevel = 0;
    u32 requiredSkill = 0; // SkillLine.dbc
    u32 requiredSkillRank = 0;
    u32 requiredSpell = 0;
    u32 requiredHonorRank = 0;
    u32 requiredCityRank = 0;
    u32 requiredReputationFaction = 0; // Faction.dbc
    u32 requiredReputationRank = 0;    // ReputationRank (0 hated .. 7 exalted)
    u32 maxCount = 0;                  // "Unique": how many one may carry (0: no limit)
    u32 containerSlots = 0;            // bags
    struct Stat {
        u32 type = 0; // ItemModType
        i32 value = 0;
    };
    std::array<Stat, 10> stats{};
    struct Damage {
        f32 min = 0.0f;
        f32 max = 0.0f;
        u32 school = 0; // SpellSchools (0 physical)
    };
    std::array<Damage, 5> damages{};
    i32 armor = 0;
    std::array<i32, 6> resistances{}; // holy, fire, nature, frost, shadow, arcane
    u32 delayMs = 0;                  // weapon speed
    u32 ammoType = 0;                 // projectile subclass a ranged weapon fires
    f32 rangedModRange = 0.0f;
    std::string description; // flavour text (in quotes on the tooltip)
    u32 pageText = 0;        // first PageText of a book or letter
    u32 language = 0;
    u32 pageMaterial = 0;
    u32 lockId = 0;         // Lock.dbc: a locked box
    i32 material = 0;       // Material.dbc (-1 consumables)
    u32 randomProperty = 0; // ItemRandomProperties.dbc: "of the Bear" items
    u32 block = 0;          // shield block value
    u32 itemSet = 0;        // ItemSet.dbc
    u32 maxDurability = 0;
    u32 area = 0;      // AreaTable.dbc: only usable there
    u32 map = 0;       // Map.dbc: only usable there
    u32 bagFamily = 0; // BagFamily: what a special bag holds, or the family an item belongs to
};
constexpr u32 kItemSpellTriggerOnUse = 0;
constexpr u32 kItemSpellTriggerOnEquip = 1;
constexpr u32 kItemSpellTriggerChanceOnHit = 2;
std::optional<ItemQueryInfo> parseItemQueryResponse(std::span<const u8> payload);

// SMSG_NAME_QUERY_RESPONSE (Query.cpp, build 5875: with the realm name).
struct NameQueryInfo {
    u64 guid = 0;
    std::string name;
    u32 race = 0, gender = 0, cls = 0;
};
std::optional<NameQueryInfo> parseNameQueryResponse(std::span<const u8> payload);

// SMSG_CREATURE_QUERY_RESPONSE (Query.cpp): name, title and classification of a creature entry.
struct CreatureQueryInfo {
    u32 entry = 0;
    bool found = false;
    std::string name;
    std::string subname;
    u32 typeFlags = 0, type = 0, family = 0, rank = 0;
    u32 displayId = 0; // phase 15 (pets): its first model (CreatureDisplayInfo)
};
std::optional<CreatureQueryInfo> parseCreatureQueryResponse(std::span<const u8> payload);

} // namespace vd::net
