#include "game/PlayerCommands.h"

#include "game/ReadyCheck.h"
#include "game/Social.h"
#include "net/GameMessages.h"
#include "net/SocialMessages.h"
#include "net/UpdateFields.gen.h"

namespace vd::game {
namespace {

using net::Opcode;

constexpr u32 kPvpAnswerMs = 5000; // how long /pvp waits for its PLAYER_FLAGS change

} // namespace

PlayerCommands::PlayerCommands(const EntityStore& entities, const Social& social, Events& events)
    : m_entities(entities), m_social(social), m_events(events) {
    m_body.reserve(64);
}

void PlayerCommands::enterWorld(u64 player, bool newLogin, u32 nowMs) {
    m_inWorld = true;
    m_player = player;
    if (newLogin) {
        clearRaidTargets(nowMs);
        m_wasInGroup = false;
    }
    m_raidTargetsAsked = false; // a new map: asked again once the group is known
    m_pvpAsked = false;
}

void PlayerCommands::leaveWorld() {
    m_inWorld = false;
    m_pvpAsked = false;
}

void PlayerCommands::clearRaidTargets(u32 nowMs) {
    bool any = false;
    for (u64& guid : m_raidTargets) {
        any = any || guid != 0;
        guid = 0;
    }
    if (any) {
        ++m_raidTargetVersion;
        event(GameEventKind::RaidTargetChanged, nowMs);
    }
}

void PlayerCommands::update(u32 nowMs) {
    m_nowMs = nowMs;
    if (!m_inWorld) {
        return;
    }
    const bool inGroup = m_social.group().active;
    if (!inGroup && m_wasInGroup) {
        clearRaidTargets(nowMs); // the icons are the group's: gone with it
    }
    if (inGroup && !m_raidTargetsAsked) {
        m_raidTargetsAsked = requestRaidTargets();
    }
    if (!inGroup) {
        m_raidTargetsAsked = false;
    }
    m_wasInGroup = inGroup;

    if (m_pvpAsked) {
        const Entity* me = m_entities.find(m_player);
        const bool on = me && (me->value(net::uf::PLAYER_FLAGS) & kPlayerFlagPvpDesired) != 0;
        if (me && on != m_pvpWasOn) {
            m_pvpAsked = false;
            GameEvent& e = event(GameEventKind::PvpToggled, nowMs);
            e.value[0] = on ? 1 : 0;
        } else if (nowMs - m_pvpAskedMs > kPvpAnswerMs) {
            m_pvpAsked = false;
        }
    }
}

bool PlayerCommands::handles(Opcode opcode) {
    switch (opcode) {
    case Opcode::MSG_RAID_TARGET_UPDATE:
    case Opcode::SMSG_PLAYED_TIME:
    case Opcode::SMSG_INSPECT:
    case Opcode::SMSG_PET_NAME_INVALID: return true;
    default: return false;
    }
}

bool PlayerCommands::handle(Opcode opcode, std::span<const u8> payload, u32 nowMs) {
    switch (opcode) {
    case Opcode::MSG_RAID_TARGET_UPDATE: {
        if (!net::parseRaidTargetUpdate(payload, m_update)) {
            return false;
        }
        if (m_update.full) {
            m_raidTargets = {};
        }
        for (u32 i = 0; i < m_update.count; ++i) {
            const net::RaidTargetUpdate::Entry& entry = m_update.entries[i];
            if (entry.guid != 0) { // one icon per unit (Group::SetTargetIcon moves it)
                for (u64& guid : m_raidTargets) {
                    guid = guid == entry.guid ? 0 : guid;
                }
            }
            m_raidTargets[entry.icon] = entry.guid;
        }
        ++m_raidTargetVersion;
        GameEvent& e = event(GameEventKind::RaidTargetChanged, nowMs);
        if (!m_update.full) {
            e.id = m_update.entries[0].icon + 1u;
            e.guid = m_update.entries[0].guid;
        }
        return true;
    }
    case Opcode::SMSG_PLAYED_TIME: {
        net::PlayedTime played;
        if (!net::parsePlayedTime(payload, played)) {
            return false;
        }
        GameEvent& e = event(GameEventKind::TimePlayed, nowMs);
        e.value[0] = static_cast<i32>(played.total);
        e.value[1] = static_cast<i32>(played.level);
        return true;
    }
    case Opcode::SMSG_INSPECT: {
        u64 guid = 0; // the 1.12 client reads the GUID and does nothing with it (WoW.exe 0x5e7d70)
        return net::parseGuid(payload, guid);
    }
    case Opcode::SMSG_PET_NAME_INVALID: event(GameEventKind::PetNameInvalid, nowMs); return true;
    default: return false;
    }
}

GameEvent& PlayerCommands::event(GameEventKind kind, u32 nowMs) {
    GameEvent& e = m_events.push();
    e = GameEvent{};
    e.kind = kind;
    e.timeMs = nowMs;
    return e;
}

bool PlayerCommands::sendBody(Opcode opcode) {
    return m_inWorld && m_sink(opcode, m_body);
}

u8 PlayerCommands::raidTarget(u64 guid) const {
    if (guid == 0) {
        return 0;
    }
    for (u32 i = 0; i < m_raidTargets.size(); ++i) {
        if (m_raidTargets[i] == guid) {
            return static_cast<u8>(i + 1);
        }
    }
    return 0;
}

u64 PlayerCommands::raidTargetUnit(u8 icon) const {
    return icon >= 1 && icon <= m_raidTargets.size() ? m_raidTargets[icon - 1u] : 0;
}

bool PlayerCommands::setRaidTarget(u64 guid, u8 icon) {
    if (guid == 0 || icon > net::kRaidTargetIcons || !m_social.group().active) {
        return false;
    }
    m_body.clear();
    if (icon == 0) {
        const u8 current = raidTarget(guid);
        if (current == 0) {
            return false;
        }
        net::writeRaidTargetSet(m_body, static_cast<u8>(current - 1), 0); // that icon's slot emptied
    } else {
        net::writeRaidTargetSet(m_body, static_cast<u8>(icon - 1), guid);
    }
    return sendBody(Opcode::MSG_RAID_TARGET_UPDATE);
}

bool PlayerCommands::requestRaidTargets() {
    m_body.clear();
    net::writeRaidTargetRequest(m_body);
    return sendBody(Opcode::MSG_RAID_TARGET_UPDATE);
}

bool PlayerCommands::inspect(u64 guid) {
    m_body.clear();
    net::writeGuid(m_body, guid);
    return guid != 0 && sendBody(Opcode::CMSG_INSPECT);
}

bool PlayerCommands::requestTimePlayed() {
    m_body.clear();
    return sendBody(Opcode::CMSG_PLAYED_TIME);
}

bool PlayerCommands::togglePvp(u32 nowMs) {
    m_body.clear();
    if (!sendBody(Opcode::CMSG_TOGGLE_PVP)) {
        return false;
    }
    const Entity* me = m_entities.find(m_player);
    m_pvpAsked = me != nullptr;
    m_pvpWasOn = me && (me->value(net::uf::PLAYER_FLAGS) & kPlayerFlagPvpDesired) != 0;
    m_pvpAskedMs = nowMs;
    return true;
}

bool PlayerCommands::guildSetLeader(std::string_view name) {
    m_body.clear();
    net::writeCString(m_body, name);
    return !name.empty() && sendBody(Opcode::CMSG_GUILD_LEADER);
}

bool PlayerCommands::guildDisband() {
    m_body.clear();
    return sendBody(Opcode::CMSG_GUILD_DISBAND);
}

bool PlayerCommands::guildInfo() {
    m_body.clear();
    return sendBody(Opcode::CMSG_GUILD_INFO);
}

bool PlayerCommands::readyCheck() {
    if (m_readyCheck) {
        return m_readyCheck->start(m_nowMs); // phase 16 (alerts): WoW.exe's DoReadyCheck
    }
    m_body.clear();
    return sendBody(Opcode::MSG_RAID_READY_CHECK);
}

bool PlayerCommands::setAssistant(u64 member, bool assistant) {
    m_body.clear();
    net::writeGroupAssistant(m_body, member, assistant);
    return member != 0 && sendBody(Opcode::CMSG_GROUP_ASSISTANT_LEADER);
}

bool PlayerCommands::renamePet(u64 pet, std::string_view name) {
    m_body.clear();
    net::writePetRename(m_body, pet, name);
    return pet != 0 && sendBody(Opcode::CMSG_PET_RENAME);
}

bool PlayerCommands::channelCommand(Opcode opcode, std::string_view channel, std::string_view argument) {
    if (channel.empty()) {
        return false;
    }
    m_body.clear();
    switch (opcode) {
    case Opcode::CMSG_CHANNEL_PASSWORD:
    case Opcode::CMSG_CHANNEL_SET_OWNER:
    case Opcode::CMSG_CHANNEL_MODERATOR:
    case Opcode::CMSG_CHANNEL_UNMODERATOR:
    case Opcode::CMSG_CHANNEL_MUTE:
    case Opcode::CMSG_CHANNEL_UNMUTE:
    case Opcode::CMSG_CHANNEL_INVITE:
    case Opcode::CMSG_CHANNEL_KICK:
    case Opcode::CMSG_CHANNEL_BAN:
    case Opcode::CMSG_CHANNEL_UNBAN: net::writeChannelCommand(m_body, channel, argument); break;
    case Opcode::CMSG_CHANNEL_OWNER:
    case Opcode::CMSG_CHANNEL_ANNOUNCEMENTS:
    case Opcode::CMSG_CHANNEL_MODERATE: net::writeCString(m_body, channel); break;
    default: return false;
    }
    return sendBody(opcode);
}

} // namespace vd::game
