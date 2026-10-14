#include "game/Minimap.h"

#include "core/Text.h"
#include "game/Gameplay.h"
#include "game/MinimapData.h"
#include "game/Reactions.h"
#include "game/UnitInfo.h"
#include "net/UpdateFields.gen.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace vd::game {
namespace {

namespace uf = net::uf;

// The party members a frame looks at (a raid's 40 at most).
constexpr u32 kMaxMembers = 40;

} // namespace

f32 minimapRadius(bool indoors, u32 zoom) {
    const u32 level = std::min(zoom, kMinimapZoomLevels - 1);
    return (indoors ? kMinimapIndoorYards[level] : kMinimapOutdoorYards[level]) * 0.5f;
}

u32 minimapZoomStep(u32 zoom, i32 steps) {
    const i32 level = static_cast<i32>(std::min(zoom, kMinimapZoomLevels - 1)) + steps;
    return static_cast<u32>(std::clamp(level, 0, static_cast<i32>(kMinimapZoomLevels) - 1));
}

MinimapPoint minimapPoint(f32 centerX, f32 centerY, f32 radiusYards, f32 worldX, f32 worldY, f32 rim) {
    MinimapPoint p;
    if (radiusYards <= 0.0f) {
        return p;
    }
    p.x = (centerY - worldY) / radiusYards; // world +Y is west: left
    p.y = (centerX - worldX) / radiusYards; // world +X is north: up
    p.angle = std::atan2(p.x, -p.y);
    const f32 length = std::sqrt(p.x * p.x + p.y * p.y);
    if (length > rim) {
        p.x *= rim / length;
        p.y *= rim / length;
        p.outside = true;
    }
    return p;
}

void minimapWorldPoint(f32 centerX, f32 centerY, f32 radiusYards, f32 offsetX, f32 offsetY, f32& worldX,
                       f32& worldY) {
    worldX = centerX - offsetY * radiusYards;
    worldY = centerY - offsetX * radiusYards;
}

u32 blipCell(BlipKind kind) {
    switch (kind) {
    case BlipKind::PartyMember: return 4;
    case BlipKind::QuestReward: return 0;
    case BlipKind::QuestRewardOld: return 1;
    case BlipKind::TrackedHostile: return 1;
    case BlipKind::TrackedNeutral: return 0;
    case BlipKind::TrackedFriendly: return 2;
    case BlipKind::TrackedResource: return 3;
    case BlipKind::Corpse: return kNoBlipCell;
    case BlipKind::GossipPoi: return kNoBlipCell; // its POIIcons cell
    }
    return kNoBlipCell;
}

bool blipOnRim(BlipKind kind) {
    return kind == BlipKind::PartyMember || kind == BlipKind::Corpse || kind == BlipKind::GossipPoi;
}

u32 collectMinimapBlips(const EntityStore& entities, const Gameplay& play, const MinimapData* data,
                        const Reactions* reactions, const BlipQuery& q, std::span<MinimapBlip> out) {
    u32 count = 0;
    const auto push = [&](BlipKind kind, u64 guid, f32 x, f32 y, bool dead) {
        if (count < out.size()) {
            out[count++] = MinimapBlip{kind, guid, x, y, dead};
        }
    };
    const f32 reach2 = q.radius * q.radius;
    const auto within = [&](f32 x, f32 y) {
        const f32 dx = x - q.x, dy = y - q.y;
        return dx * dx + dy * dy <= reach2;
    };

    // The corpse a ghost runs back to, on this map.
    const DeathState& death = play.death();
    if (death.ghost && death.corpseKnown && death.corpseMap >= 0 &&
        static_cast<u32>(death.corpseMap) == q.map) {
        push(BlipKind::Corpse, 0, death.corpseX, death.corpseY, false);
    }
    // Phase 13: a guard's directions (the gossip point), on the rim when beyond the map.
    if (const GossipPointState& poi = play.minimap().gossipPoint(); poi.active) {
        push(BlipKind::GossipPoi, 0, poi.x, poi.y, false);
    }
    // Party members (where the server last put those out of sight), on the rim when beyond the map. Phase 16: in a raid
    // the player's subgroup only (WoW.exe's blips 0x6DAD60 walk the four party slots; the rest of the raid is not on
    // the 1.12 minimap).
    std::array<PartyMemberView, kMaxMembers> members;
    const u32 memberCount = play.social().partyMembers(members);
    for (u32 i = 0; i < memberCount; ++i) {
        const PartyMemberView& m = members[i];
        if (m.guid != play.player() && m.online && m.hasPosition) {
            push(BlipKind::PartyMember, m.guid, m.x, m.y, m.dead);
        }
    }

    // What stands within the map: quest givers with a quest to turn in, tracked creatures and resources.
    const Entity* me = entities.find(play.player());
    const QueryCache& queries = play.queries();
    entities.forEach([&](const Entity& e) {
        if (count == out.size() || e.guid == play.player() || !e.hasPosition) {
            return;
        }
        const bool unit = isUnit(e);
        if (!unit && e.type != ObjectType::GameObject) {
            return;
        }
        const glm::vec3 at = e.positionAt(q.nowMs);
        if (!within(at.x, at.y)) {
            return;
        }
        if (isQuestGiver(e)) {
            const net::QuestGiverStatus status = play.questGiverStatus(e.guid);
            if (status == net::QuestGiverStatus::Reward) {
                push(BlipKind::QuestReward, e.guid, at.x, at.y, false);
                return;
            }
            if (status == net::QuestGiverStatus::RewardOld) {
                push(BlipKind::QuestRewardOld, e.guid, at.x, at.y, false);
                return;
            }
        }
        if (unit) {
            // Phase 16: the party is left out (WoW.exe 0x4EABD8: UnitInParty), a raid's other subgroups are tracked as
            // any humanoid.
            if (q.trackCreatures == 0 || isDead(e) ||
                (e.type == ObjectType::Player && play.social().inPartyWith(e.guid))) {
                return;
            }
            u32 type = kCreatureTypeHumanoid;
            if (e.type == ObjectType::Unit) {
                const net::CreatureQueryInfo* creature =
                    queries.findCreature(e.value(uf::OBJECT_FIELD_ENTRY));
                type = creature && creature->found ? creature->type : 0;
            }
            if (type == 0 || type > 32 || (q.trackCreatures & (1u << (type - 1))) == 0) {
                return;
            }
            const Reaction reaction = me ? reactionTo(*me, e, reactions) : Reaction::Neutral;
            push(reaction == Reaction::Hostile    ? BlipKind::TrackedHostile
                 : reaction == Reaction::Friendly ? BlipKind::TrackedFriendly
                                                  : BlipKind::TrackedNeutral,
                 e.guid, at.x, at.y, false);
            return;
        }
        // Herbs, mineral veins and treasure are chests whose lock opens with the tracked skill (data0: the
        // lock).
        const u32 goType = gameObjectType(e);
        if (q.trackResources == 0 || !data || (goType != kGameObjectChest && goType != kGameObjectGoober)) {
            return;
        }
        const GameObjectInfo* info = queries.findGameObject(e.value(uf::OBJECT_FIELD_ENTRY));
        if (info && info->found && (data->lockTypeMask(info->data[0]) & q.trackResources) != 0) {
            push(BlipKind::TrackedResource, e.guid, at.x, at.y, false);
        }
    });
    return count;
}

std::string_view minimapBlipName(const EntityStore& entities, const Gameplay& play, const MinimapBlip& blip) {
    if (blip.kind == BlipKind::Corpse) {
        return text::get("CORPSE");
    }
    if (blip.kind == BlipKind::GossipPoi) {
        return play.minimap().gossipPoint().name;
    }
    const QueryCache& queries = play.queries();
    if (const std::string* name = queries.findPlayerName(blip.guid)) {
        return *name;
    }
    if (blip.kind == BlipKind::PartyMember) {
        std::array<PartyMemberView, kMaxMembers> members;
        const u32 count = play.social().partyMembers(members);
        for (u32 i = 0; i < count; ++i) {
            if (members[i].guid == blip.guid) {
                return members[i].name; // a view into Social's own storage
            }
        }
        return {};
    }
    const Entity* e = entities.find(blip.guid);
    if (!e) {
        return {};
    }
    if (e->type == ObjectType::Unit) {
        const net::CreatureQueryInfo* creature = queries.findCreature(e->value(uf::OBJECT_FIELD_ENTRY));
        return creature && creature->found ? std::string_view(creature->name) : std::string_view{};
    }
    if (e->type == ObjectType::GameObject) {
        const GameObjectInfo* info = queries.findGameObject(e->value(uf::OBJECT_FIELD_ENTRY));
        return info && info->found ? std::string_view(info->name) : std::string_view{};
    }
    return {};
}

u32 activeTrackingSpell(const Gameplay& play, u64 player, const MinimapData& data) {
    std::array<AuraInfo, 64> auras;
    const u32 count = play.auras(player, auras);
    for (u32 i = 0; i < count; ++i) {
        if (data.isTrackingSpell(auras[i].spellId)) {
            return auras[i].spellId;
        }
    }
    return 0;
}

bool gameTimeNight(u32 minutesOfDay) {
    const u32 minutes = minutesOfDay % (24 * 60);
    return minutes < kGameTimeDawn || minutes >= kGameTimeDusk;
}

std::string_view gameTimeText(std::span<char> out, u32 minutesOfDay, bool twentyFourHours) {
    const u32 minutes = minutesOfDay % (24 * 60);
    const i32 hour = static_cast<i32>(minutes / 60), minute = static_cast<i32>(minutes % 60);
    if (twentyFourHours) {
        return text::format(out, "TIME_TWENTYFOURHOURS", hour, minute);
    }
    const i32 shown = hour % 12 == 0 ? 12 : hour % 12;
    return text::format(out, hour >= 12 ? "TIME_TWELVEHOURPM" : "TIME_TWELVEHOURAM", shown, minute);
}

f32 minimapPingAlpha(const MinimapPingState& ping, u32 nowMs) {
    if (!ping.active) {
        return 0.0f;
    }
    const i32 age = static_cast<i32>(nowMs - ping.startMs);
    if (age < 0 || age >= static_cast<i32>(kMinimapPingMs + kMinimapPingFadeMs)) {
        return 0.0f;
    }
    // Minimap_OnUpdate's fade: SetAlpha(255 * fadeOutTimer / 0.5), clamped to 1 by Frame:SetAlpha.
    const f32 fadeLeft = static_cast<f32>(static_cast<i32>(kMinimapPingMs + kMinimapPingFadeMs) - age) /
                         static_cast<f32>(kMinimapPingFadeMs);
    return std::min(1.0f, 255.0f * fadeLeft);
}

// --- Minimap -----------------------------------------------------------------------------------------------

void Minimap::enterWorld(u64 player) {
    m_player = player;
    m_ping.active = false; // the serial keeps counting: no sound for what was
    m_gossipPoint.active = false;
}

void Minimap::leaveWorld() {
    m_player = 0;
    m_ping.active = false;
    m_gossipPoint.active = false;
}

bool Minimap::handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs) {
    if (opcode == net::Opcode::SMSG_GOSSIP_POI) { // phase 13
        if (!net::parseGossipPoi(payload, m_poi)) {
            return false;
        }
        m_gossipPoint.active = true;
        m_gossipPoint.serial = m_gossipPoint.serial + 1 == 0 ? 1 : m_gossipPoint.serial + 1;
        m_gossipPoint.x = m_poi.x;
        m_gossipPoint.y = m_poi.y;
        m_gossipPoint.icon = m_poi.icon;
        m_gossipPoint.name.assign(m_poi.name);
        return true;
    }
    if (opcode != net::Opcode::MSG_MINIMAP_PING) {
        return true;
    }
    net::MinimapPing ping;
    if (!net::parseMinimapPing(payload, ping)) {
        return false;
    }
    m_ping.serial = m_ping.serial + 1 == 0 ? 1 : m_ping.serial + 1;
    m_ping.active = true;
    m_ping.sender = ping.sender;
    m_ping.x = ping.x;
    m_ping.y = ping.y;
    m_ping.startMs = nowMs;
    return true;
}

bool Minimap::ping(f32 x, f32 y, bool grouped, u32 nowMs) {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }
    m_ping.serial = m_ping.serial + 1 == 0 ? 1 : m_ping.serial + 1;
    m_ping.active = true;
    m_ping.sender = m_player;
    m_ping.x = x;
    m_ping.y = y;
    m_ping.startMs = nowMs;
    if (!grouped || m_player == 0) {
        return false;
    }
    m_body.clear();
    net::writeMinimapPing(m_body, x, y);
    return m_sink(net::Opcode::MSG_MINIMAP_PING, m_body);
}

} // namespace vd::game
