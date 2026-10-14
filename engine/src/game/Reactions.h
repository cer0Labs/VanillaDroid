#pragma once

#include "core/Types.h"

#include <array>
#include <unordered_map>
#include <vector>

namespace vd::pack {
class PackReader;
}

namespace vd::game {

enum class Reaction : u8 { Hostile, Neutral, Friendly };

class ReputationList;

// Phase 13: the player's standing with the factions, which changes how their units look to it (vMaNGOS
// WorldObject::GetReactionTo / GetFactionReactionTo with a player): its forced reactions (SMSG_SET_FORCED_REACTIONS,
// SPELL_AURA_FORCE_REACTION: Faction.dbc id -> ReputationRank 0 hated .. 7 exalted) and, for factions with a
// reputation, the at-war flag of its reputation list (a reputation creature that attacked the player is hostile;
// any other one, friendly). game::Gameplay owns it; Reactions reads it (setPlayerStanding).
struct PlayerStanding {
    struct Forced {
        u32 faction = 0;
        u32 rank = 0;
    };
    u64 player = 0;                             // the player's GUID (0: not in the world)
    const ReputationList* reputation = nullptr; // its reputation list (game/Abilities)
    std::vector<Forced> forced;
};

// FactionTemplate.dbc group masks (vMaNGOS FactionMasks).
constexpr u32 kFactionGroupAlliance = 0x2;
constexpr u32 kFactionGroupHorde = 0x4;

// How units look to each other (phase 7: name and frame colors, what a tap on them does), from FactionTemplate.dbc
// with the server's own rules (vMaNGOS FactionTemplateEntry::IsHostileTo / IsFriendlyTo), and between the player
// and the others its standing (PlayerStanding: forced reactions, at war with a faction).
class Reactions {
public:
    void init(const pack::PackReader& db);
    bool ready() const { return !m_templates.empty(); }

    // How a unit of faction template `theirs` looks to a unit of `mine` (UNIT_FIELD_FACTIONTEMPLATE values).
    Reaction between(u32 mine, u32 theirs) const;
    // A FactionTemplate.dbc row given directly (tests, tools): faction, own group mask, friendly and hostile masks.
    void setTemplate(u32 id, u32 faction, u32 ourMask, u32 friendlyMask, u32 hostileMask);
    // The side of a faction template (UnitFactionGroup: its own group mask, kFactionGroup*); 0 unknown.
    u32 groupMask(u32 templateId) const;

    // Phase 13: the player's standing (may be null: between() alone). It must outlive this object's use.
    void setPlayerStanding(const PlayerStanding* standing) { m_standing = standing; }
    const PlayerStanding* playerStanding() const { return m_standing; }
    // How a unit of template `theirs` looks to the player (template `mine`): a forced reaction, else for a faction
    // with a reputation hostile when at war with it and friendly otherwise, else between().
    Reaction playerTowards(u32 mine, u32 theirs) const;
    // How the player (template `player`) looks to a unit of template `theirs`: a forced reaction, else hostile when
    // the player is at war with its reputation faction, else between().
    Reaction towardsPlayer(u32 theirs, u32 player) const;
    // A Faction.dbc row's reputation list id given directly (tests): -1 = none.
    void setFactionList(u32 faction, i32 listId) { m_factionList[faction] = listId; }

private:
    struct Template {
        u32 faction = 0;
        u32 ourMask = 0;
        u32 friendlyMask = 0;
        u32 hostileMask = 0;
        std::array<u32, 4> enemies{};
        std::array<u32, 4> friends{};

        bool friendlyTo(const Template& other) const;
        bool hostileTo(const Template& other) const;
    };
    std::unordered_map<u32, Template> m_templates;
    std::unordered_map<u32, i32> m_factionList; // Faction.dbc id -> reputation list id (only those with one)
    const PlayerStanding* m_standing = nullptr;

    // The player's forced reaction to `faction`, if any.
    const PlayerStanding::Forced* forced(u32 faction) const;
    // At war with `faction` (false for factions without a reputation).
    bool atWar(u32 faction) const;
};

} // namespace vd::game
