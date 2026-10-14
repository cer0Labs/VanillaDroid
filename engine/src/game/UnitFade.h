#pragma once

#include "core/Types.h"
#include "game/Entities.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <utility>
#include <vector>

namespace vd::pack {
class PackReader;
}

namespace vd::game {

// Phase 11: how a unit's auras change the way it is drawn, as the 1.12 client does it. Every aura in a unit's visible
// slots (UNIT_FIELD_AURA, public: the client sees them on everyone it is sent) shows its spell's SpellVisual "state"
// kit while it lasts (vMaNGOS gives a slot to any aura with a visual for this reason), and that kit's character procs
// (SpellVisualKit.dbc) may make the model translucent (proc 14: its opacity) and tint it (proc 1: a color). That is
// how ghosts look (Ghost, 8326: 0.5 and 0x8CB9FD), Stealth, Prowl and Vanish (0.3), Invisibility potions and Fade
// (0.5), Shadowform (0.65, dark purple) or Banish (0.3, purple). Who is sent at all (a ghost only to ghosts, a rogue
// in stealth only to who detects it) is the server's business.
struct UnitFade {
    f32 alpha = 1.0f;       // opacity of the model and what it carries
    glm::vec3 tint{1.0f};   // multiplies its colors
    f32 fadeSeconds = 0.0f; // how long the change into it takes (Stealth: 1 s), 0 = at once
    bool operator==(const UnitFade&) const = default;
};

// SpellVisualKit character procs (fields 15-18) and their parameters (fields 19-34), as the DBC lays them:
// parameter p of proc i at [p * 4 + i]. Proc 14: opacity (p0) and, when p2 is a time in ms, how long the fade
// takes; proc 1: a color (p0, an RGB integer stored as a float). Phase 14 (spellfx): WoW.exe 1.12.1 reads
// only p0 of proc 1 and colors the model with it while the kit lasts (0x60D840, 0x60CB50): Frostbolt's chill,
// Frost Nova, Immolate, Drain Life... all tint steadily (the flashes are proc 13, world/SpellVisuals).
UnitFade fadeFromProcs(const std::array<u32, 4>& types, const std::array<f32, 16>& params);
// The look of several auras together: the most translucent wins (with its fade time); tints multiply.
UnitFade combineFades(const UnitFade& a, const UnitFade& b);
// The shown fade (rgb tint, a opacity) moved towards `target`: across the whole range in target.fadeSeconds, or at
// once.
glm::vec4 approachFade(const glm::vec4& shown, const UnitFade& target, f32 dt);

class UnitFades {
public:
    static constexpr u32 kProcColor = 1;
    static constexpr u32 kProcTranslucency = 14;

    // Spell.dbc, SpellVisual.dbc and SpellVisualKit.dbc from db.vdpk. False (and no fades) when they are missing.
    bool init(const pack::PackReader& db);
    // What a spell's aura does to its unit; nullptr: nothing.
    const UnitFade* ofSpell(u32 spellId) const;
    // A unit's look from its visible auras (combineFades of them all).
    UnitFade ofUnit(const Entity& unit) const;
    u32 size() const { return static_cast<u32>(m_spells.size()); }

private:
    std::vector<std::pair<u32, UnitFade>> m_spells; // sorted by spell id
    std::vector<u64> m_mask; // bit per spell id up to the highest in m_spells: most auras are rejected with one test
};

} // namespace vd::game
