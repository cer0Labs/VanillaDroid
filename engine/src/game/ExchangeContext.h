#pragma once

#include "core/Types.h"
#include "game/Entities.h"
#include "game/EventRing.h"
#include "game/GameEvents.h"
#include "game/Queries.h"
#include "net/Opcodes.gen.h"

#include <vector>

namespace vd::game {

class Inventory;

// UNIT_NPC_FLAG_AUCTIONEER (Objects/UnitDefines.h) and GAMEOBJECT_TYPE_MAILBOX (Objects/GameObjectDefines.h).
constexpr u32 kNpcFlagAuctioneer = 0x1000;
constexpr u32 kGameObjectMailbox = 19;

// What the parts of game::Exchange share (phase 8): the session's stores, the event ring the HUD reads, the
// packet sink and the outgoing scratch. Exchange owns it; its parts keep a reference.
struct ExchangeContext {
    using Events = EventRing<GameEvent, 64>;

    ExchangeContext(const EntityStore& entityStore, QueryCache& queryCache, Events& eventRing,
                    Inventory& items)
        : entities(entityStore), queries(queryCache), events(eventRing), inventory(items) {
        body.reserve(640);
    }

    const EntityStore& entities;
    QueryCache& queries;
    Events& events;
    Inventory& inventory;
    PacketSink sink;
    u64 player = 0;
    bool inWorld = false;
    u32 nowMs = 0;
    std::vector<u8> body; // outgoing scratch

    const Entity* me() const { return entities.find(player); }
    // `body` as the packet's body; false when not sent (not in the world).
    bool send(net::Opcode opcode) { return inWorld && sink(opcode, body); }
    bool sendEmpty(net::Opcode opcode) {
        body.clear();
        return send(opcode);
    }
    GameEvent& event(GameEventKind kind, u32 id = 0, u64 guid = 0) {
        GameEvent& e = events.push();
        e = GameEvent{};
        e.kind = kind;
        e.timeMs = nowMs;
        e.id = id;
        e.guid = guid;
        return e;
    }
};

} // namespace vd::game
