#include "game/RaidAuras.h"

namespace vd::game {
namespace {

constexpr u32 kAuraTrackCreatures = 44;  // SPELL_AURA_TRACK_CREATURES
constexpr u32 kAuraTrackResources = 45;  // SPELL_AURA_TRACK_RESOURCES
constexpr u32 kAuraTrackStealthed = 151; // SPELL_AURA_TRACK_STEALTHED

constexpr u32 kAreaAuraEffects[] = {27, 35, 119, 128, 129};

} // namespace

bool raidAuraListed(const RaidAuraSpell& spell) {
    if ((spell.attributes & kSpellAttrDoNotDisplay) != 0 ||
        (spell.attributesEx & kSpellAttrExNoAuraIcon) != 0) {
        return false;
    }
    for (const u32 aura : spell.effectAuras) {
        if (aura == kAuraTrackCreatures || aura == kAuraTrackResources || aura == kAuraTrackStealthed) {
            return false;
        }
    }
    return true;
}

bool raidCastableBuff(const RaidAuraSpell& spell, bool known) {
    if (!known || (spell.attributes & kSpellAttrPassive) != 0 ||
        (spell.attributesEx3 & kSpellAttrEx3HideInRaidFilter) != 0 ||
        spell.implicitTargetA[0] == kTargetUnitCaster) {
        return false;
    }
    for (const u32 effect : spell.effects) {
        for (const u32 area : kAreaAuraEffects) {
            if (effect == area) {
                return false;
            }
        }
    }
    return true;
}

u32 raidDispelTypes(const RaidAuraSpell& spell) {
    u32 types = 0;
    for (u32 i = 0; i < spell.effects.size(); ++i) {
        const i32 type = spell.effectMisc[i];
        if (spell.effects[i] == kSpellEffectDispel && spell.implicitTargetA[i] != kTargetUnitEnemy &&
            type > 0 && type < 32) {
            types |= 1u << static_cast<u32>(type);
        }
    }
    return types;
}

bool raidDispellable(u32 dispelTypes, u32 debuffDispel) {
    return debuffDispel > 0 && debuffDispel < 32 && (dispelTypes & (1u << debuffDispel)) != 0;
}

} // namespace vd::game
