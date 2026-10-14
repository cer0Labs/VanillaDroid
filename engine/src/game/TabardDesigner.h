#pragma once

#include "core/Types.h"
#include "game/Entities.h"
#include "game/GameEvents.h"
#include "game/GuildEmblem.h"
#include "game/GuildNotice.h"
#include "net/Opcodes.gen.h"

#include <span>
#include <vector>

namespace vd::game {

class Social;

// Phase 15 (guild): the tabard designer (TabardFrame.lua and its TabardModel's C side, WoW.exe
// 0x502780..0x503990): MSG_TABARDVENDOR_ACTIVATE opens it (OPEN_TABARD_FRAME) with the guild's design
// (InitializeTabardColors: the guild's emblem when it has one, a random one otherwise), the five arrows cycle
// their choice (CycleVariation), Accept saves it (TabardModel:Save: the client's checks — a guild master, a
// different design, 10 gold — then MSG_SAVE_GUILD_EMBLEM) and the server's answer is an ERR_GUILDEMBLEM_*
// line (notices()). The guild's emblem comes from game::Social (SMSG_GUILD_QUERY_RESPONSE). Packets as
// vmangos-core 4641790 handles them. Main thread.
class TabardDesigner {
public:
    TabardDesigner(const EntityStore& entities, const Social& social);

    void setSink(PacketSink sink) { m_sink = sink; }
    void enterWorld(u64 player, u32 nowMs);
    void leaveWorld();
    static bool handles(net::Opcode opcode);
    bool handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs); // false when malformed
    const GuildNotices& notices() const { return m_notices; }

    bool open() const { return m_open; }
    u64 vendor() const { return m_vendor; }
    // The design shown (always designed() while open).
    const GuildEmblem& design() const { return m_design; }
    // CycleVariation(id, direction): choice `kind` (0 emblem, 1 its colour, 2 border, 3 its colour, 4
    // background) one step back or on, round its count.
    void cycle(u32 kind, i32 direction);
    // TabardFrame_UpdateButtons: a guild master (TABARDVENDORGREETING; TABARDVENDORNOGUILDGREETING
    // otherwise).
    bool guildMaster() const;
    // CanSaveTabardNow: the guild is known and no save waits for its answer (with guildMaster(): Accept
    // enabled).
    bool canSaveNow() const;
    bool save(u32 nowMs); // TabardModel:Save
    void close();         // CloseTabardCreation (CLOSE_TABARD_FRAME)
    // Development / tests: the random seed of InitializeTabardColors.
    void setSeed(u32 seed) { m_seed = seed; }

private:
    void initialize(); // InitializeTabardColors
    u32 random(u32 count);
    GuildNotice& notice(const char* key);

    const EntityStore& m_entities;
    const Social& m_social;
    PacketSink m_sink;
    std::vector<u8> m_body;
    bool m_inWorld = false;
    u64 m_player = 0;
    u32 m_nowMs = 0;
    bool m_open = false;
    u64 m_vendor = 0;
    GuildEmblem m_design;
    bool m_pending = false; // a save waits for MSG_SAVE_GUILD_EMBLEM (WoW.exe's 0xC4D780)
    u32 m_seed = 0x7AB0D5u;
    GuildNotices m_notices;
};

} // namespace vd::game
