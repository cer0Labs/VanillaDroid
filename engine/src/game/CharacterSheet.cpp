#include "game/CharacterSheet.h"

#include "core/Text.h"
#include "game/Abilities.h"
#include "game/UnitInfo.h"
#include "net/UpdateFields.gen.h"

#include <cstdio>

namespace vd::game {
namespace {

namespace uf = net::uf;
using net::Opcode;

constexpr u32 kHighestRankShown = 5; // GetPVPLifetimeStats reads a highest rank under 5 as none

} // namespace

CharacterSheet::CharacterSheet(const EntityStore& entities, Abilities& abilities)
    : m_entities(entities), m_abilities(abilities) {
    m_body.reserve(16);
}

void CharacterSheet::enterWorld(u64 player, bool newLogin) {
    m_inWorld = true;
    m_player = player;
    if (newLogin) {
        setInspected(0);
    }
}

void CharacterSheet::leaveWorld() {
    m_inWorld = false;
    setInspected(0);
}

bool CharacterSheet::handles(Opcode opcode) {
    return opcode == Opcode::MSG_INSPECT_HONOR_STATS;
}

bool CharacterSheet::handle(Opcode opcode, std::span<const u8> payload) {
    if (opcode != Opcode::MSG_INSPECT_HONOR_STATS) {
        return true;
    }
    net::InspectHonorStats m;
    if (!net::parseInspectHonorStats(payload, m)) {
        return false;
    }
    if (m.player == m_inspected && m_inspected != 0) {
        m_honor = m;
        m_honorKnown = true; // INSPECT_HONOR_UPDATE: the frame reads it from now on
        m_honorAsked = false;
    }
    return true;
}

void CharacterSheet::setInspected(u64 player) {
    if (player == m_inspected) {
        return;
    }
    m_inspected = player;
    m_honorKnown = false;
    m_honorAsked = false;
}

bool CharacterSheet::requestInspectHonor() {
    if (!m_inWorld || m_inspected == 0 || m_honorKnown || m_honorAsked) {
        return false;
    }
    m_body.clear();
    net::writeInspectHonorRequest(m_body, m_inspected);
    m_honorAsked = sendBody(Opcode::MSG_INSPECT_HONOR_STATS);
    return m_honorAsked;
}

i32 CharacterSheet::watchedFaction() const {
    const Entity* me = m_entities.find(m_player);
    return me ? static_cast<i32>(me->value(uf::PLAYER_FIELD_WATCHED_FACTION_INDEX)) : -1;
}

bool CharacterSheet::setWatchedFaction(i32 listId) {
    if (!m_inWorld || listId == watchedFaction()) {
        return false; // 0x4D6240: only a change goes to the server
    }
    m_body.clear();
    net::writeSetWatchedFaction(m_body, listId);
    return sendBody(Opcode::CMSG_SET_WATCHED_FACTION);
}

bool CharacterSheet::setFactionInactive(u32 listId, bool inactive) {
    if (!m_inWorld || listId >= net::kFactionListSize) {
        return false;
    }
    m_body.clear();
    net::writeSetFactionInactive(m_body, listId, inactive);
    if (!sendBody(Opcode::CMSG_SET_FACTION_INACTIVE)) {
        return false;
    }
    m_abilities.reputation().setInactive(listId, inactive);
    return true;
}

CharacterSheet::WarResult CharacterSheet::toggleAtWar(u32 listId, i32 value) {
    const bool war = (m_abilities.reputation().entry(listId).flags & net::kFactionAtWar) == 0;
    if (!war && value < -3000) {
        return WarResult::Refused; // peace only from Unfriendly up
    }
    const Entity* me = m_entities.find(m_player);
    if (me && inCombat(*me)) {
        return WarResult::InCombat;
    }
    return m_abilities.setAtWar(listId, war) ? WarResult::Sent : WarResult::Refused;
}

bool CharacterSheet::unlearnSkill(u32 skillId) {
    if (!m_inWorld) {
        return false;
    }
    m_body.clear();
    net::writeUnlearnSkill(m_body, skillId);
    return sendBody(Opcode::CMSG_UNLEARN_SKILL);
}

bool CharacterSheet::sendBody(Opcode opcode) {
    return m_sink(opcode, m_body);
}

// -------------------------------------------------------------------------------------------------------
// Honor.

bool allianceRace(u32 race) {
    return race == 1 || race == 3 || race == 4 || race == 7; // human, dwarf, night elf, gnome
}

HonorStats honorStats(const Entity& player) {
    HonorStats h;
    const u32 today = player.value(uf::PLAYER_FIELD_SESSION_KILLS);
    h.todayHonorable = today & 0xFFFFu;
    h.todayDishonorable = today >> 16;
    h.yesterdayHonorable = player.value(uf::PLAYER_FIELD_YESTERDAY_KILLS) & 0xFFFFu;
    h.yesterdayHonor = player.value(uf::PLAYER_FIELD_YESTERDAY_CONTRIBUTION);
    h.thisWeekHonorable = player.value(uf::PLAYER_FIELD_THIS_WEEK_KILLS) & 0xFFFFu;
    h.thisWeekHonor = player.value(uf::PLAYER_FIELD_THIS_WEEK_CONTRIBUTION);
    h.lastWeekHonorable = player.value(uf::PLAYER_FIELD_LAST_WEEK_KILLS) & 0xFFFFu;
    h.lastWeekHonor = player.value(uf::PLAYER_FIELD_LAST_WEEK_CONTRIBUTION);
    h.lastWeekStanding = player.value(uf::PLAYER_FIELD_LAST_WEEK_RANK);
    h.lifetimeHonorable = player.value(uf::PLAYER_FIELD_LIFETIME_HONORBALE_KILLS);
    h.lifetimeDishonorable = player.value(uf::PLAYER_FIELD_LIFETIME_DISHONORBALE_KILLS);
    const u32 highest = player.byteValue(uf::PLAYER_FIELD_BYTES, 3);
    h.highestRank = highest >= kHighestRankShown ? highest : 0;
    h.rank = player.byteValue(uf::PLAYER_BYTES_3, 3);
    h.progress = static_cast<f32>(player.byteValue(uf::PLAYER_FIELD_BYTES2, 0)) / 255.0f;
    h.alliance = allianceRace(player.byteValue(uf::UNIT_FIELD_BYTES_0, 0));
    return h;
}

HonorStats honorStats(const net::InspectHonorStats& m, const Entity* unit) {
    HonorStats h;
    h.todayHonorable = m.todayHonorable;
    h.todayDishonorable = m.todayDishonorable;
    h.yesterdayHonorable = m.yesterdayHonorable;
    h.yesterdayHonor = m.yesterdayHonor;
    h.thisWeekHonorable = m.thisWeekHonorable;
    h.thisWeekHonor = m.thisWeekHonor;
    h.lastWeekHonorable = m.lastWeekHonorable;
    h.lastWeekHonor = m.lastWeekHonor;
    h.lastWeekStanding = m.lastWeekStanding;
    h.lifetimeHonorable = m.lifetimeHonorable;
    h.lifetimeDishonorable = m.lifetimeDishonorable;
    h.highestRank = m.highestRank; // GetInspectHonorData does not hide the low ranks
    h.progress = static_cast<f32>(m.rankProgress) / 255.0f;
    if (unit && unit->type == ObjectType::Player) {
        h.rank = unit->byteValue(uf::PLAYER_BYTES_3, 3);
        h.alliance = allianceRace(unit->byteValue(uf::UNIT_FIELD_BYTES_0, 0));
    }
    return h;
}

std::string_view pvpRankKey(std::span<char> out, u32 rank, bool alliance, bool female) {
    if (rank < 1 || rank > 18 || out.size() < 24) {
        return {};
    }
    int n = std::snprintf(out.data(), out.size(), "PVP_RANK_%u_%u_FEMALE", rank, alliance ? 1u : 0u);
    if (!female || n <= 0 || !text::has(text::Key(std::string_view(out.data(), static_cast<usize>(n))))) {
        n = std::snprintf(out.data(), out.size(), "PVP_RANK_%u_%u", rank, alliance ? 1u : 0u);
    }
    return n > 0 ? std::string_view(out.data(), static_cast<usize>(n)) : std::string_view{};
}

i32 pvpRankNumber(u32 rank) {
    if (rank < 1 || rank > 18) {
        return 0;
    }
    return rank >= 5 ? static_cast<i32>(rank) - 4 : static_cast<i32>(rank) - 5;
}

} // namespace vd::game
