#pragma once

#include "core/Types.h"

#include <utility>
#include <vector>

namespace vd::pack {
class PackReader;
}

namespace vd::game {

// The client tables of the minimap's tracking (parity pass), indexed once from the db pack:
// - Lock.dbc (1.12.1, 33 fields: 0 id, 1..8 Type, 9..16 Index, 17..24 Skill, 25..32 Action): a Type 2
//   (LOCK_KEY_SKILL) entry's Index is a LockType.dbc id (2 Herbalism, 3 Mining, 6 Treasure...). Find Herbs,
//   Find Minerals and Find Treasure set bit (LockType - 1) of PLAYER_TRACK_RESOURCES (vMaNGOS
//   Aura::HandleAuraTrackResources); a chest whose lock opens with one of those types is a tracked resource.
// - Spell.dbc's tracking spells: an effect applying aura 44 (SPELL_AURA_TRACK_CREATURES) or 45
//   (SPELL_AURA_TRACK_RESOURCES). EffectApplyAuraName is at 91..93 in the client's layout (vMaNGOS'
//   SpellEntry.h numbers it 94..96 counting the 3 EffectBonusCoefficient columns the client lacks). The
//   minimap's tracking icon is the icon of the player's aura of such a spell (the client's
//   GetTrackingTexture).
class MinimapData {
public:
    bool init(const pack::PackReader& db);
    // Tests: hand-made rows, (lock id, LockType mask) and spell ids.
    void set(std::vector<std::pair<u32, u32>> lockTypes, std::vector<u32> trackingSpells);
    bool ready() const { return !m_locks.empty(); }

    // The LockType bits (1 << (type - 1)) that open a lock with a skill; 0 when unknown or none.
    u32 lockTypeMask(u32 lockId) const;
    bool isTrackingSpell(u32 spellId) const;
    u32 trackingSpellCount() const { return static_cast<u32>(m_trackingSpells.size()); }

private:
    std::vector<std::pair<u32, u32>> m_locks; // (lock id, LockType mask), sorted by id
    std::vector<u32> m_trackingSpells;        // sorted
};

} // namespace vd::game
