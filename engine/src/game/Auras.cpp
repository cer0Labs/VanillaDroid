#include "game/Auras.h"

#include "game/SpellText.h"
#include "net/UpdateFields.gen.h"

namespace vd::game {

namespace uf = net::uf;

u32 readAuras(const Entity& unit, std::span<AuraInfo> out) {
    if ((unit.type != ObjectType::Unit && unit.type != ObjectType::Player) ||
        unit.values.size() < uf::UNIT_END) {
        return 0;
    }
    u32 count = 0;
    for (u32 slot = 0; slot < kMaxAuras && count < out.size(); ++slot) {
        const u32 spellId = unit.value(static_cast<u16>(uf::UNIT_FIELD_AURA + slot));
        if (spellId == 0) {
            continue;
        }
        AuraInfo& a = out[count++];
        a = AuraInfo{};
        a.spellId = spellId;
        a.slot = static_cast<u8>(slot);
        // Flags: 4 bits a slot (8 a field); levels and applications: a byte a slot (4 a field).
        a.flags = static_cast<u8>(
            (unit.value(static_cast<u16>(uf::UNIT_FIELD_AURAFLAGS + slot / 8)) >> ((slot % 8) * 4)) & 0xF);
        a.casterLevel = unit.byteValue(static_cast<u16>(uf::UNIT_FIELD_AURALEVELS + slot / 4), slot % 4);
        const u8 applications =
            unit.byteValue(static_cast<u16>(uf::UNIT_FIELD_AURAAPPLICATIONS + slot / 4), slot % 4);
        a.stacks = static_cast<u8>(applications == 0xFF ? 0xFF : applications + 1); // stores count - 1
    }
    return count;
}

bool canCancelAura(const AuraInfo& aura, const SpellDetails& spell, bool ownSpell) {
    constexpr u32 kAttrDoNotDisplay = 0x00000080;  // SPELL_ATTR_DO_NOT_DISPLAY
    constexpr u32 kAttrNoAuraCancel = 0x80000000;  // SPELL_ATTR_NO_AURA_CANCEL
    constexpr u32 kAttrExNoAuraIcon = 0x10000000;  // SPELL_ATTR_EX_NO_AURA_ICON
    if (!aura.positive() || !aura.cancelable() || spell.passive() || (spell.attributes & kAttrNoAuraCancel) != 0 ||
        (spell.attributes & kAttrDoNotDisplay) != 0 ||
        ((spell.attributesEx & kAttrExNoAuraIcon) != 0 && spell.activeIcon == 0)) {
        return false;
    }
    // SPELL_EFFECT_APPLY_AREA_AURA_PARTY (35); the later area kinds (119, 128, 129, 132) are not in 1.12's Spell.dbc.
    for (const SpellEffectData& e : spell.effects) {
        if ((e.effect == 35 || e.effect == 119 || e.effect == 128 || e.effect == 129 || e.effect == 132) && !ownSpell) {
            return false;
        }
    }
    return true;
}

void AuraDurations::reset() {
    m_slots.fill(Slot{});
    m_seen.fill(0);
}

void AuraDurations::onDuration(u8 slot, u32 durationMs, u32 nowMs) {
    if (slot < kMaxAuras) {
        m_slots[slot] = Slot{true, durationMs, nowMs};
    }
}

void AuraDurations::onPlayerFields(const Entity& player, u32 nowMs) {
    for (u32 slot = 0; slot < kMaxAuras; ++slot) {
        const u32 spellId = player.value(static_cast<u16>(uf::UNIT_FIELD_AURA + slot));
        if (spellId == m_seen[slot]) {
            continue;
        }
        m_seen[slot] = spellId;
        Slot& s = m_slots[slot];
        if (s.valid && nowMs - s.receivedMs > kGraceMs) {
            s = Slot{}; // it belonged to the aura that left the slot
        }
    }
}

void AuraDurations::apply(std::span<AuraInfo> auras, u32 nowMs) const {
    for (AuraInfo& a : auras) {
        const Slot& s = m_slots[a.slot % kMaxAuras];
        if (!s.valid || s.durationMs == 0) {
            a.durationMs = 0;
            a.remainingMs = 0;
            continue;
        }
        const u32 elapsed = nowMs - s.receivedMs;
        a.durationMs = s.durationMs;
        a.remainingMs = elapsed < s.durationMs ? s.durationMs - elapsed : 0;
    }
}

} // namespace vd::game
