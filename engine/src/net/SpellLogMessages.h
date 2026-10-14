#pragma once

#include "core/Types.h"

#include <array>
#include <span>
#include <vector>

// Phase 13 (combat): the combat log packets the client did not read yet, as vmangos-core 4641790 writes them for
// build 5875 (Server/Packets/Spell: SpellLogExecute, SpellUpdateChainTargets; Server/Packets/Combat:
// SpellDamageShield, FeignDeathResisted; Server/Packets/Item: EnchantmentLog; Spells/SpellEffects.cpp:
// SMSG_SPELLINSTAKILLLOG, SMSG_SPELLDISPELLOG, SMSG_DISPEL_FAILED). Same rules as net/GameMessages: parsers fill a
// caller-owned struct (reused ones keep their vectors' capacity) and return false on a malformed packet without
// reading past its end. SMSG_FEIGN_DEATH_RESISTED has an empty body.
namespace vd::net {

// SpellEffects (Spells/SpellDefines.h) that SMSG_SPELLLOGEXECUTE logs.
constexpr u32 kEffectInstaKill = 1;
constexpr u32 kEffectPowerDrain = 8;
constexpr u32 kEffectHeal = 10;
constexpr u32 kEffectResurrect = 18;
constexpr u32 kEffectAddExtraAttacks = 19;
constexpr u32 kEffectCreateItem = 24;
constexpr u32 kEffectSummon = 28;
constexpr u32 kEffectEnergize = 30;
constexpr u32 kEffectOpenLock = 33;
constexpr u32 kEffectLearnSpell = 36;
constexpr u32 kEffectDispel = 38;
constexpr u32 kEffectSummonWild = 41;
constexpr u32 kEffectSummonGuardian = 42;
constexpr u32 kEffectTransDoor = 50;
constexpr u32 kEffectSummonPet = 56;
constexpr u32 kEffectOpenLockItem = 59;
constexpr u32 kEffectThreat = 63;
constexpr u32 kEffectHealMaxHealth = 67;
constexpr u32 kEffectInterruptCast = 68;
constexpr u32 kEffectDistract = 69;
constexpr u32 kEffectSummonPossessed = 73;
constexpr u32 kEffectSummonTotem = 74;
constexpr u32 kEffectSummonObjectWild = 76;
constexpr u32 kEffectSanctuary = 79;
constexpr u32 kEffectSummonTotemSlot1 = 87; // .. 90
constexpr u32 kEffectThreatAll = 91;
constexpr u32 kEffectSummonCritter = 97;
constexpr u32 kEffectFeedPet = 101;
constexpr u32 kEffectDismissPet = 102;
constexpr u32 kEffectSummonObjectSlot1 = 104; // .. 107
constexpr u32 kEffectDispelMechanic = 108;
constexpr u32 kEffectDurabilityDamage = 111;
constexpr u32 kEffectSummonDemon = 112;
constexpr u32 kEffectResurrectNew = 113;
constexpr u32 kEffectAttackMe = 114;
constexpr u32 kEffectSkinPlayerCorpse = 116;
constexpr u32 kEffectModifyThreatPercent = 125;
constexpr u32 kEffect126 = 126;

// SMSG_SPELLLOGEXECUTE (Spell::SendLogExecute): what a spell's effects did, per effect and target. The layout of an
// entry depends on the effect (SpellLogExecute::AppendBodyTo); effects the server never logs carry no bytes.
struct SpellLogExecute {
    struct Entry {
        u64 target = 0;         // full GUID; 0 for CREATE_ITEM and FEED_PET
        u32 value = 0;          // POWER_DRAIN / HEAL / ENERGIZE: amount; ADD_EXTRA_ATTACKS: count; CREATE_ITEM,
                                // FEED_PET, DURABILITY_DAMAGE: item entry (0xFFFFFFFF: every item); INTERRUPT_CAST:
                                // the interrupted spell
        u32 value2 = 0;         // POWER_DRAIN / ENERGIZE: power type; DURABILITY_DAMAGE: the second i32 (-1)
        f32 multiplier = 0.0f;  // POWER_DRAIN: what the caster gains per point drained (0: a plain drain)
        bool critical = false;  // HEAL
    };
    struct Effect {
        u32 effect = 0; // SpellEffects
        u32 first = 0;  // into entries
        u32 count = 0;
    };
    static constexpr u32 kMaxEffects = 3;   // MAX_EFFECT_INDEX
    static constexpr u32 kMaxEntries = 256; // per packet; more is malformed
    u64 caster = 0;                         // packed on the wire
    u32 spellId = 0;
    u32 effectCount = 0;
    std::array<Effect, kMaxEffects> effects{};
    std::vector<Entry> entries;
};
bool parseSpellLogExecute(std::span<const u8> payload, SpellLogExecute& out);

// SMSG_SPELLDAMAGESHIELD (Unit::DealDamageShieldDamage, 24 bytes): Thorns, Retribution Aura, Lightning Shield,
// Fire Shield... hurting whoever struck their bearer.
struct SpellDamageShield {
    u64 bearer = 0;   // the unit with the shield aura (vMaNGOS' "victimGuid")
    u64 attacker = 0; // who struck it and takes the damage
    u32 damage = 0;
    u32 school = 0; // SpellSchools index
};
bool parseSpellDamageShield(std::span<const u8> payload, SpellDamageShield& out);

// SMSG_SPELLINSTAKILLLOG (Spell::EffectInstaKill, 12 bytes): full victim GUID + spell.
struct SpellInstaKillLog {
    u64 victim = 0;
    u32 spellId = 0;
};
bool parseSpellInstaKillLog(std::span<const u8> payload, SpellInstaKillLog& out);

// SMSG_SPELLDISPELLOG (Spell::EffectDispel): packed victim, packed caster, the auras removed.
struct SpellDispelLog {
    static constexpr u32 kMaxSpells = 64;
    u64 victim = 0;
    u64 caster = 0;
    u32 count = 0;
    std::array<u32, kMaxSpells> spells{};
};
bool parseSpellDispelLog(std::span<const u8> payload, SpellDispelLog& out);

// SMSG_DISPEL_FAILED (Spell::EffectDispel): full caster, full victim, then the auras it failed to remove up to the
// end of the packet (no count).
struct DispelFailed {
    static constexpr u32 kMaxSpells = 64;
    u64 caster = 0;
    u64 victim = 0;
    u32 count = 0;
    std::array<u32, kMaxSpells> spells{};
};
bool parseDispelFailed(std::span<const u8> payload, DispelFailed& out);

// SMSG_ENCHANTMENTLOG (Player::SendEnchantmentLog, 25 bytes): an enchantment put on an item, or gone from it.
struct EnchantmentLog {
    u64 caster = 0;     // the enchanter; 0: the enchantment faded
    u64 owner = 0;      // the item's owner
    u32 itemEntry = 0;
    u32 enchantId = 0;  // SpellItemEnchantment.dbc (Item::SetEnchantment passes the enchantment's id)
    bool showAffiliation = false; // the copy for the others nearby (the owner's own copy has 0)
};
bool parseEnchantmentLog(std::span<const u8> payload, EnchantmentLog& out);

// SMSG_SPELL_UPDATE_CHAIN_TARGETS (builds > 1.11.2): the units a chain spell (Chain Lightning, Chain Heal) jumped
// to, in order.
struct SpellChainTargets {
    static constexpr u32 kMaxTargets = 16;
    u64 caster = 0; // full GUID
    u32 spellId = 0;
    u32 count = 0;
    std::array<u64, kMaxTargets> targets{};
};
bool parseSpellChainTargets(std::span<const u8> payload, SpellChainTargets& out);

} // namespace vd::net
