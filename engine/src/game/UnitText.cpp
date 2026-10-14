#include "game/UnitText.h"

#include "core/Text.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace vd::game {
namespace {

namespace uf = net::uf;

// The server's level at and below which a unit gives no experience (vMaNGOS Formulas.h GetGrayLevel).
u32 grayLevel(u32 playerLevel) {
    if (playerLevel <= 5) {
        return 0;
    }
    if (playerLevel <= 39) {
        return playerLevel - 5 - playerLevel / 10;
    }
    return playerLevel - 1 - playerLevel / 5;
}

} // namespace

Classification classification(u32 rank) {
    return rank <= 4 ? static_cast<Classification>(rank) : Classification::Normal;
}

bool isElite(u32 rank) {
    const Classification c = classification(rank);
    return c == Classification::Elite || c == Classification::RareElite;
}

i32 shownLevel(const Entity& me, const Entity& unit, u32 rank, const Reactions* reactions) {
    const u32 level = unit.value(uf::UNIT_FIELD_LEVEL);
    if (unit.type == ObjectType::Unit && classification(rank) == Classification::WorldBoss) {
        return -1;
    }
    const u32 mine = me.value(uf::UNIT_FIELD_LEVEL);
    const bool enemy = unit.guid != me.guid && (mayFight(me, unit, reactions) ||
                                                reactionTo(me, unit, reactions) == Reaction::Hostile);
    if (enemy && level >= mine + 10) {
        return -1;
    }
    return static_cast<i32>(level);
}

Difficulty levelDifficulty(u32 level, u32 playerLevel) {
    const i32 diff = static_cast<i32>(level) - static_cast<i32>(playerLevel);
    if (diff >= 5) {
        return Difficulty::Impossible;
    }
    if (diff >= 3) {
        return Difficulty::VeryDifficult;
    }
    if (diff >= -2) {
        return Difficulty::Difficult;
    }
    return level > grayLevel(playerLevel) ? Difficulty::Standard : Difficulty::Trivial;
}

u32 difficultyColor(Difficulty difficulty) {
    switch (difficulty) {
    case Difficulty::Impossible: return tooltipColor(255, 26, 26);     // 1.00, 0.10, 0.10
    case Difficulty::VeryDifficult: return tooltipColor(255, 128, 64); // 1.00, 0.50, 0.25
    case Difficulty::Difficult: return tooltipColor(255, 255, 0);      // 1.00, 1.00, 0.00
    case Difficulty::Standard: return tooltipColor(64, 191, 64);       // 0.25, 0.75, 0.25
    default: return tooltipColor(128, 128, 128);                       // 0.50, 0.50, 0.50
    }
}

u32 levelColor(const Entity& me, const Entity& unit, const Reactions* reactions) {
    if (!mayFight(me, unit, reactions)) {
        return kTooltipGold; // GameFontNormalSmall
    }
    return difficultyColor(levelDifficulty(unit.value(uf::UNIT_FIELD_LEVEL), me.value(uf::UNIT_FIELD_LEVEL)));
}

u32 targetNameBackground(NameColor color) {
    switch (color) {
    case NameColor::Hostile: return tooltipColor(255, 0, 0);
    case NameColor::Neutral: return tooltipColor(255, 255, 0);
    case NameColor::Friendly: return tooltipColor(0, 255, 0);
    case NameColor::FriendlyPlayer: return tooltipColor(0, 0, 255);
    default: return tooltipColor(128, 128, 128); // tapped by someone else
    }
}

u32 tooltipNameColor(NameColor color) {
    switch (color) {
    case NameColor::Hostile: return tooltipColor(204, 77, 56); // 0.8, 0.3, 0.22
    case NameColor::Neutral: return tooltipColor(230, 179, 0); // 0.9, 0.7, 0
    case NameColor::Friendly: return tooltipColor(0, 153, 26); // 0, 0.6, 0.1
    case NameColor::FriendlyPlayer: return kTooltipWhite;
    default: return tooltipColor(128, 128, 128);
    }
}

bool hordeRace(u8 race) {
    return race == 2 || race == 5 || race == 6 || race == 8;
}

PvpBadge pvpBadge(const Entity& unit, const Reactions* reactions) {
    if (isFreeForAllPvp(unit)) {
        return PvpBadge::FreeForAll;
    }
    if (!isPvpFlagged(unit)) {
        return PvpBadge::None;
    }
    u32 group = reactions ? reactions->groupMask(unit.value(uf::UNIT_FIELD_FACTIONTEMPLATE)) : 0;
    if (group == 0 && unit.type == ObjectType::Player) { // no table: a player's side is its race's
        group =
            hordeRace(unit.byteValue(uf::UNIT_FIELD_BYTES_0, 0)) ? kFactionGroupHorde : kFactionGroupAlliance;
    }
    if (group & kFactionGroupAlliance) {
        return PvpBadge::Alliance;
    }
    return (group & kFactionGroupHorde) ? PvpBadge::Horde : PvpBadge::None;
}

TooltipLine& UnitTooltip::add(std::string_view text, u32 color) {
    if (m_count >= m_lines.size()) {
        m_overflow = {};
        return m_overflow;
    }
    const u32 length = static_cast<u32>(std::min<usize>(text.size(), m_text.size() - m_used));
    std::memcpy(m_text.data() + m_used, text.data(), length);
    TooltipLine& line = m_lines[m_count++];
    line = {};
    line.left = {m_text.data() + m_used, length};
    line.leftColor = color;
    m_used += length;
    return line;
}

void buildUnitTooltip(const Entity& me, const Entity& unit, const UnitNames& names,
                      const Reactions* reactions, UnitTooltip& out) {
    out.clear();
    if (!isUnit(unit)) {
        return;
    }
    const bool player = unit.type == ObjectType::Player;
    char buffer[256];
    char level[16];

    // The name, a player's PvP rank title before it (PLAYER_BYTES_3 byte 3: 5 Private / Scout .. 18 Grand
    // Marshal / High Warlord; PVP_RANK_<rank>_<0 Horde, 1 Alliance>[_FEMALE]).
    const std::string_view name = names.name.empty() ? text::get("UNKNOWNOBJECT") : names.name;
    const u32 nameColor = tooltipNameColor(unitNameColor(me, unit, reactions));
    const u32 honorRank = player ? unit.byteValue(uf::PLAYER_BYTES_3, 3) : 0;
    if (honorRank >= 5 && honorRank <= 18) {
        char key[40];
        std::snprintf(key, sizeof(key), "PVP_RANK_%u_%u%s", honorRank,
                      hordeRace(unit.byteValue(uf::UNIT_FIELD_BYTES_0, 0)) ? 0u : 1u,
                      unit.byteValue(uf::UNIT_FIELD_BYTES_0, 2) == 1 ? "_FEMALE" : "");
        const text::Key rankKey{std::string_view(key)};
        if (text::has(rankKey)) {
            out.add(text::format(buffer, "UNIT_PVP_NAME", text::get(rankKey), name), nameColor);
        } else {
            out.add(name, nameColor);
        }
    } else {
        out.add(name, nameColor);
    }

    // Who it is to the world: a creature's title, the owner of a pet or charmed creature, a player's guild.
    if (!player && !names.subname.empty()) {
        out.add(text::format(buffer, "UNITNAME_TITLE", names.subname));
    } else if (!player && !names.owner.empty()) {
        const bool charmed = guidValue(unit, uf::UNIT_FIELD_CHARMEDBY) != 0;
        out.add(text::format(buffer,
                             charmed ? text::Key("UNITNAME_TITLE_CHARM") : text::Key("UNITNAME_TITLE_PET"),
                             names.owner));
    } else if (player && !names.guild.empty()) {
        out.add(text::format(buffer, "VD_UNIT_GUILD", names.guild));
    }

    // The level line.
    const i32 shown = shownLevel(me, unit, names.rank, reactions);
    if (shown >= 0) {
        std::snprintf(level, sizeof(level), "%d", shown);
    } else {
        std::snprintf(level, sizeof(level), "??");
    }
    if (player) {
        out.add(text::format(buffer, "PLAYER_LEVEL", std::string_view(level), names.race, names.cls));
    } else if (isDead(unit)) {
        out.add(shown >= 0 ? text::format(buffer, "UNIT_LEVEL_DEAD_TEMPLATE", shown)
                           : text::format(buffer, "UNIT_LETHAL_LEVEL_DEAD_TEMPLATE"));
    } else {
        const std::string_view type = names.creatureType;
        const bool elite = isElite(names.rank);
        if (shown < 0) {
            out.add(type.empty() ? text::format(buffer, "UNIT_LETHAL_LEVEL_TEMPLATE")
                                 : text::format(buffer, "UNIT_TYPE_LETHAL_LEVEL_TEMPLATE", type));
        } else if (type.empty()) {
            out.add(text::format(
                buffer, elite ? text::Key("UNIT_PLUS_LEVEL_TEMPLATE") : text::Key("UNIT_LEVEL_TEMPLATE"),
                shown));
        } else {
            out.add(text::format(buffer,
                                 elite ? text::Key("UNIT_TYPE_PLUS_LEVEL_TEMPLATE")
                                       : text::Key("UNIT_TYPE_LEVEL_TEMPLATE"),
                                 shown, type));
        }
    }

    if (isPvpFlagged(unit) || isFreeForAllPvp(unit)) {
        out.add(text::get("PVP_ENABLED"));
    }
    if (isDead(unit) && isSkinnable(unit)) {
        out.add(text::get("UNIT_SKINNABLE"));
    }
}

} // namespace vd::game
