#include "net/SpellLogMessages.h"

#include "net/ByteBuffer.h"

namespace vd::net {
namespace {

// What one entry of an effect carries in SMSG_SPELLLOGEXECUTE (SpellLogExecute::AppendBodyTo).
enum class EntryLayout : u8 { None, Target, PowerDrain, Heal, Energize, ExtraAttacks, Item, Interrupt, Durability };

EntryLayout entryLayout(u32 effect) {
    switch (effect) {
    case kEffectPowerDrain: return EntryLayout::PowerDrain;
    case kEffectHeal:
    case kEffectHealMaxHealth: return EntryLayout::Heal;
    case kEffectEnergize: return EntryLayout::Energize;
    case kEffectAddExtraAttacks: return EntryLayout::ExtraAttacks;
    case kEffectCreateItem:
    case kEffectFeedPet: return EntryLayout::Item;
    case kEffectInterruptCast: return EntryLayout::Interrupt;
    case kEffectDurabilityDamage: return EntryLayout::Durability;
    case kEffectInstaKill:
    case kEffectResurrect:
    case kEffectDispel:
    case kEffectThreat:
    case kEffectDistract:
    case kEffectSanctuary:
    case kEffectThreatAll:
    case kEffectDispelMechanic:
    case kEffectResurrectNew:
    case kEffectAttackMe:
    case kEffectSkinPlayerCorpse:
    case kEffectModifyThreatPercent:
    case kEffect126:
    case kEffectOpenLock:
    case kEffectOpenLockItem:
    case kEffectDismissPet:
    case kEffectTransDoor:
    case kEffectSummon:
    case kEffectSummonPet:
    case kEffectSummonWild:
    case kEffectSummonGuardian:
    case kEffectSummonTotemSlot1:
    case kEffectSummonTotemSlot1 + 1:
    case kEffectSummonTotemSlot1 + 2:
    case kEffectSummonTotemSlot1 + 3:
    case kEffectSummonPossessed:
    case kEffectSummonTotem:
    case kEffectSummonCritter:
    case kEffectSummonObjectWild:
    case kEffectSummonObjectSlot1:
    case kEffectSummonObjectSlot1 + 1:
    case kEffectSummonObjectSlot1 + 2:
    case kEffectSummonObjectSlot1 + 3:
    case kEffectSummonDemon: return EntryLayout::Target;
    default: return EntryLayout::None; // "every effect that can add execute log info is handled above"
    }
}

} // namespace

bool parseSpellLogExecute(std::span<const u8> payload, SpellLogExecute& out) {
    ByteReader r(payload);
    out.caster = r.packedGuid(); // WriteAsPackedClientBuildAware: packed after 1.8.4
    out.spellId = r.u32v();
    out.effectCount = r.u32v();
    out.entries.clear();
    if (r.failed() || out.effectCount > SpellLogExecute::kMaxEffects) {
        return false;
    }
    for (u32 i = 0; i < out.effectCount; ++i) {
        SpellLogExecute::Effect& effect = out.effects[i];
        effect.effect = r.u32v();
        effect.count = r.u32v();
        effect.first = static_cast<u32>(out.entries.size());
        if (r.failed() || out.entries.size() + effect.count > SpellLogExecute::kMaxEntries) {
            return false;
        }
        const EntryLayout layout = entryLayout(effect.effect);
        for (u32 j = 0; j < effect.count; ++j) {
            SpellLogExecute::Entry entry;
            switch (layout) {
            case EntryLayout::None: break;
            case EntryLayout::Target: entry.target = r.u64v(); break;
            case EntryLayout::PowerDrain:
                entry.target = r.u64v();
                entry.value = r.u32v();  // amount
                entry.value2 = r.u32v(); // power
                entry.multiplier = r.f32v();
                break;
            case EntryLayout::Heal:
                entry.target = r.u64v();
                entry.value = r.u32v();
                entry.critical = r.u8v() != 0;
                break;
            case EntryLayout::Energize:
                entry.target = r.u64v();
                entry.value = r.u32v();
                entry.value2 = r.u32v();
                break;
            case EntryLayout::ExtraAttacks:
                entry.target = r.u64v();
                entry.value = r.u32v();
                break;
            case EntryLayout::Item: entry.value = r.u32v(); break;
            case EntryLayout::Interrupt:
                entry.target = r.u64v();
                entry.value = r.u32v();
                break;
            case EntryLayout::Durability:
                entry.target = r.u64v();
                entry.value = r.u32v();
                entry.value2 = r.u32v();
                break;
            }
            if (r.failed()) {
                return false;
            }
            out.entries.push_back(entry);
        }
    }
    return r.ok();
}

bool parseSpellDamageShield(std::span<const u8> payload, SpellDamageShield& out) {
    ByteReader r(payload);
    out.bearer = r.u64v();
    out.attacker = r.u64v();
    out.damage = r.u32v();
    out.school = r.u32v();
    return r.ok();
}

bool parseSpellInstaKillLog(std::span<const u8> payload, SpellInstaKillLog& out) {
    ByteReader r(payload);
    out.victim = r.u64v();
    out.spellId = r.u32v();
    return r.ok();
}

bool parseSpellDispelLog(std::span<const u8> payload, SpellDispelLog& out) {
    ByteReader r(payload);
    out.victim = r.packedGuid();
    out.caster = r.packedGuid();
    out.count = r.u32v();
    if (r.failed() || out.count > SpellDispelLog::kMaxSpells) {
        return false;
    }
    for (u32 i = 0; i < out.count; ++i) {
        out.spells[i] = r.u32v();
    }
    return r.ok();
}

bool parseDispelFailed(std::span<const u8> payload, DispelFailed& out) {
    ByteReader r(payload);
    out.caster = r.u64v();
    out.victim = r.u64v();
    out.count = 0;
    // At least one aura: the server only sends it for a failure.
    if (r.failed() || r.remaining() == 0 || r.remaining() % 4 != 0 || r.remaining() / 4 > DispelFailed::kMaxSpells) {
        return false;
    }
    while (!r.atEnd()) {
        out.spells[out.count++] = r.u32v();
    }
    return r.ok();
}

bool parseEnchantmentLog(std::span<const u8> payload, EnchantmentLog& out) {
    ByteReader r(payload);
    out.caster = r.u64v();
    out.owner = r.u64v();
    out.itemEntry = r.u32v();
    out.enchantId = r.u32v();
    out.showAffiliation = r.u8v() != 0;
    return r.ok();
}

bool parseSpellChainTargets(std::span<const u8> payload, SpellChainTargets& out) {
    ByteReader r(payload);
    out.caster = r.u64v();
    out.spellId = r.u32v();
    out.count = r.u32v();
    if (r.failed() || out.count > SpellChainTargets::kMaxTargets) {
        return false;
    }
    for (u32 i = 0; i < out.count; ++i) {
        out.targets[i] = r.u64v();
    }
    return r.ok();
}

} // namespace vd::net
