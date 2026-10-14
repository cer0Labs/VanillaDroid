#pragma once

#include "core/Types.h"
#include "game/Entities.h"
#include "game/EventRing.h"
#include "game/GameEvents.h"
#include "net/Opcodes.gen.h"
#include "net/PlayerCommandMessages.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace vd::game {

class ReadyCheck;
class Social;

// PLAYER_FLAGS_PVP_DESIRED (Objects/Player.h): the player's wish for PvP, what /pvp toggles.
constexpr u32 kPlayerFlagPvpDesired = 0x200;

// The game side of the player's menus and the slash commands of phase 13 (UnitPopup.lua, ChatFrame.lua's
// SlashCmdList and the C functions behind them) that no other area owns: the group's raid target icons
// (MSG_RAID_TARGET_UPDATE: SetRaidTarget / GetRaidTargetIndex), inspecting a player (NotifyInspect), the time
// played (RequestTimePlayed), the PvP wish (TogglePVP), the guild's leadership and disbanding (GuildSetLeaderByName,
// GuildDisband), the raid's ready check and assistants (DoReadyCheck, PromoteToAssistant / DemoteAssistant),
// renaming a hunter's pet (PetRename) and the chat channels' moderation (SetChannelPassword, ChannelModerator...).
// Packets as vmangos-core 4641790 handles them (net/PlayerCommandMessages). Main thread; fixed storage.
//
// Events (Gameplay::events()): TimePlayed, PetNameInvalid, PvpToggled, RaidTargetChanged.
class PlayerCommands {
public:
    using Events = EventRing<GameEvent, 64>;

    PlayerCommands(const EntityStore& entities, const Social& social, Events& events);

    void setSink(PacketSink sink) { m_sink = sink; }
    // Phase 16 (alerts): where DoReadyCheck goes (game::ReadyCheck: only the leader, not twice in 30 s, the answers).
    void setReadyCheck(ReadyCheck* readyCheck) { m_readyCheck = readyCheck; }
    // `newLogin`: another character (its icons are forgotten); false for a new map of the same one.
    void enterWorld(u64 player, bool newLogin, u32 nowMs);
    void leaveWorld();
    // Once per frame: the icons go when the player leaves its group, and are asked for when it is in one (the
    // server only sends a party's list after its group lists; a raid's must be asked for).
    void update(u32 nowMs);
    // MSG_RAID_TARGET_UPDATE, SMSG_PLAYED_TIME, SMSG_INSPECT, SMSG_PET_NAME_INVALID. False when malformed.
    static bool handles(net::Opcode opcode);
    bool handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs);

    // --- Raid target icons ---
    // GetRaidTargetIndex: the icon on a unit (1..8), 0 none.
    u8 raidTarget(u64 guid) const;
    // The unit wearing an icon (1..8), 0 none.
    u64 raidTargetUnit(u8 icon) const;
    // SetRaidTarget(unit, icon): 1..8 puts it on, 0 takes the unit's icon off. The server obeys the group's
    // leader and assistants only and tells the whole group (RaidTargetChanged). False when not sent (no group,
    // not allowed, nothing to take off).
    bool setRaidTarget(u64 guid, u8 icon);
    // The group's whole list (MSG_RAID_TARGET_UPDATE 0xFF).
    bool requestRaidTargets();
    // Bumps with every change of the icons (the frames and the world redraw them).
    u32 raidTargetVersion() const { return m_raidTargetVersion; }

    // --- The rest ---
    bool inspect(u64 guid);                     // NotifyInspect: CMSG_INSPECT (the server checks 10 yards)
    bool requestTimePlayed();                   // RequestTimePlayed: CMSG_PLAYED_TIME -> TimePlayed
    bool togglePvp(u32 nowMs);                  // TogglePVP: CMSG_TOGGLE_PVP (no state byte) -> PvpToggled
    bool guildSetLeader(std::string_view name); // GuildSetLeaderByName: CMSG_GUILD_LEADER
    bool guildDisband();                        // GuildDisband: CMSG_GUILD_DISBAND
    bool guildInfo();                           // GuildInfo: CMSG_GUILD_INFO (game::Social's GuildInfo event)
    bool readyCheck();                          // DoReadyCheck: MSG_RAID_READY_CHECK (the leader's request)
                                                // (phase 16: ReadyCheck::start when set)
    bool setAssistant(u64 member, bool assistant); // PromoteToAssistant / DemoteAssistant
    bool renamePet(u64 pet, std::string_view name); // PetRename: CMSG_PET_RENAME (refused: PetNameInvalid)
    // The channels' moderation: CMSG_CHANNEL_SET_OWNER, _MODERATOR, _UNMODERATOR, _MUTE, _UNMUTE, _INVITE,
    // _KICK, _BAN, _UNBAN and _PASSWORD take the channel and an argument (a player, the password);
    // CMSG_CHANNEL_OWNER, _ANNOUNCEMENTS and _MODERATE the channel alone. The server answers each with an
    // SMSG_CHANNEL_NOTIFY (game::Social's ChannelNotice events). False for any other opcode.
    bool channelCommand(net::Opcode opcode, std::string_view channel, std::string_view argument = {});

private:
    bool sendBody(net::Opcode opcode);
    GameEvent& event(GameEventKind kind, u32 nowMs);
    void clearRaidTargets(u32 nowMs);

    const EntityStore& m_entities;
    const Social& m_social;
    Events& m_events;
    PacketSink m_sink;
    std::vector<u8> m_body;
    bool m_inWorld = false;
    u64 m_player = 0;

    std::array<u64, net::kRaidTargetIcons> m_raidTargets{}; // by icon - 1
    u32 m_raidTargetVersion = 0;
    bool m_wasInGroup = false;
    bool m_raidTargetsAsked = false;

    // /pvp: the wish asked for, until PLAYER_FLAGS says it changed (or a while passed).
    bool m_pvpAsked = false;
    bool m_pvpWasOn = false;
    u32 m_pvpAskedMs = 0;

    net::RaidTargetUpdate m_update; // parsed, reused
    ReadyCheck* m_readyCheck = nullptr; // phase 16 (alerts)
    u32 m_nowMs = 0;                    // the last update()'s
};

} // namespace vd::game
