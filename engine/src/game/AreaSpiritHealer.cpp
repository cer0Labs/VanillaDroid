#include "game/AreaSpiritHealer.h"

#include "core/Profile.h"
#include "game/Reactions.h"
#include "game/UnitInfo.h"
#include "net/GameMessages.h"
#include "net/PvpMessages.h"
#include "net/UpdateFields.gen.h"

namespace vd::game {
namespace {

using net::Opcode;
namespace uf = net::uf;

// UnitCanAssist as 0x4924C0 asks it of a spirit guide (0x6066F0): selectable, and friendly to the player.
bool canAssist(const Entity& player, const Entity& unit, const Reactions* reactions) {
    return (unit.value(uf::UNIT_FIELD_FLAGS) & kUnitFlagNotSelectable) == 0 &&
           reactionTo(player, unit, reactions) == Reaction::Friendly;
}

f32 distanceSquared(const Entity& a, const Entity& b, u32 nowMs) {
    const glm::vec3 d = a.positionAt(nowMs) - b.positionAt(nowMs);
    return d.x * d.x + d.y * d.y + d.z * d.z;
}

} // namespace

AreaSpiritHealer::AreaSpiritHealer(const EntityStore& entities, Events& events)
    : m_entities(entities), m_events(events) {
    m_body.reserve(16);
}

void AreaSpiritHealer::enterWorld(u64 player, u32 nowMs) {
    m_nowMs = nowMs;
    m_inWorld = true;
    m_player = player;
    m_healer = 0; // another map: whatever was chosen is gone (no packet: the server forgot it too)
    m_hasTime = false;
    m_waveAtMs = 0;
}

void AreaSpiritHealer::leaveWorld() {
    m_inWorld = false;
    m_healer = 0;
    m_hasTime = false;
    m_waveAtMs = 0;
}

bool AreaSpiritHealer::handles(Opcode opcode) {
    return opcode == Opcode::SMSG_AREA_SPIRIT_HEALER_TIME;
}

bool AreaSpiritHealer::handle(Opcode opcode, std::span<const u8> payload, u32 nowMs) {
    if (opcode != Opcode::SMSG_AREA_SPIRIT_HEALER_TIME) {
        return true;
    }
    m_nowMs = nowMs;
    net::AreaSpiritHealerTime m;
    if (!net::parseAreaSpiritHealerTime(payload, m)) {
        return false;
    }
    update(nowMs, m_reactions); // 0x492296: the healer is looked at first
    if (m.healer != m_healer || static_cast<i32>(m.timeMs) <= 0) {
        return true; // another guide's (an old query), or the guide is not channeling: nothing
    }
    m_hasTime = true;
    m_waveAtMs = nowMs + m.timeMs;
    event(GameEventKind::AreaSpiritHealerInRange).value[0] = static_cast<i32>(m.timeMs);
    return true;
}

void AreaSpiritHealer::update(u32 nowMs, const Reactions* reactions) {
    VD_PROFILE_ZONE("AreaSpiritHealer::update");
    m_reactions = reactions;
    m_nowMs = nowMs;
    if (!m_inWorld) {
        return;
    }
    const Entity* me = m_entities.find(m_player);
    if (!me || !isGhost(*me)) {
        set(0, nowMs); // alive (or not seen yet): no healer
        return;
    }
    if (m_healer != 0) {
        const Entity* healer = m_entities.find(m_healer);
        if (!healer || distanceSquared(*me, *healer, nowMs) > kKeepRange * kKeepRange) {
            set(0, nowMs);
        }
    }
    if (m_healer != 0 || !me->hasPosition) {
        return;
    }
    // The first spirit guide close enough that the player may assist (WoW.exe walks its object list and the
    // last one it meets wins; two of the player's side within 20 yd never stand at one graveyard).
    u64 found = 0;
    m_entities.forEach([&](const Entity& e) {
        if (found != 0 || e.type != ObjectType::Unit || !e.hasPosition ||
            (npcFlags(e) & kNpcFlagSpiritGuide) == 0) {
            return;
        }
        if (distanceSquared(*me, e, nowMs) <= kFindRange * kFindRange && canAssist(*me, e, reactions)) {
            found = e.guid;
        }
    });
    if (found != 0) {
        set(found, nowMs);
    }
}

void AreaSpiritHealer::set(u64 guid, u32 nowMs) {
    m_nowMs = nowMs;
    if (guid == m_healer) {
        return;
    }
    m_healer = guid;
    if (m_hasTime) {
        cancelAura(); // its wave was coming: out of its queue (0x4921FC)
    }
    m_hasTime = false;
    m_waveAtMs = 0;
    if (guid != 0) {
        query(guid);
    }
}

bool AreaSpiritHealer::select(u64 guid, u32 nowMs) {
    m_nowMs = nowMs;
    const Entity* me = m_entities.find(m_player);
    if (!m_inWorld || !me || !isGhost(*me)) {
        return false;
    }
    set(0, nowMs);
    set(guid, nowMs);
    return true;
}

bool AreaSpiritHealer::accept() {
    if (m_healer == 0) {
        return false;
    }
    m_body.clear();
    net::writeAreaSpiritHealerQueue(m_body, m_healer);
    return m_inWorld && m_sink(Opcode::CMSG_AREA_SPIRIT_HEALER_QUEUE, m_body);
}

void AreaSpiritHealer::cancel() {
    cancelAura();
}

void AreaSpiritHealer::onAuraCancelled(u32 spellId) {
    if (spellId == kWaitingToResurrect) {
        event(GameEventKind::AreaSpiritHealerOutOfRange);
    }
}

u32 AreaSpiritHealer::timeLeftMs(u32 nowMs) const {
    if (!m_hasTime) {
        return 0;
    }
    const i32 left = static_cast<i32>(m_waveAtMs - nowMs);
    return left > 0 ? static_cast<u32>(left) : 0;
}

void AreaSpiritHealer::demoInRange(u64 healer, u32 timeMs, u32 nowMs) {
    m_nowMs = nowMs;
    m_healer = healer;
    m_hasTime = true;
    m_waveAtMs = nowMs + timeMs;
    event(GameEventKind::AreaSpiritHealerInRange).value[0] = static_cast<i32>(timeMs);
}

void AreaSpiritHealer::cancelAura() {
    // CancelAura(2584), 0x6E7040: the packet, and AREA_SPIRIT_HEALER_OUT_OF_RANGE for this spell.
    m_body.clear();
    net::writeU32(m_body, kWaitingToResurrect);
    if (m_inWorld) {
        m_sink(Opcode::CMSG_CANCEL_AURA, m_body);
    }
    onAuraCancelled(kWaitingToResurrect);
}

bool AreaSpiritHealer::query(u64 guid) {
    m_body.clear();
    net::writeAreaSpiritHealerQuery(m_body, guid);
    return m_inWorld && m_sink(Opcode::CMSG_AREA_SPIRIT_HEALER_QUERY, m_body);
}

GameEvent& AreaSpiritHealer::event(GameEventKind kind) {
    GameEvent& e = m_events.push();
    e = GameEvent{};
    e.kind = kind;
    e.timeMs = m_nowMs;
    e.guid = m_healer;
    return e;
}

} // namespace vd::game
