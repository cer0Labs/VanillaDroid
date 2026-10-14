#include "game/BattlefieldScore.h"

#include "core/Profile.h"
#include "core/Text.h"
#include "game/Battlegrounds.h"
#include "game/CharacterSheet.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/Notices.h"
#include "game/Queries.h"
#include "game/Social.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>

namespace vd::game {
namespace {

using net::Opcode;

// ChrRaces' faction template's group (0x4AA26F, 0x5EFE00): Horde 0, Alliance 1, -1 a race neither side has.
i32 sideOfRace(u32 race) {
    if (race == 0 || race > 8) {
        return -1;
    }
    return allianceRace(race) ? 1 : 0;
}

} // namespace

BattlefieldScore::BattlefieldScore(const EntityStore& entities, const Social& social, QueryCache& queries,
                                   Battlegrounds& battlegrounds, Events& events)
    : m_entities(entities), m_social(social), m_queries(queries), m_battlegrounds(battlegrounds),
      m_events(events) {
    m_body.reserve(8);
}

void BattlefieldScore::enterWorld(u64 player, u32 nowMs) {
    (void)nowMs;
    // 0x4A9DB0: the score, its filter, the columns, the positions and both timers start over.
    m_inWorld = true;
    m_player = player;
    m_count = m_shown = 0;
    m_ready = m_waiting = m_ended = false;
    m_faction = -1;
    m_scoreAsked = m_positionsAsked = false;
    m_statCount = 0;
    m_statsSlot = -1;
    m_eventCursor = m_events.end(); // only a status from now on collects the columns
    m_positions.count = 0;
    m_positions.hasCarrier = false;
    ++m_changes;
    ++m_worldEntries;
}

void BattlefieldScore::leaveWorld() {
    m_inWorld = false;
}

bool BattlefieldScore::handles(Opcode opcode) {
    switch (opcode) {
    case Opcode::MSG_PVP_LOG_DATA:
    case Opcode::MSG_BATTLEGROUND_PLAYER_POSITIONS:
    case Opcode::SMSG_BATTLEGROUND_PLAYER_JOINED:
    case Opcode::SMSG_BATTLEGROUND_PLAYER_LEFT:
    case Opcode::SMSG_BATTLEFIELD_WIN:
    case Opcode::SMSG_BATTLEFIELD_LOSE: return true;
    default: return false;
    }
}

bool BattlefieldScore::handle(Opcode opcode, std::span<const u8> payload, u32 nowMs) {
    switch (opcode) {
    case Opcode::MSG_PVP_LOG_DATA:
        if (!net::parsePvpLogData(payload, m_packet)) {
            return false;
        }
        apply(m_packet, nowMs);
        return true;
    case Opcode::MSG_BATTLEGROUND_PLAYER_POSITIONS:
        if (!net::parseBattlegroundPositions(payload, m_positions)) {
            m_positions.count = 0;
            m_positions.hasCarrier = false;
            return false;
        }
        return true;
    case Opcode::SMSG_BATTLEGROUND_PLAYER_JOINED:
    case Opcode::SMSG_BATTLEGROUND_PLAYER_LEFT: {
        u64 player = 0;
        if (!net::parseBattlegroundPlayer(payload, player)) {
            return false;
        }
        m_queries.playerName(player); // 0x4AAE50: asked; the line waits for it (game::NoticeFeed)
        GameEvent& e = m_events.push();
        e = GameEvent{};
        e.kind = GameEventKind::BattlegroundPlayer;
        e.timeMs = nowMs;
        e.guid = player;
        e.value[0] = opcode == Opcode::SMSG_BATTLEGROUND_PLAYER_JOINED ? 1 : 0;
        return true;
    }
    case Opcode::SMSG_BATTLEFIELD_WIN:
    case Opcode::SMSG_BATTLEFIELD_LOSE:
        return true; // no handler in 1.12.1: the SMSG_PLAY_SOUND is the victory's
    default: return false;
    }
}

void BattlefieldScore::apply(const net::PvpLogData& data, u32 nowMs) {
    // 0x4AAB8B: the end and the winner at once (GetBattlefieldWinner), the lines once their names are known.
    m_ended = data.ended;
    m_battlegrounds.setWinner(data.ended ? static_cast<i32>(data.winner) : -1);
    m_battlegrounds.setEnded(data.ended); // LeaveBattlefield's condition
    m_count = data.count;
    for (u32 i = 0; i < m_count; ++i) {
        const net::PvpLogScore& s = data.scores[i];
        BattlefieldScoreLine& line = m_lines[i];
        line = BattlefieldScoreLine{};
        line.guid = s.guid;
        line.rank = s.rank;
        line.killingBlows = s.killingBlows;
        line.honorableKills = s.honorableKills;
        line.deaths = s.deaths;
        line.bonusHonor = s.bonusHonor;
        line.stats = s.extra;
        m_queries.playerInfo(s.guid); // the name query (0x4AAC79) when the cache has not got it
    }
    m_ready = false;
    m_shown = 0;
    m_waiting = true;
    ++m_changes;
    if (namesKnown()) {
        finish(nowMs);
    }
}

bool BattlefieldScore::namesKnown() const {
    for (u32 i = 0; i < m_count; ++i) {
        if (!m_queries.findPlayerInfo(m_lines[i].guid) || !m_queries.findPlayerName(m_lines[i].guid)) {
            return false;
        }
    }
    return true;
}

void BattlefieldScore::finish(u32 nowMs) {
    m_waiting = false;
    m_ready = true;
    m_shown = 0;
    for (u32 i = 0; i < m_count; ++i) {
        BattlefieldScoreLine& line = m_lines[i];
        const PlayerInfo* info = m_queries.findPlayerInfo(line.guid);
        line.race = info ? info->race : 0;
        line.cls = info ? info->cls : 0;
        line.faction = sideOfRace(line.race);
        m_shown += m_faction == -1 || line.faction == m_faction ? 1 : 0;
    }
    // 0x4AA350: the filter's side first, then killing blows (more first), deaths (fewer first), bonus honor
    // (more first) and the name (SStrCmp, ascending). A name it cannot find compares equal. WoW.exe's qsort;
    // here an insertion sort of the 80 lines at most (no allocation, stable).
    const auto before = [this](const BattlefieldScoreLine& a, const BattlefieldScoreLine& b) {
        const std::string* na = m_queries.findPlayerName(a.guid);
        const std::string* nb = m_queries.findPlayerName(b.guid);
        if (!na || !nb) {
            return false;
        }
        if (m_faction != -1 && a.faction != b.faction) {
            return a.faction == m_faction;
        }
        if (a.killingBlows != b.killingBlows) {
            return a.killingBlows > b.killingBlows;
        }
        if (a.deaths != b.deaths) {
            return a.deaths < b.deaths;
        }
        if (a.bonusHonor != b.bonusHonor) {
            return a.bonusHonor > b.bonusHonor;
        }
        return *na < *nb;
    };
    for (u32 i = 0; i < m_count; ++i) {
        m_order[i] = static_cast<u8>(i);
    }
    for (u32 i = 1; i < m_count; ++i) {
        const u8 moving = m_order[i];
        u32 j = i;
        while (j > 0 && before(m_lines[moving], m_lines[m_order[j - 1]])) {
            m_order[j] = m_order[j - 1];
            --j;
        }
        m_order[j] = moving;
    }
    ++m_changes;
    GameEvent& e = m_events.push(); // UPDATE_BATTLEFIELD_SCORE
    e = GameEvent{};
    e.kind = GameEventKind::BattlefieldScore;
    e.timeMs = nowMs;
    e.id = m_shown;
}

void BattlefieldScore::setFaction(i32 faction, u32 nowMs) {
    if (faction != -1 && (faction < 0 || faction >= 2)) {
        return; // 0x4AA5A0
    }
    m_faction = faction;
    if (!m_waiting) { // 0x4AA216: not while names are awaited (their arrival sorts)
        finish(nowMs);
    }
}

void BattlefieldScore::update(u32 nowMs) {
    VD_PROFILE_ZONE("BattlefieldScore::update");
    while (const GameEvent* e = m_events.next(m_eventCursor)) {
        if (e->kind != GameEventKind::BattlefieldStatus) {
            continue;
        }
        const auto state = static_cast<BattlefieldState>(e->value[0]);
        if (state == BattlefieldState::Active) {
            // 0x4AA99D: the columns of the queue that turned active; its filter starts over (0x4AAA57).
            const u32 map = m_battlegrounds.queue(e->id).mapId;
            m_statCount = m_data && map != 0 ? m_data->statColumns(map, m_stats) : 0;
            m_statsSlot = static_cast<i32>(e->id);
            m_faction = -1;
            if (!m_waiting) {
                finish(nowMs);
            }
        } else if (state == BattlefieldState::None && static_cast<i32>(e->id) == m_statsSlot) {
            m_statCount = 0; // 0x4AAAE0: the active queue emptied takes the columns and the positions with it
            m_statsSlot = -1;
            m_positions.count = 0;
            m_positions.hasCarrier = false;
        }
    }
    if (m_waiting && namesKnown()) {
        finish(nowMs);
    }
}

bool BattlefieldScore::sendEmpty(Opcode opcode) {
    m_body.clear();
    return m_inWorld && m_sink(opcode, m_body);
}

bool BattlefieldScore::requestScore(u32 nowMs) {
    if (m_scoreAsked && static_cast<i32>(nowMs - m_scoreAllowedAtMs) < 0) {
        return false; // 0x4AA18E
    }
    if (!sendEmpty(Opcode::MSG_PVP_LOG_DATA)) {
        return false;
    }
    m_scoreAsked = true;
    m_scoreAllowedAtMs = nowMs + kRequestEveryMs;
    return true;
}

bool BattlefieldScore::requestPositions(u32 nowMs) {
    const i32 slot = m_battlegrounds.activeSlot();
    if (slot < 0 || m_battlegrounds.queue(static_cast<u32>(slot)).mapId == 0) {
        m_positions.count = 0; // 0x4AA5DF
        return false;
    }
    if (m_positionsAsked && static_cast<i32>(nowMs - m_positionsAllowedAtMs) < 0) {
        return false;
    }
    if (!sendEmpty(Opcode::MSG_BATTLEGROUND_PLAYER_POSITIONS)) {
        return false;
    }
    m_positionsAsked = true;
    m_positionsAllowedAtMs = nowMs + kRequestEveryMs;
    return true;
}

const BattlefieldScoreLine& BattlefieldScore::line(u32 index) const {
    static const BattlefieldScoreLine kNone{};
    return m_ready && index < m_count ? m_lines[m_order[index]] : kNone;
}

std::string_view BattlefieldScore::name(const BattlefieldScoreLine& line) const {
    const std::string* known = line.guid != 0 ? m_queries.findPlayerName(line.guid) : nullptr;
    return known ? std::string_view(*known) : std::string_view{};
}

bool BattlefieldScore::groupOrSelf(u64 guid) const {
    if (guid == m_player) {
        return true;
    }
    const GroupState& group = m_social.group();
    if (!group.active) {
        return false;
    }
    for (const GroupMember& m : group.members) {
        if (m.guid == guid) {
            return true; // a party slot (0x4E7F70) or the raid's list (0x4BAEE0)
        }
    }
    return false;
}

u32 BattlefieldScore::positionCount() const {
    u32 n = 0;
    for (u32 i = 0; i < m_positions.count; ++i) {
        n += groupOrSelf(m_positions.players[i].guid) ? 0 : 1;
    }
    return n;
}

bool BattlefieldScore::position(u32 index, u32 nowMs, BattlefieldPosition& out) {
    for (u32 i = 0; i < m_positions.count; ++i) {
        const net::BattlegroundPosition& p = m_positions.players[i];
        if (groupOrSelf(p.guid)) {
            continue;
        }
        if (index-- != 0) {
            continue;
        }
        out.guid = p.guid;
        out.x = p.x;
        out.y = p.y;
        if (const Entity* unit = m_entities.find(p.guid);
            unit && unit->type == ObjectType::Player && unit->hasPosition) {
            const glm::vec3 at =
                unit->positionAt(nowMs); // 0x4AC07B: the player itself while the client has it
            out.x = at.x;
            out.y = at.y;
        }
        const std::string* name = m_queries.playerName(p.guid); // asked (0x4AC113) while unknown
        out.name = name ? std::string_view(*name) : std::string_view{};
        return true;
    }
    return false;
}

bool BattlefieldScore::flagCarrier(u32 nowMs, BattlefieldFlag& out) const {
    if (!m_positions.hasCarrier || m_positions.carrier.guid == 0) {
        return false;
    }
    out.guid = m_positions.carrier.guid;
    out.x = m_positions.carrier.x;
    out.y = m_positions.carrier.y;
    if (const Entity* unit = m_entities.find(out.guid);
        unit && unit->type == ObjectType::Player && unit->hasPosition) {
        const glm::vec3 at = unit->positionAt(nowMs); // 0x4AC2AD: where the client sees it
        out.x = at.x;
        out.y = at.y;
    }
    // 0x4AA670: the token is the other side's flag than the PLAYER's (0x468550 is the active player, 0x5EFE00
    // its side): the carrier the server sends is the player's teammate holding the enemy flag. The player's
    // race from its own fields, else from its name query.
    u32 race = 0;
    if (const Entity* me = m_entities.find(m_player)) {
        race = me->byteValue(net::uf::UNIT_FIELD_BYTES_0, 0);
    } else if (const PlayerInfo* info = m_queries.findPlayerInfo(m_player)) {
        race = info->race;
    }
    const i32 side = sideOfRace(race);
    out.token = side == 0   ? BattlefieldFlag::Token::AllianceFlag
                : side == 1 ? BattlefieldFlag::Token::HordeFlag
                            : BattlefieldFlag::Token::Unknown;
    return true;
}

void BattlefieldScore::demoScore(const net::PvpLogData& data, u32 nowMs) {
    apply(data, nowMs);
}

void BattlefieldScore::demoPositions(const net::BattlegroundPositions& positions) {
    m_positions = positions;
}

void BattlefieldScore::demoStats(u32 map) {
    m_statCount = m_data && map != 0 ? m_data->statColumns(map, m_stats) : 0;
}

// game::NoticeFeed's part: the line once the name is known (0x4AAEF0 / 0x4AAFE0, the name query's callbacks;
// the error table's entries 444 and 445, display type 0: the chat, CHAT_MSG_SYSTEM). The joined one links the
// name as the chat links players ("|Hplayer:Aeryn|h[Aeryn]|h").
void NoticeFeed::battlegroundPlayerLine(Gameplay& play, const GameEvent& e) {
    const std::string* name = play.queries().findPlayerName(e.guid);
    const std::string_view who = name ? std::string_view(*name) : std::string_view("...");
    if (e.value[0] != 0) {
        text::format(push(NoticeKind::System), "ERR_BG_PLAYER_JOINED_SS", who, who);
    } else {
        text::format(push(NoticeKind::System), "ERR_BG_PLAYER_LEFT_S", who);
    }
}

} // namespace vd::game
