#include "game/SpellMods.h"

#include "game/Entities.h"
#include "game/GameData.h"
#include "game/SpellText.h"
#include "net/SpellMessages.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vd::game {
namespace {

namespace uf = net::uf;

constexpr u32 kAttrEx3IgnoreCasterModifiers = 0x20000000; // SPELL_ATTR_EX3_IGNORE_CASTER_MODIFIERS
constexpr u32 kAttrNoCastSpeed = 0x30; // SPELL_ATTR_RANGED | SPELL_ATTR_TRADESPELL... (WoW.exe)
constexpr f32 kCastSpeedEpsilon = 0.00001f;

f32 floatField(const Entity& e, u16 index) {
    const u32 bits = e.value(index);
    f32 value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

} // namespace

u32 classSpellFamily(u32 cls) {
    switch (cls) {
    case 1: return 4;  // warrior
    case 2: return 10; // paladin
    case 3: return 9;  // hunter
    case 4: return 8;  // rogue
    case 5: return 6;  // priest
    case 7: return 11; // shaman
    case 8: return 3;  // mage
    case 9: return 5;  // warlock
    case 11: return 7; // druid
    default: return 0;
    }
}

SpellModTarget modTarget(const SpellInfo& spell) {
    return {spell.spellFamily, spell.familyFlags, spell.attributesEx3};
}

SpellModTarget modTarget(const SpellDetails& spell) {
    return {spell.spellFamily, spell.familyFlags, spell.attributesEx3};
}

void SpellMods::reset() {
    m_flat.fill(0);
    m_percent.fill(0);
    ++m_version;
}

void SpellMods::setClassFamily(u32 family) {
    if (family != m_classFamily) {
        m_classFamily = family;
        ++m_version;
    }
}

bool SpellMods::handle(bool flat, std::span<const u8> payload) {
    const std::optional<net::SpellModifierUpdate> m = net::parseSpellModifier(payload);
    if (!m) {
        return false;
    }
    set(flat, m->bit, m->op, m->value);
    return true;
}

void SpellMods::set(bool flat, u32 bit, u32 op, i32 value) {
    if (bit >= kSpellModBits || op >= kSpellModOps) {
        return; // the client's tables have no room for it either
    }
    i32& slot = (flat ? m_flat : m_percent)[index(bit, op)];
    if (slot != value) {
        slot = value;
        ++m_version;
    }
}

i32 SpellMods::flat(u32 bit, SpellModOp op) const {
    return bit < kSpellModBits ? m_flat[index(bit, static_cast<u32>(op))] : 0;
}

i32 SpellMods::percent(u32 bit, SpellModOp op) const {
    return bit < kSpellModBits ? m_percent[index(bit, static_cast<u32>(op))] : 0;
}

bool SpellMods::modifiers(const SpellModTarget& spell, SpellModOp op, i32& flat, i32& percent) const {
    flat = 0;
    percent = 100;
    if (spell.family == 0 || spell.family != m_classFamily ||
        (spell.attributesEx3 & kAttrEx3IgnoreCasterModifiers)) {
        return false;
    }
    i32 sumFlat = 0, sumPercent = 0;
    for (u32 bit = 0; bit < kSpellModBits; ++bit) {
        if (spell.familyFlags & (u64{1} << bit)) {
            sumFlat += m_flat[index(bit, static_cast<u32>(op))];
            sumPercent += m_percent[index(bit, static_cast<u32>(op))];
        }
    }
    if (sumFlat == 0 && sumPercent == 0) {
        return false;
    }
    flat = sumFlat;
    percent = std::max(0, sumPercent + 100);
    return true;
}

i32 SpellMods::apply(const SpellModTarget& spell, SpellModOp op, i32 value) const {
    i32 flat, percent;
    if (!modifiers(spell, op, flat, percent)) {
        return value;
    }
    return static_cast<i32>((static_cast<i64>(value) + flat) * percent / 100); // truncated towards zero
}

f32 SpellMods::apply(const SpellModTarget& spell, SpellModOp op, f32 value) const {
    i32 flat, percent;
    if (!modifiers(spell, op, flat, percent)) {
        return value;
    }
    return (static_cast<f32>(flat) + value) * static_cast<f32>(percent) * 0.01f;
}

u32 modifiedPowerCost(const SpellModTarget& spell, u32 school, u32 powerType, u32 powerCost,
                      u32 powerCostPercent, const Entity* me, const SpellMods* mods) {
    i64 cost = powerCost;
    if (me) {
        if (powerCostPercent > 0 && powerType == 0) {
            cost += static_cast<i64>(powerCostPercent) * me->value(uf::UNIT_FIELD_BASE_MANA) / 100;
        }
        const u16 s = static_cast<u16>(std::min<u32>(school, 6));
        cost += static_cast<i32>(me->value(static_cast<u16>(uf::UNIT_FIELD_POWER_COST_MODIFIER + s)));
        const f32 multiplier =
            1.0f + floatField(*me, static_cast<u16>(uf::UNIT_FIELD_POWER_COST_MULTIPLIER + s));
        cost = static_cast<i64>(std::nearbyint(static_cast<f32>(cost) * multiplier));
    }
    i32 value = static_cast<i32>(std::max<i64>(0, cost));
    if (mods) {
        value = mods->apply(spell, SpellModOp::Cost, value);
    }
    return static_cast<u32>(std::max(0, value));
}

u32 modifiedCastTime(const SpellModTarget& spell, u32 attributes, u32 castTimeMs, const Entity* me,
                     const SpellMods* mods) {
    i32 value = static_cast<i32>(castTimeMs);
    if (mods) {
        value = mods->apply(spell, SpellModOp::CastingTime, value);
    }
    if (me && (attributes & kAttrNoCastSpeed) == 0 && value > 0) {
        const f32 speed = floatField(*me, uf::UNIT_MOD_CAST_SPEED);
        if (speed > 0.0f && std::fabs(speed - 1.0f) > kCastSpeedEpsilon) {
            value = static_cast<i32>(std::nearbyint(speed * static_cast<f32>(value)));
        }
    }
    return static_cast<u32>(std::max(0, value));
}

void applySpellMods(SpellDetails& d, const Entity* me, const SpellMods* mods) {
    const SpellModTarget target = modTarget(d);
    if (me || d.powerCostPercent == 0) {
        // The client shows what the spell costs this caster, not the share of its base mana.
        d.powerCost =
            modifiedPowerCost(target, d.school, d.powerType, d.powerCost, d.powerCostPercent, me, mods);
        d.powerCostPercent = 0;
    }
    d.castTimeMs = modifiedCastTime(target, d.attributes, d.castTimeMs, me, mods);
    if (!mods) {
        return;
    }
    if (d.maxRange > 0.0f) {
        d.maxRange = mods->apply(target, SpellModOp::Range, d.maxRange);
    }
    d.recoveryMs = static_cast<u32>(
        std::max(0, mods->apply(target, SpellModOp::Cooldown, static_cast<i32>(d.recoveryMs))));
    d.categoryRecoveryMs = static_cast<u32>(
        std::max(0, mods->apply(target, SpellModOp::Cooldown, static_cast<i32>(d.categoryRecoveryMs))));
    if (d.durationMs > 0) {
        d.durationMs = static_cast<u32>(
            std::max(0, mods->apply(target, SpellModOp::Duration, static_cast<i32>(d.durationMs))));
    }
}

} // namespace vd::game
