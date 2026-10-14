#pragma once

#include "core/Types.h"
#include "game/Auras.h"
#include "world/EffectsVisualWorld.h"
#include "world/SpellVisuals.h"

#include <array>
#include <unordered_map>
#include <vector>

namespace vd {

class Actors;
class Effects;
class GameSession;
class Units;

// The spell visuals of what the server tells (phase 9): Gameplay::fx() (casts and their targets, hits,
// interruptions, the kits of SMSG_PLAY_SPELL_VISUAL / _IMPACT), the units' fields (their auras' state kits,
// their channels: UNIT_CHANNEL_SPELL and UNIT_FIELD_CHANNEL_OBJECT, for every unit) and the ground spells'
// dynamic objects, played by SpellVisuals on the units' actors (app/Units) with world/Effects; and the glows
// of the players' enchanted weapons (the visible item's enchantments: SpellItemEnchantment's item visual
// along the blade). Also the SpellVisualWorld of the online game: units by GUID. Game thread; per frame it
// compares each unit's channel, level and 48 aura fields with the last ones it saw (a few microseconds for a crowd)
// and never allocates once its units are known. A unit that gains a level shows the golden level up glow.
class SpellEffects final : public EffectsVisualWorld {
public:
    void init(const game::SpellVisualData* data, Effects* effects, Actors* actors, const Units* units);
    // After Units::update (the units are placed), before Effects::updateSpells.
    void update(GameSession& session, u32 nowMs, f32 dt, const glm::dvec3& camera);
    // Leaving the world: every visual stops.
    void clear();
    SpellVisuals& visuals() { return m_visuals; }
    // Phase 14 (spellfx): the kits' procs and shakes (beams, tints, flashes, the fishing line: SpellVisuals).
    void setKitFx(const game::SpellKitFx* fx, CameraShakes* shakes);
    // The units fishing now (a channel whose kit draws a line, at a fishing bobber): the caster's actor and
    // the bobber's GUID. Refreshed by update().
    struct FishingLine {
        u32 actor = ~0u;
        u64 bobber = 0;
    };
    std::span<const FishingLine> fishingLines() const { return m_lines; }

    // SpellVisualWorld: the server's units by GUID.
    bool unit(u64 key, Unit& out) const override;

private:
    // What was last seen of a unit's fields.
    struct Watched {
        u32 frame = 0;
        u32 channel = 0;
        u64 channelTarget = 0;
        std::array<u32, game::kMaxAuras> auras{};
        // Enchantment glows on the main and off hand's weapons: the enchantment shown and its models.
        std::array<u32, 2> enchant{};
        std::array<bool, 2> enchantShown{};
        std::array<std::array<EffectHandle, game::SpellVisualData::kItemVisualSlots>, 2> enchantFx{};
        EffectHandle lootFx; // parity: the sparkles over a corpse this client may loot (Particles\LootFX.m2)
        u32 level = 0;       // UNIT_FIELD_LEVEL (0: not seen yet)
    };
    void readFx(GameSession& session, u32 nowMs);
    void watchUnits(GameSession& session, u32 nowMs, const glm::dvec3& camera);
    void watchEnchants(const game::Entity& e, Watched& w, const glm::dvec3& camera);
    void stopEnchant(Watched& w, u32 hand);
    void levelUp(u64 guid);

    SpellVisuals m_visuals;
    const game::SpellVisualData* m_data = nullptr;
    const Units* m_units = nullptr;
    std::unordered_map<u64, Watched> m_watched;
    std::vector<u64> m_gone; // scratch
    const game::SpellKitFx* m_kitFx = nullptr; // phase 14
    std::vector<FishingLine> m_lines;
    u64 m_cursorsPlayer = 0;
    u32 m_fxCursor = 0;
    u32 m_frame = 0;
};

} // namespace vd
