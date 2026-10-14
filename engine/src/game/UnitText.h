#pragma once

#include "core/Types.h"
#include "game/Entities.h"
#include "game/ItemTooltip.h"
#include "game/Reactions.h"
#include "game/UnitInfo.h"

#include <array>
#include <span>
#include <string_view>

namespace vd::game {

// What the client's interface says about a unit (parity pass): its classification, the level it shows and in
// which color, its PvP badge, the colors of its name and the lines of its tooltip (GameTooltip:SetUnit), from
// its update fields and the names the caller knows (queries, guilds, DBC tables). Pure functions: no state,
// no allocation.

// creature_template.rank as the creature query tells it (UnitClassification): the target frame's art.
enum class Classification : u8 { Normal = 0, Elite = 1, RareElite = 2, WorldBoss = 3, Rare = 4 };
Classification classification(u32 rank);
// The "plus mobs" of the tooltip's level line (UNIT_TYPE_PLUS_LEVEL_TEMPLATE): elites and rare elites.
bool isElite(u32 rank);

// UnitLevel: the unit's level, or -1 ("??", the target frame's skull) for world bosses and for units the
// player could fight (or that are hostile to it) 10 or more levels above its own. Friendly units always show
// their level.
i32 shownLevel(const Entity& me, const Entity& unit, u32 rank, const Reactions* reactions);

// GetDifficultyColor (QuestLogFrame.lua) of a level against the player's. The gray level is the server's
// (vMaNGOS Formulas.h GetGrayLevel: no experience at or below it).
enum class Difficulty : u8 { Impossible, VeryDifficult, Difficult, Standard, Trivial };
Difficulty levelDifficulty(u32 level, u32 playerLevel);
u32 difficultyColor(Difficulty difficulty); // QuestDifficultyColor, RGBA8
// TargetFrame_CheckLevel: the level text in its difficulty color when the player may attack the unit
// (UnitCanAttack: game::mayFight), gold (1, 0.82, 0) otherwise.
u32 levelColor(const Entity& me, const Entity& unit, const Reactions* reactions);

// unitNameColor's classes as each view paints them (RGBA8): behind the target frame's name (UnitReactionColor
// in TargetFrame_CheckFaction; blue for players nobody may fight) and on the tooltip's first line
// (FACTION_BAR_COLORS in GameTooltip_UnitColor; white for those players). Tapped is the target frame's gray
// in both.
u32 targetNameBackground(NameColor color);
u32 tooltipNameColor(NameColor color);

// The PvP badge of a unit frame (PlayerFrame_UpdatePvPStatus, TargetFrame_CheckFaction): UI-PVP-FFA for a
// player in a free-for-all area, else the side's badge (UnitFactionGroup: its faction template's group) while
// flagged.
enum class PvpBadge : u8 { None, Alliance, Horde, FreeForAll };
PvpBadge pvpBadge(const Entity& unit, const Reactions* reactions);
// The playable races of the Horde (Orc, Undead, Tauren, Troll): the side of a player's PvP rank titles.
bool hordeRace(u8 race);

// CreatureType.dbc's "Not specified": a creature type the tooltip does not name.
constexpr u32 kCreatureTypeNotSpecified = 10;

// What the caller knows by name about a unit (empty: unknown, the line is left out; an unknown name reads
// UNKNOWNOBJECT, as the client's while its query is out).
struct UnitNames {
    std::string_view name;         // the player's (name query) or the creature's (creature query)
    std::string_view subname;      // a creature's title ("Innkeeper")
    std::string_view guild;        // a player's guild
    std::string_view owner;        // the player a pet or a charmed creature belongs to
    std::string_view race, cls;    // a player's race and class (ChrRaces / ChrClasses names)
    std::string_view creatureType; // a creature's type (CreatureType.dbc: "Beast"); empty: none
    u32 rank = 0;                  // creature_template.rank
};

// A unit tooltip's lines in fixed storage (rebuild it whenever it is shown; nothing allocates).
class UnitTooltip {
public:
    static constexpr u32 kMaxLines = 8;

    void clear() {
        m_count = 0;
        m_used = 0;
    }
    std::span<const TooltipLine> lines() const { return {m_lines.data(), m_count}; }
    bool empty() const { return m_count == 0; }
    // Copies `text` in. Lines beyond kMaxLines, or text beyond the storage, are dropped.
    TooltipLine& add(std::string_view text, u32 color = kTooltipWhite);

private:
    std::array<TooltipLine, kMaxLines> m_lines{};
    u32 m_count = 0;
    std::array<char, 768> m_text{};
    u32 m_used = 0;
    TooltipLine m_overflow;
};

// GameTooltip:SetUnit with the 1.12 client's texts: the name in its color (a player's PvP rank title before
// it, UNIT_PVP_NAME), the creature's title, the pet's owner (UNITNAME_TITLE_PET / _CHARM) or the player's
// guild, the level line (PLAYER_LEVEL "Level 12 Human Warrior" for players; UNIT_TYPE_LEVEL_TEMPLATE "Level
// 10 Beast" and its Elite, ?? and Corpse forms for the rest), PVP_ENABLED while flagged, UNIT_SKINNABLE on a
// skinnable corpse.
void buildUnitTooltip(const Entity& me, const Entity& unit, const UnitNames& names,
                      const Reactions* reactions, UnitTooltip& out);

} // namespace vd::game
