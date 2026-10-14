#include "net/GameMessages.h"

namespace vd::net {
namespace {

// String preceded by its length including the NUL (u32 len, then len bytes ending in 0), as BuildChatPacket,
// SMSG_RESURRECT_REQUEST, SMSG_AREA_TRIGGER_MESSAGE and SMSG_TEXT_EMOTE write it.
std::string_view lengthString(ByteReader& r) {
    const u32 length = r.u32v();
    if (r.failed() || length == 0 || length > r.remaining()) {
        r.fail();
        return {};
    }
    const std::span<const u8> bytes = r.view(length);
    if (bytes.back() != 0) {
        r.fail();
        return {};
    }
    return {reinterpret_cast<const char*>(bytes.data()), length - 1};
}

void readVec3(ByteReader& r, std::array<f32, 3>& out) {
    out[0] = r.f32v();
    out[1] = r.f32v();
    out[2] = r.f32v();
}

void readQuestItems(ByteReader& r, u32 count, u32 max, u32& outCount, QuestItem* out, bool withDisplay) {
    outCount = 0;
    if (count > max) {
        r.fail();
        return;
    }
    for (u32 i = 0; i < count && r.ok(); ++i) {
        out[i].itemId = r.u32v();
        out[i].count = r.u32v();
        out[i].displayId = withDisplay ? r.u32v() : 0;
    }
    outCount = r.ok() ? count : 0;
}

constexpr const char* kSpellCastResultNames[] = {
    "AFFECTING_COMBAT",
    "ALREADY_AT_FULL_HEALTH",
    "ALREADY_AT_FULL_POWER",
    "ALREADY_BEING_TAMED",
    "ALREADY_HAVE_CHARM",
    "ALREADY_HAVE_SUMMON",
    "ALREADY_OPEN",
    "AURA_BOUNCED",
    "AUTOTRACK_INTERRUPTED",
    "BAD_IMPLICIT_TARGETS",
    "BAD_TARGETS",
    "CANT_BE_CHARMED",
    "CANT_BE_DISENCHANTED",
    "CANT_BE_PROSPECTED",
    "CANT_CAST_ON_TAPPED",
    "CANT_DUEL_WHILE_INVISIBLE",
    "CANT_DUEL_WHILE_STEALTHED",
    "CANT_STEALTH",
    "CASTER_AURASTATE",
    "CASTER_DEAD",
    "CHARMED",
    "CHEST_IN_USE",
    "CONFUSED",
    "DONT_REPORT",
    "EQUIPPED_ITEM",
    "EQUIPPED_ITEM_CLASS",
    "EQUIPPED_ITEM_CLASS_MAINHAND",
    "EQUIPPED_ITEM_CLASS_OFFHAND",
    "ERROR",
    "FIZZLE",
    "FLEEING",
    "FOOD_LOWLEVEL",
    "HIGHLEVEL",
    "HUNGER_SATIATED",
    "IMMUNE",
    "INTERRUPTED",
    "INTERRUPTED_COMBAT",
    "ITEM_ALREADY_ENCHANTED",
    "ITEM_GONE",
    "ITEM_NOT_FOUND",
    "ITEM_NOT_READY",
    "LEVEL_REQUIREMENT",
    "LINE_OF_SIGHT",
    "LOWLEVEL",
    "LOW_CASTLEVEL",
    "MAINHAND_EMPTY",
    "MOVING",
    "NEED_AMMO",
    "NEED_AMMO_POUCH",
    "NEED_EXOTIC_AMMO",
    "NOPATH",
    "NOT_BEHIND",
    "NOT_FISHABLE",
    "NOT_HERE",
    "NOT_INFRONT",
    "NOT_IN_CONTROL",
    "NOT_KNOWN",
    "NOT_MOUNTED",
    "NOT_ON_TAXI",
    "NOT_ON_TRANSPORT",
    "NOT_READY",
    "NOT_SHAPESHIFT",
    "NOT_STANDING",
    "NOT_TRADEABLE",
    "NOT_TRADING",
    "NOT_UNSHEATHED",
    "NOT_WHILE_GHOST",
    "NO_AMMO",
    "NO_CHARGES_REMAIN",
    "NO_CHAMPION",
    "NO_COMBO_POINTS",
    "NO_DUELING",
    "NO_ENDURANCE",
    "NO_FISH",
    "NO_ITEMS_WHILE_SHAPESHIFTED",
    "NO_MOUNTS_ALLOWED",
    "NO_PET",
    "NO_POWER",
    "NOTHING_TO_DISPEL",
    "NOTHING_TO_STEAL",
    "ONLY_ABOVEWATER",
    "ONLY_DAYTIME",
    "ONLY_INDOORS",
    "ONLY_MOUNTED",
    "ONLY_NIGHTTIME",
    "ONLY_OUTDOORS",
    "ONLY_SHAPESHIFT",
    "ONLY_STEALTHED",
    "ONLY_UNDERWATER",
    "OUT_OF_RANGE",
    "PACIFIED",
    "POSSESSED",
    "REAGENTS",
    "REQUIRES_AREA",
    "REQUIRES_SPELL_FOCUS",
    "ROOTED",
    "SILENCED",
    "SPELL_IN_PROGRESS",
    "SPELL_LEARNED",
    "SPELL_UNAVAILABLE",
    "STUNNED",
    "TARGETS_DEAD",
    "TARGET_AFFECTING_COMBAT",
    "TARGET_AURASTATE",
    "TARGET_DUELING",
    "TARGET_ENEMY",
    "TARGET_ENRAGED",
    "TARGET_FRIENDLY",
    "TARGET_IN_COMBAT",
    "TARGET_IS_PLAYER",
    "TARGET_NOT_DEAD",
    "TARGET_NOT_IN_PARTY",
    "TARGET_NOT_LOOTED",
    "TARGET_NOT_PLAYER",
    "TARGET_NO_POCKETS",
    "TARGET_NO_WEAPONS",
    "TARGET_UNSKINNABLE",
    "THIRST_SATIATED",
    "TOO_CLOSE",
    "TOO_MANY_OF_ITEM",
    "TOTEMS",
    "TRAINING_POINTS",
    "TRY_AGAIN",
    "UNIT_NOT_BEHIND",
    "UNIT_NOT_INFRONT",
    "WRONG_PET_FOOD",
    "NOT_WHILE_FATIGUED",
    "TARGET_NOT_IN_INSTANCE",
    "NOT_WHILE_TRADING",
    "TARGET_NOT_IN_RAID",
    "DISENCHANT_WHILE_LOOTING",
    "PROSPECT_WHILE_LOOTING",
    "PROSPECT_NEED_MORE",
    "TARGET_FREEFORALL",
    "NO_EDIBLE_CORPSES",
    "ONLY_BATTLEGROUNDS",
    "TARGET_NOT_GHOST",
    "TOO_MANY_SKILLS",
    "TRANSFORM_UNUSABLE",
    "WRONG_WEATHER",
    "DAMAGE_IMMUNE",
    "PREVENTED_BY_MECHANIC",
    "PLAY_TIME",
    "REPUTATION",
    "MIN_SKILL",
    "UNKNOWN",
};
static_assert(std::size(kSpellCastResultNames) == 146);

constexpr const char* kInventoryResultNames[] = {
    "OK",
    "CANT_EQUIP_LEVEL_I",
    "CANT_EQUIP_SKILL",
    "ITEM_DOESNT_GO_TO_SLOT",
    "BAG_FULL",
    "NONEMPTY_BAG_OVER_OTHER_BAG",
    "CANT_TRADE_EQUIP_BAGS",
    "ONLY_AMMO_CAN_GO_HERE",
    "NO_REQUIRED_PROFICIENCY",
    "NO_EQUIPMENT_SLOT_AVAILABLE",
    "YOU_CAN_NEVER_USE_THAT_ITEM",
    "YOU_CAN_NEVER_USE_THAT_ITEM2",
    "NO_EQUIPMENT_SLOT_AVAILABLE2",
    "CANT_EQUIP_WITH_TWOHANDED",
    "CANT_DUAL_WIELD",
    "ITEM_DOESNT_GO_INTO_BAG",
    "ITEM_DOESNT_GO_INTO_BAG2",
    "CANT_CARRY_MORE_OF_THIS",
    "NO_EQUIPMENT_SLOT_AVAILABLE3",
    "ITEM_CANT_STACK",
    "ITEM_CANT_BE_EQUIPPED",
    "ITEMS_CANT_BE_SWAPPED",
    "SLOT_IS_EMPTY",
    "ITEM_NOT_FOUND",
    "CANT_DROP_SOULBOUND",
    "OUT_OF_RANGE",
    "TRIED_TO_SPLIT_MORE_THAN_COUNT",
    "COULDNT_SPLIT_ITEMS",
    "MISSING_REAGENT",
    "NOT_ENOUGH_MONEY",
    "NOT_A_BAG",
    "CAN_ONLY_DO_WITH_EMPTY_BAGS",
    "DONT_OWN_THAT_ITEM",
    "CAN_EQUIP_ONLY1_QUIVER",
    "MUST_PURCHASE_THAT_BAG_SLOT",
    "TOO_FAR_AWAY_FROM_BANK",
    "ITEM_LOCKED",
    "YOU_ARE_STUNNED",
    "YOU_ARE_DEAD",
    "CANT_DO_RIGHT_NOW",
    "INT_BAG_ERROR",
    "CAN_EQUIP_ONLY1_BOLT",
    "CAN_EQUIP_ONLY1_AMMOPOUCH",
    "STACKABLE_CANT_BE_WRAPPED",
    "EQUIPPED_CANT_BE_WRAPPED",
    "WRAPPED_CANT_BE_WRAPPED",
    "BOUND_CANT_BE_WRAPPED",
    "UNIQUE_CANT_BE_WRAPPED",
    "BAGS_CANT_BE_WRAPPED",
    "ALREADY_LOOTED",
    "INVENTORY_FULL",
    "BANK_FULL",
    "ITEM_IS_CURRENTLY_SOLD_OUT",
    "BAG_FULL3",
    "ITEM_NOT_FOUND2",
    "ITEM_CANT_STACK2",
    "BAG_FULL4",
    "ITEM_SOLD_OUT",
    "OBJECT_IS_BUSY",
    "NONE",
    "NOT_IN_COMBAT",
    "NOT_WHILE_DISARMED",
    "BAG_FULL6",
    "CANT_EQUIP_RANK",
    "CANT_EQUIP_REPUTATION",
    "TOO_MANY_SPECIAL_BAGS",
    "LOOT_CANT_LOOT_THAT_NOW",
};
static_assert(std::size(kInventoryResultNames) == 67);

} // namespace

const char* spellCastResultName(u8 reason) {
    if (reason == 0xFF) {
        return "OK";
    }
    return reason < std::size(kSpellCastResultNames) ? kSpellCastResultNames[reason] : "UNKNOWN";
}

const char* inventoryResultName(u8 result) {
    return result < std::size(kInventoryResultNames) ? kInventoryResultNames[result] : "UNKNOWN";
}

const char* lootErrorName(u8 error) {
    switch (error) {
    case 0: return "DIDNT_KILL";
    case 4: return "TOO_FAR";
    case 5: return "BAD_FACING";
    case 6: return "LOCKED";
    case 8: return "NOTSTANDING";
    case 9: return "STUNNED";
    case 10: return "PLAYER_NOT_FOUND";
    case 11: return "PLAY_TIME_EXCEEDED";
    case 12: return "MASTER_INV_FULL";
    case 13: return "MASTER_UNIQUE_ITEM";
    case 14: return "MASTER_OTHER";
    case 15: return "ALREADY_PICKPOCKETED";
    case 16: return "NOT_WHILE_SHAPESHIFTED";
    default: return "UNKNOWN";
    }
}

// -------------------------------------------------------------------------------------------------------

void readSpellCastTargets(ByteReader& r, SpellCastTargets& out) {
    out = SpellCastTargets{};
    out.mask = r.u16v();
    if (out.mask & (kTargetUnit | kTargetCorpseEnemy | kTargetGameObject | kTargetCorpseAlly)) {
        const u64 guid = r.packedGuid(); // exactly one, by priority
        if (out.mask & kTargetUnit) {
            out.unit = guid;
        } else if (out.mask & kTargetGameObject) {
            out.gameObject = guid;
        } else {
            out.corpse = guid;
        }
    }
    if (out.mask & (kTargetItem | kTargetTradeItem)) {
        out.item = r.packedGuid();
    }
    if (out.mask & kTargetSourceLocation) {
        readVec3(r, out.source);
    }
    if (out.mask & kTargetDestLocation) {
        readVec3(r, out.destination);
    }
    if (out.mask & kTargetString) {
        out.text = r.cstring();
    }
}

void writeSpellCastTargets(ByteWriter& w, const SpellCastTargets& t) {
    w.u16v(t.mask);
    if (t.mask == kTargetSelf) {
        return;
    }
    if (t.mask & kTargetUnit) {
        w.packedGuid(t.unit);
    }
    if (t.mask & kTargetGameObject) {
        w.packedGuid(t.gameObject);
    }
    if (t.mask & (kTargetCorpseEnemy | kTargetCorpseAlly)) {
        w.packedGuid(t.corpse);
    }
    if (t.mask & (kTargetItem | kTargetTradeItem)) {
        w.packedGuid(t.item);
    }
    if (t.mask & kTargetSourceLocation) {
        w.f32v(t.source[0]).f32v(t.source[1]).f32v(t.source[2]);
    }
    if (t.mask & kTargetDestLocation) {
        w.f32v(t.destination[0]).f32v(t.destination[1]).f32v(t.destination[2]);
    }
    if (t.mask & kTargetString) {
        w.cstring(t.text);
    }
}

// -------------------------------------------------------------------------------------------------------
// Melee and combat log.

bool parseAttackStart(std::span<const u8> payload, AttackStart& out) {
    ByteReader r(payload);
    out.attacker = r.u64v();
    out.victim = r.u64v();
    return r.ok();
}

bool parseAttackStop(std::span<const u8> payload, AttackStop& out) {
    ByteReader r(payload);
    out.attacker = r.packedGuid();
    out.victim = r.packedGuid();
    out.dead = r.u32v() != 0;
    return r.ok();
}

bool parseAttackerStateUpdate(std::span<const u8> payload, AttackerStateUpdate& out) {
    ByteReader r(payload);
    out.hitInfo = r.u32v();
    out.attacker = r.packedGuid();
    out.victim = r.packedGuid();
    out.totalDamage = r.i32v();
    const u8 count = r.u8v();
    out.subDamageCount = 0;
    for (u32 i = 0; i < count && r.ok(); ++i) {
        AttackerStateUpdate::SubDamage sub;
        sub.school = r.u32v();
        r.f32v(); // the same damage as a float
        sub.damage = r.i32v();
        sub.absorbed = r.i32v();
        sub.resisted = r.i32v();
        if (out.subDamageCount < AttackerStateUpdate::kMaxSubDamages) {
            out.subDamages[out.subDamageCount++] = sub;
        }
    }
    out.victimState = static_cast<VictimState>(r.u32v());
    out.attackerState = r.u32v();
    out.meleeSpellId = r.u32v();
    out.blocked = r.i32v();
    // A debug block may follow with HITINFO_DEBUG; the server never fills it and nothing here needs it.
    return r.ok();
}

bool parseSpellDamageLog(std::span<const u8> payload, SpellDamageLog& out) {
    ByteReader r(payload);
    out.target = r.packedGuid();
    out.attacker = r.packedGuid();
    out.spellId = r.u32v();
    out.damage = r.u32v();
    out.school = r.u8v();
    out.absorbed = r.u32v();
    out.resisted = r.i32v();
    out.periodic = r.u8v() != 0;
    r.u8v(); // unused
    out.blocked = r.u32v();
    out.hitType = r.u32v();
    const bool extended = r.u8v() != 0; // never set by the server
    if (extended) {
        r.skip(((out.hitType & 0x01) ? 8u : 0u) + ((out.hitType & 0x04) ? 8u : 0u));
    }
    return r.ok();
}

bool parseSpellHealLog(std::span<const u8> payload, SpellHealLog& out) {
    ByteReader r(payload);
    out.target = r.packedGuid();
    out.healer = r.packedGuid();
    out.spellId = r.u32v();
    out.amount = r.u32v();
    out.critical = r.u8v() != 0;
    return r.ok();
}

bool parseSpellEnergizeLog(std::span<const u8> payload, SpellEnergizeLog& out) {
    ByteReader r(payload);
    out.target = r.packedGuid();
    out.caster = r.packedGuid();
    out.spellId = r.u32v();
    out.powerType = r.u32v();
    out.amount = r.u32v();
    return r.ok();
}

bool parsePeriodicAuraLog(std::span<const u8> payload, PeriodicAuraLog& out) {
    ByteReader r(payload);
    out = PeriodicAuraLog{};
    out.target = r.packedGuid();
    out.caster = r.packedGuid();
    out.spellId = r.u32v();
    const u32 count = r.u32v();
    if (count != 1) {
        return false; // always 1 (Unit::SendPeriodicAuraLog)
    }
    out.auraType = r.u32v();
    switch (out.auraType) {
    case kAuraPeriodicDamage:
    case kAuraPeriodicDamagePercent:
        out.amount = r.u32v();
        out.school = r.u32v();
        out.absorbed = r.u32v();
        out.resisted = r.i32v();
        break;
    case kAuraPeriodicHeal:
    case kAuraObsModHealth: out.amount = r.u32v(); break;
    case kAuraObsModMana:
    case kAuraPeriodicEnergize:
        out.powerType = r.u32v();
        out.amount = r.u32v();
        break;
    case kAuraPeriodicManaLeech:
        out.powerType = r.u32v();
        out.amount = r.u32v();
        out.multiplier = r.f32v();
        break;
    default: return false; // the server sends no other aura type
    }
    return r.ok();
}

bool parseSpellLogMiss(std::span<const u8> payload, SpellLogMiss& out) {
    ByteReader r(payload);
    out.spellId = r.u32v();
    out.caster = r.u64v();
    const bool extended = r.u8v() != 0; // never set
    const u32 count = r.u32v();
    out.count = 0;
    if (count > r.remaining()) {
        return false; // each entry takes at least 9 bytes
    }
    for (u32 i = 0; i < count && r.ok(); ++i) {
        SpellLogMiss::Entry entry;
        entry.target = r.u64v();
        entry.miss = static_cast<SpellMiss>(r.u8v());
        if (extended) {
            r.skip(8);
        }
        if (out.count < SpellLogMiss::kMaxEntries) {
            out.entries[out.count++] = entry;
        }
    }
    return r.ok();
}

bool parseSpellImmuneLog(std::span<const u8> payload, SpellImmuneLog& out) {
    ByteReader r(payload);
    out.caster = r.u64v();
    out.target = r.u64v();
    out.spellId = r.u32v();
    r.u8v(); // log format
    return r.ok();
}

bool parseEnvironmentalDamageLog(std::span<const u8> payload, EnvironmentalDamageLog& out) {
    ByteReader r(payload);
    out.victim = r.u64v();
    out.type = r.u8v();
    out.damage = r.u32v();
    out.absorbed = r.u32v();
    out.resisted = r.i32v();
    return r.ok();
}

bool parsePartyKillLog(std::span<const u8> payload, PartyKillLog& out) {
    ByteReader r(payload);
    out.killer = r.u64v();
    out.victim = r.u64v();
    return r.ok();
}

// -------------------------------------------------------------------------------------------------------
// Spellbook, action bar, casting, cooldowns.

bool parseInitialSpells(std::span<const u8> payload, InitialSpells& out) {
    ByteReader r(payload);
    out.spells.clear();
    out.cooldowns.clear();
    r.u8v(); // talent spec, always 0
    const u16 spellCount = r.u16v();
    if (usize{spellCount} * 4 > r.remaining()) {
        return false;
    }
    out.spells.reserve(spellCount);
    for (u32 i = 0; i < spellCount; ++i) {
        out.spells.push_back(r.u16v());
        r.u16v(); // unknown, always 0
    }
    const u16 cooldownCount = r.u16v();
    if (r.failed() || usize{cooldownCount} * 14 > r.remaining()) {
        return false;
    }
    out.cooldowns.reserve(cooldownCount);
    for (u32 i = 0; i < cooldownCount; ++i) {
        InitialSpells::Cooldown c;
        c.spellId = r.u16v();
        c.itemId = r.u16v();
        c.category = r.u16v();
        c.recoveryMs = r.i32v();
        c.categoryRecoveryMs = r.i32v();
        out.cooldowns.push_back(c);
    }
    return r.ok();
}

bool parseLearnedSpell(std::span<const u8> payload, u32& spellId) {
    ByteReader r(payload);
    spellId = r.u16v();
    r.u16v(); // action bar slot, unused
    return r.ok();
}

bool parseRemovedSpell(std::span<const u8> payload, u32& spellId) {
    ByteReader r(payload);
    spellId = r.u16v();
    return r.ok();
}

bool parseSupercededSpell(std::span<const u8> payload, u32& oldSpellId, u32& newSpellId) {
    ByteReader r(payload);
    oldSpellId = r.u16v();
    newSpellId = r.u16v();
    return r.ok();
}

bool parseActionButtons(std::span<const u8> payload, std::array<u32, kActionButtonCount>& out) {
    ByteReader r(payload);
    std::array<u32, kActionButtonCount> buttons{};
    for (u32& b : buttons) {
        b = r.u32v();
    }
    if (r.failed()) {
        return false;
    }
    out = buttons;
    return true;
}

bool parseSpellStart(std::span<const u8> payload, SpellStart& out) {
    ByteReader r(payload);
    out.caster = r.packedGuid();
    out.unitCaster = r.packedGuid();
    out.spellId = r.u32v();
    out.castFlags = r.u16v();
    out.castTimeMs = r.u32v();
    readSpellCastTargets(r, out.targets);
    if (out.castFlags & 0x20) { // CAST_FLAG_AMMO
        r.u32v();               // ammo display id
        r.u32v();               // ammo inventory type
    }
    return r.ok();
}

bool parseSpellGo(std::span<const u8> payload, SpellGo& out) {
    ByteReader r(payload);
    out.hits.clear();
    out.misses.clear();
    out.caster = r.packedGuid();
    out.unitCaster = r.packedGuid();
    out.spellId = r.u32v();
    out.castFlags = r.u16v();
    const u8 hitCount = r.u8v();
    for (u32 i = 0; i < hitCount && r.ok(); ++i) {
        out.hits.push_back(r.u64v()); // full GUIDs here, not packed
    }
    const u8 missCount = r.u8v();
    for (u32 i = 0; i < missCount && r.ok(); ++i) {
        SpellGo::Miss miss;
        miss.target = r.u64v();
        miss.miss = static_cast<SpellMiss>(r.u8v());
        if (miss.miss == SpellMiss::Reflect) {
            r.u8v(); // reflect result
        }
        out.misses.push_back(miss);
    }
    readSpellCastTargets(r, out.targets);
    if (out.castFlags & 0x20) {
        r.u32v();
        r.u32v();
    }
    return r.ok();
}

bool parseSpellFailedOther(std::span<const u8> payload, SpellFailedOther& out) {
    ByteReader r(payload);
    out.caster = r.u64v();
    out.spellId = r.u32v();
    return r.ok();
}

bool parseCastResult(std::span<const u8> payload, CastResult& out) {
    ByteReader r(payload);
    out.spellId = r.u32v();
    out.status = r.u8v();
    out.reason = 0;
    out.argCount = 0;
    out.args = {};
    if (out.status == kCastStatusFailed) {
        out.reason = r.u8v();
        // The arguments depend on the reason (and a Spell.dbc attribute): read what is there.
        while (out.argCount < 2 && r.remaining() >= 4) {
            out.args[out.argCount++] = r.u32v();
        }
    }
    return r.ok();
}

bool parseSpellCooldown(std::span<const u8> payload, SpellCooldown& out) {
    ByteReader r(payload);
    out.entries.clear();
    out.caster = r.u64v();
    if (r.failed() || r.remaining() % 8 != 0) {
        return false;
    }
    while (r.remaining() >= 8) {
        SpellCooldown::Entry e;
        e.spellId = r.u32v();
        e.cooldownMs = r.u32v();
        out.entries.push_back(e);
    }
    return r.ok();
}

bool parseCooldownEvent(std::span<const u8> payload, u32& spellId, u64& guid) {
    ByteReader r(payload);
    spellId = r.u32v();
    guid = r.u64v();
    return r.ok();
}

bool parseSpellDelayed(std::span<const u8> payload, SpellDelayed& out) {
    ByteReader r(payload);
    out.caster = r.u64v();
    out.delayMs = r.u32v();
    return r.ok();
}

bool parseChannelStart(std::span<const u8> payload, ChannelStart& out) {
    ByteReader r(payload);
    out.spellId = r.u32v();
    out.durationMs = r.u32v();
    return r.ok();
}

bool parseChannelUpdate(std::span<const u8> payload, u32& remainingMs) {
    ByteReader r(payload);
    remainingMs = r.u32v();
    return r.ok();
}

bool parseUpdateAuraDuration(std::span<const u8> payload, u8& slot, u32& durationMs) {
    ByteReader r(payload);
    slot = r.u8v();
    durationMs = r.u32v();
    return r.ok();
}

// -------------------------------------------------------------------------------------------------------
// Gossip and quests.

namespace {

bool readQuestMenu(ByteReader& r, u32 count, u32& outCount, GossipMessage::Quest* out) {
    outCount = 0;
    if (count > GossipMessage::kMaxItems) {
        return false;
    }
    for (u32 i = 0; i < count && r.ok(); ++i) {
        out[i].questId = r.u32v();
        out[i].icon = r.u32v();
        out[i].level = r.u32v();
        out[i].title = r.cstring();
    }
    outCount = r.ok() ? count : 0;
    return r.ok();
}

} // namespace

bool parseGossipMessage(std::span<const u8> payload, GossipMessage& out) {
    ByteReader r(payload);
    out.source = r.u64v();
    out.textId = r.u32v();
    const u32 optionCount = r.u32v();
    out.optionCount = 0;
    out.questCount = 0;
    if (r.failed() || optionCount > GossipMessage::kMaxItems) {
        return false;
    }
    for (u32 i = 0; i < optionCount && r.ok(); ++i) {
        GossipMessage::Option& o = out.options[i];
        o.index = r.u32v();
        o.icon = r.u8v();
        o.coded = r.u8v() != 0;
        o.text = r.cstring();
    }
    if (r.failed()) {
        return false;
    }
    out.optionCount = optionCount;
    const u32 questCount = r.u32v();
    return r.ok() && readQuestMenu(r, questCount, out.questCount, out.quests.data());
}

bool parseNpcTextUpdate(std::span<const u8> payload, NpcTextUpdate& out) {
    ByteReader r(payload);
    out.textId = r.u32v();
    for (NpcTextUpdate::Option& o : out.options) {
        o.probability = r.f32v();
        o.maleText = r.cstring();
        o.femaleText = r.cstring();
        o.language = r.u32v();
        for (u32 i = 0; i < 3; ++i) {
            o.emoteDelays[i] = r.u32v();
            o.emotes[i] = r.u32v();
        }
    }
    return r.ok();
}

bool parseQuestGiverStatus(std::span<const u8> payload, u64& guid, u32& status) {
    ByteReader r(payload);
    guid = r.u64v();
    status = r.u32v();
    return r.ok();
}

bool parseQuestGiverQuestList(std::span<const u8> payload, QuestGiverQuestList& out) {
    ByteReader r(payload);
    out.questGiver = r.u64v();
    out.greeting = r.cstring();
    out.emoteDelay = r.u32v();
    out.emote = r.u32v();
    const u8 count = r.u8v();
    out.questCount = 0;
    return r.ok() && readQuestMenu(r, count, out.questCount, out.quests.data());
}

bool parseQuestDetails(std::span<const u8> payload, QuestDetails& out) {
    ByteReader r(payload);
    out.questGiver = r.u64v();
    out.questId = r.u32v();
    out.title = r.cstring();
    out.details = r.cstring();
    out.objectives = r.cstring();
    out.activateAccept = r.u32v() != 0;
    // With hidden rewards the two counts are 0: the same reading covers both forms.
    readQuestItems(r, r.u32v(), kQuestChoiceCount, out.choiceCount, out.choices.data(), true);
    readQuestItems(r, r.u32v(), kQuestRewardCount, out.rewardCount, out.rewards.data(), true);
    out.money = r.i32v();
    out.rewardSpell = r.u32v();
    const u32 emoteCount = r.u32v();
    if (r.failed() || emoteCount > 4) {
        return false;
    }
    out.emotes = {};
    out.emoteDelays = {};
    for (u32 i = 0; i < emoteCount; ++i) {
        out.emotes[i] = r.u32v(); // emote, then delay (the opposite of OFFER_REWARD)
        out.emoteDelays[i] = r.u32v();
    }
    return r.ok();
}

bool parseQuestRequestItems(std::span<const u8> payload, QuestRequestItems& out) {
    ByteReader r(payload);
    out.questGiver = r.u64v();
    out.questId = r.u32v();
    out.title = r.cstring();
    out.text = r.cstring();
    out.emoteDelay = r.u32v();
    out.emote = r.u32v();
    out.closeOnCancel = r.u32v() != 0;
    out.requiredMoney = r.u32v();
    readQuestItems(r, r.u32v(), kQuestObjectiveCount, out.itemCount, out.items.data(), true);
    bool completable = true;
    for (u32 i = 0; i < 4; ++i) {
        completable = r.u32v() != 0 && completable; // all four must be non-zero
    }
    out.completable = completable;
    return r.ok();
}

bool parseQuestOfferReward(std::span<const u8> payload, QuestOfferReward& out) {
    ByteReader r(payload);
    out.questGiver = r.u64v();
    out.questId = r.u32v();
    out.title = r.cstring();
    out.text = r.cstring();
    out.enableNext = r.u32v() != 0;
    const u32 emoteCount = r.u32v();
    if (r.failed() || emoteCount > 4) {
        return false;
    }
    out.emoteCount = emoteCount;
    out.emotes = {};
    out.emoteDelays = {};
    for (u32 i = 0; i < emoteCount; ++i) {
        out.emoteDelays[i] = r.u32v(); // delay, then emote
        out.emotes[i] = r.u32v();
    }
    readQuestItems(r, r.u32v(), kQuestChoiceCount, out.choiceCount, out.choices.data(), true);
    readQuestItems(r, r.u32v(), kQuestRewardCount, out.rewardCount, out.rewards.data(), true);
    out.money = r.i32v();
    out.flags = r.u32v();
    out.rewardSpell = r.u32v();
    return r.ok();
}

bool parseQuestComplete(std::span<const u8> payload, QuestComplete& out) {
    ByteReader r(payload);
    out.questId = r.u32v();
    r.u32v(); // always 3
    out.xp = r.u32v();
    out.money = r.i32v();
    readQuestItems(r, r.u32v(), static_cast<u32>(out.items.size()), out.itemCount, out.items.data(), false);
    return r.ok();
}

bool parseQuestGiverQuestFailed(std::span<const u8> payload, u32& questId, u32& reason) {
    ByteReader r(payload);
    questId = r.u32v();
    reason = r.u32v();
    return r.ok();
}

bool parseQuestGiverQuestInvalid(std::span<const u8> payload, u32& reason) {
    return parseU32(payload, reason);
}

bool parseQuestQueryResponse(std::span<const u8> payload, QuestQueryResponse& out) {
    ByteReader r(payload);
    out.questId = r.u32v();
    out.method = r.u32v();
    out.level = r.u32v();
    out.zoneOrSort = r.i32v();
    out.type = r.u32v();
    out.repObjectiveFaction = r.u32v();
    out.repObjectiveValue = r.i32v();
    r.u32v(); // required opposite reputation faction, always 0
    r.u32v(); // and value
    out.nextQuestInChain = r.u32v();
    out.money = r.i32v();
    out.moneyMaxLevel = r.u32v();
    out.rewardSpell = r.u32v();
    out.sourceItemId = r.u32v();
    out.flags = r.u32v();
    for (QuestItem& item : out.rewards) {
        item.itemId = r.u32v();
        item.count = r.u32v();
        item.displayId = 0;
    }
    for (QuestItem& item : out.choices) {
        item.itemId = r.u32v();
        item.count = r.u32v();
        item.displayId = 0;
    }
    out.pointMapId = r.u32v();
    out.pointX = r.f32v();
    out.pointY = r.f32v();
    out.pointOpt = r.u32v();
    out.title = r.cstring(); // title, objectives, details, end text (not QUEST_DETAILS' order)
    out.objectives = r.cstring();
    out.details = r.cstring();
    out.endText = r.cstring();
    for (QuestQueryResponse::Objective& o : out.objectiveList) {
        o.creatureOrGo = r.u32v();
        o.creatureOrGoCount = r.u32v();
        o.itemId = r.u32v();
        o.itemCount = r.u32v();
    }
    for (QuestQueryResponse::Objective& o : out.objectiveList) {
        o.text = r.cstring();
    }
    return r.ok();
}

bool parseQuestUpdateAddKill(std::span<const u8> payload, QuestUpdateAddKill& out) {
    ByteReader r(payload);
    out.questId = r.u32v();
    out.entry = r.u32v();
    out.count = r.u32v();
    out.required = r.u32v();
    out.source = r.u64v();
    return r.ok();
}

bool parseQuestUpdateAddItem(std::span<const u8> payload, u32& itemId, u32& added) {
    ByteReader r(payload);
    itemId = r.u32v();
    added = r.u32v();
    return r.ok();
}

bool parseQuestId(std::span<const u8> payload, u32& questId) {
    return parseU32(payload, questId);
}

bool parseQuestConfirmAccept(std::span<const u8> payload, QuestConfirmAccept& out) {
    ByteReader r(payload);
    out.questId = r.u32v();
    out.title = r.cstring();
    out.sharer = r.u64v();
    return r.ok();
}

// -------------------------------------------------------------------------------------------------------
// Loot and items.

bool parseLootResponse(std::span<const u8> payload, LootResponse& out) {
    ByteReader r(payload);
    out.items.clear();
    out.gold = 0;
    out.error = 0;
    out.guid = r.u64v();
    out.lootType = r.u8v();
    if (out.lootType == 0) {
        out.error = r.u8v();
        return r.ok();
    }
    out.gold = r.u32v();
    const u8 count = r.u8v();
    if (r.failed() || usize{count} * 22 > r.remaining()) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        LootResponse::Item item;
        item.slot = r.u8v();
        item.itemId = r.u32v();
        item.count = r.u32v();
        item.displayId = r.u32v();
        item.randomSuffix = r.u32v();
        item.randomPropertyId = r.u32v();
        item.slotType = r.u8v();
        out.items.push_back(item);
    }
    return r.ok();
}

bool parseLootReleaseResponse(std::span<const u8> payload, u64& guid) {
    ByteReader r(payload);
    guid = r.u64v();
    r.u8v(); // always 1
    return r.ok();
}

bool parseLootRemoved(std::span<const u8> payload, u8& slot) {
    ByteReader r(payload);
    slot = r.u8v();
    return r.ok();
}

bool parseU32(std::span<const u8> payload, u32& value) {
    ByteReader r(payload);
    value = r.u32v();
    return r.ok();
}

bool parseItemPushResult(std::span<const u8> payload, ItemPushResult& out) {
    ByteReader r(payload);
    out.player = r.u64v();
    out.fromNpc = r.u32v() != 0;
    out.created = r.u32v() != 0;
    out.showInChat = r.u32v() != 0;
    out.bag = r.u8v();
    out.bagSlot = r.u32v();
    out.itemId = r.u32v();
    out.suffixFactor = r.u32v();
    out.randomPropertyId = r.u32v();
    out.count = r.u32v();
    return r.ok();
}

bool parseInventoryChangeFailure(std::span<const u8> payload, InventoryChangeFailure& out) {
    ByteReader r(payload);
    out = InventoryChangeFailure{};
    out.result = r.u8v();
    if (out.result != 0) {
        if (out.result == 1) { // EQUIP_ERR_CANT_EQUIP_LEVEL_I
            out.requiredLevel = r.u32v();
        }
        out.item1 = r.u64v();
        out.item2 = r.u64v();
        out.bagSlot = r.u8v();
    }
    return r.ok();
}

bool parseGameObjectQueryResponse(std::span<const u8> payload, GameObjectQueryInfo& out) {
    ByteReader r(payload);
    const u32 entry = r.u32v();
    if (r.failed()) {
        return false;
    }
    out = GameObjectQueryInfo{};
    out.entry = entry & 0x7FFFFFFFu;
    if (entry & 0x80000000u) {
        return true; // unknown to the server
    }
    out.found = true;
    out.type = r.u32v();
    out.displayId = r.u32v();
    out.name = r.cstring();
    for (u32 i = 0; i < 3; ++i) {
        r.cstring(); // name2..4, always empty
    }
    out.icon = r.cstring();
    for (u32& d : out.data) {
        d = r.u32v();
    }
    return r.ok();
}

// -------------------------------------------------------------------------------------------------------
// Experience and death.

bool parseXpGain(std::span<const u8> payload, XpGain& out) {
    ByteReader r(payload);
    out.victim = r.u64v();
    out.totalXp = r.u32v();
    out.kill = r.u8v() == 0;
    out.baseXp = out.totalXp;
    out.groupBonus = 1.0f;
    if (out.kill) {
        out.baseXp = r.u32v();
        out.groupBonus = r.f32v();
    }
    return r.ok();
}

bool parseLevelUpInfo(std::span<const u8> payload, LevelUpInfo& out) {
    ByteReader r(payload);
    out.level = r.u32v();
    out.healthGain = r.u32v();
    for (u32& p : out.powerGains) {
        p = r.u32v();
    }
    for (u32& s : out.statGains) {
        s = r.u32v();
    }
    return r.ok();
}

bool parseCorpseQuery(std::span<const u8> payload, CorpseQuery& out) {
    ByteReader r(payload);
    out = CorpseQuery{};
    out.found = r.u8v() != 0;
    if (out.found) {
        out.mapId = r.i32v();
        out.x = r.f32v();
        out.y = r.f32v();
        out.z = r.f32v();
        out.corpseMapId = r.u32v();
    }
    return r.ok();
}

bool parseResurrectRequest(std::span<const u8> payload, ResurrectRequest& out) {
    ByteReader r(payload);
    out.resurrector = r.u64v();
    out.name = lengthString(r);
    out.sickness = r.u8v() != 0;
    out.delayed = r.u8v() != 0;
    return r.ok();
}

bool parseGuid(std::span<const u8> payload, u64& guid) {
    ByteReader r(payload);
    guid = r.u64v();
    return r.ok();
}

bool parseStandStateUpdate(std::span<const u8> payload, u8& standState) {
    ByteReader r(payload);
    standState = r.u8v();
    return r.ok();
}

// -------------------------------------------------------------------------------------------------------
// Chat.

bool parseChatMessage(std::span<const u8> payload, ChatMessage& out) {
    ByteReader r(payload);
    out = ChatMessage{};
    out.type = static_cast<ChatType>(r.u8v());
    out.language = r.u32v();
    switch (out.type) {
    case ChatType::MonsterWhisper:
    case ChatType::RaidBossWhisper:
    case ChatType::RaidBossEmote:
    case ChatType::MonsterEmote: // a name and whom it is addressed to, no sender GUID
        out.senderName = lengthString(r);
        out.target = r.u64v();
        break;
    case ChatType::Say:
    case ChatType::Party:
    case ChatType::Yell:
        out.sender = r.u64v();
        r.u64v(); // the sender again
        break;
    case ChatType::MonsterSay:
    case ChatType::MonsterYell:
        out.sender = r.u64v();
        out.senderName = lengthString(r);
        out.target = r.u64v();
        break;
    case ChatType::Channel:
        out.channel = r.cstring();
        out.playerRank = r.u32v();
        out.sender = r.u64v();
        break;
    default: out.sender = r.u64v(); break;
    }
    out.text = lengthString(r);
    out.tag = r.u8v();
    return r.ok();
}

bool parseCString(std::span<const u8> payload, std::string_view& text) {
    ByteReader r(payload);
    text = r.cstring();
    return r.ok();
}

bool parseAreaTriggerMessage(std::span<const u8> payload, std::string_view& text) {
    ByteReader r(payload);
    text = lengthString(r);
    return r.ok();
}

bool parseTextEmote(std::span<const u8> payload, TextEmote& out) {
    ByteReader r(payload);
    out.guid = r.u64v();
    out.textEmote = r.u32v();
    out.emoteNum = r.u32v();
    out.targetName = lengthString(r);
    return r.ok();
}

// -------------------------------------------------------------------------------------------------------
// Visuals and sounds (Packets/Spell.cpp, Packets/Misc.cpp).

bool parseSpellVisualKit(std::span<const u8> payload, u64& guid, u32& kitId) {
    ByteReader r(payload);
    guid = r.u64v();
    kitId = r.u32v();
    return r.ok();
}

bool parseSoundId(std::span<const u8> payload, u32& soundId) {
    ByteReader r(payload);
    soundId = r.u32v();
    return r.ok();
}

bool parseIdAndGuid(std::span<const u8> payload, u32& id, u64& guid) {
    ByteReader r(payload);
    id = r.u32v();
    guid = r.u64v();
    return r.ok();
}

// -------------------------------------------------------------------------------------------------------
// Client -> server.

void writeGuid(std::vector<u8>& out, u64 guid) {
    ByteWriter(out).u64v(guid);
}

void writeU32(std::vector<u8>& out, u32 value) {
    ByteWriter(out).u32v(value);
}

void writeU8(std::vector<u8>& out, u8 value) {
    ByteWriter(out).u8v(value);
}

void writeCastSpell(std::vector<u8>& out, u32 spellId, const SpellCastTargets& targets) {
    ByteWriter w(out);
    w.u32v(spellId);
    writeSpellCastTargets(w, targets);
}

void writeUseItem(std::vector<u8>& out, u8 bag, u8 slot, u8 spellIndex, const SpellCastTargets& targets) {
    ByteWriter w(out);
    w.u8v(bag).u8v(slot).u8v(spellIndex);
    writeSpellCastTargets(w, targets);
}

void writeSetActionButton(std::vector<u8>& out, u8 button, u32 packed) {
    ByteWriter(out).u8v(button).u32v(packed);
}

void writeGossipSelectOption(std::vector<u8>& out, u64 source, u32 option, std::string_view code) {
    ByteWriter w(out);
    w.u64v(source).u32v(option);
    if (!code.empty()) {
        w.cstring(code);
    }
}

void writeNpcTextQuery(std::vector<u8>& out, u32 textId, u64 npc) {
    ByteWriter(out).u32v(textId).u64v(npc);
}

void writeQuestGiverQuest(std::vector<u8>& out, u64 questGiver, u32 questId) {
    ByteWriter(out).u64v(questGiver).u32v(questId);
}

void writeQuestGiverChooseReward(std::vector<u8>& out, u64 questGiver, u32 questId, u32 choice) {
    ByteWriter(out).u64v(questGiver).u32v(questId).u32v(choice);
}

void writeResurrectResponse(std::vector<u8>& out, u64 resurrector, bool accept) {
    ByteWriter(out).u64v(resurrector).u8v(accept ? 1 : 0);
}

void writeMessageChat(std::vector<u8>& out, ChatType type, Language language, std::string_view target,
                      std::string_view text) {
    ByteWriter w(out);
    w.u32v(static_cast<u32>(type)).u32v(static_cast<u32>(language));
    if (type == ChatType::Whisper || type == ChatType::Channel) {
        w.cstring(target);
    }
    w.cstring(text);
}

void writeEntryQuery(std::vector<u8>& out, u32 entry, u64 guid) {
    ByteWriter(out).u32v(entry).u64v(guid);
}

} // namespace vd::net
