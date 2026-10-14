#pragma once

#include "core/Types.h"
#include "game/Entities.h"
#include "game/EventRing.h"
#include "game/GameEvents.h"
#include "net/Opcodes.gen.h"

#include <span>
#include <string_view>

namespace vd::game {

class ChatLog;
class Reactions;
class Social;
class Travel;
struct ChatChannel;

// How a SMSG_PVP_CREDIT reads (WoW.exe 0x625270): a HonorGained event's value[1].
enum class HonorCredit : u8 {
    Awarded,      // no victim: COMBATLOG_HONORAWARD "You have been awarded %d honor points." (bonus honor)
    Honorable,    // honor > 0: COMBATLOG_HONORGAIN with the victim's name, its rank title and the honor
    Dishonorable, // honor <= 0: COMBATLOG_DISHONORGAIN (its rank title: PVP_RANK_CIVILIAN)
};

// The defense channels (ChatChannels.dbc rows with flag 0x10000, which WoW.exe tests): LocalDefense
// ("LocalDefense - %s", flags 0x10003: zone dependent, joined in every zone) and WorldDefense (flags 0x10004,
// joined by hand).
constexpr u32 kChannelLocalDefense = 22;
constexpr u32 kChannelWorldDefense = 23;
// Which defense channel this is: its ChatChannels kind (the zone channels the client joined), else its name
// (one joined by hand); 0 for any other channel.
u32 defenseChannelKind(const ChatChannel& channel);
u32 defenseChannelKind(std::string_view name);

// The lines and texts of a HonorGained event, as WoW.exe words them (0x625270, 0x6072E0). `playerFemale`: the
// rank title takes the PLAYER's gender, not the victim's (0x612BF0 is asked of the player object): "_FEMALE"
// for a female player. The rank title: PVP_RANK_<rank>_<side> (side 0 Horde, 1 Alliance: the victim's
// FactionTemplate group), or PVP_RANK_CIVILIAN for a dishonorable kill; empty for an award.
std::string_view honorRankTitle(std::span<char> keyBuffer, const GameEvent& credit, bool playerFemale);
// CHAT_MSG_COMBAT_HONOR_GAIN's line: COMBATLOG_HONORGAIN (name, rank title, honor), COMBATLOG_DISHONORGAIN
// (name), COMBATLOG_HONORAWARD (honor).
std::string_view formatHonorLine(std::span<char> out, const GameEvent& credit, std::string_view victimName,
                                 bool playerFemale);
// The text that rises over the player's head for a kill (0x6072E0: "%s: %s" with HK or DK and the rank title,
// in COMBAT_HONOR_GAIN's colour); empty for an award.
std::string_view formatHonorWorldText(std::span<char> out, const GameEvent& credit, bool playerFemale);

// Phase 17 (honor): honor and the world's PvP messages as WoW.exe 1.12.1 takes them (read-only disassembly;
// vMaNGOS HonorMgr.cpp, Objects/Creature.cpp, Maps/Map.cpp, Objects/Player.cpp RemovedInsignia):
//  - SMSG_PVP_CREDIT (0x48F6C9 -> 0x625270): without a victim, an award. With one, nothing at all unless the
//    client still has that unit; honor > 0 is an honorable kill whose victim's FactionTemplate must be on a
//    side (Horde 0, Alliance 1; else nothing), honor <= 0 a dishonorable one. Then the
//    CHAT_MSG_COMBAT_HONOR_GAIN line, the floating combat text (COMBAT_TEXT_UPDATE "HONOR_GAINED", the honor)
//    and, for kills, "HK: <rank>" over the player. One HonorGained event carries it (guid = victim, id =
//    rank, value[0] = honor, value[1] = HonorCredit, value[2] = side).
//  - SMSG_PLAYER_SKINNED (0x5E7D40): PLAYER_SKINNED. UIParent hides the RESURRECT popups and writes
//    DEATH_CORPSE_SKINNED in the UIErrorsFrame (red); its SKINNED / SKINNED_REPOP popups are commented out in
//    1.12.1.
//  - SMSG_ZONE_UNDER_ATTACK (0x49DCC0): nothing for an area AreaTable lacks; else ZONE_UNDER_ATTACK with the
//    area's name, added as a CHANNEL message of every joined defense channel: LocalDefense only while the
//    player stands in the area's zone (its parent, or itself), WorldDefense always. ChatFrame shows it as
//    "[2. LocalDefense] : ...".
//  - SMSG_DEFENSE_MESSAGE (0x49DE30): the server's text the same way (the zone: its parent when AreaTable
//    knows it).
// Main thread; nothing allocates per frame (a message's chat line is built once, when it comes).
class Honor {
public:
    using Events = EventRing<GameEvent, 64>;

    Honor(const EntityStore& entities, const Travel& travel, const Social& social, ChatLog& chat,
          Events& events);

    void enterWorld(u64 player);
    void leaveWorld();
    static bool handles(net::Opcode opcode);
    // `reactions`: the victims' sides (FactionTemplate groups); null: no honorable kill line. False when
    // malformed.
    bool handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs, const Reactions* reactions);

    // How many defense channel lines were added (tests, the trace).
    u32 defenseLines() const { return m_defenseLines; }

private:
    void credit(std::span<const u8> payload, u32 nowMs, const Reactions* reactions, bool& ok);
    // A defense message about `zoneId` (already its zone) in every joined defense channel that takes it.
    void defenseLine(u32 zoneId, std::string_view text, u32 nowMs);
    GameEvent& event(GameEventKind kind, u32 nowMs);

    const EntityStore& m_entities;
    const Travel& m_travel;
    const Social& m_social;
    ChatLog& m_chat;
    Events& m_events;
    u64 m_player = 0;
    u32 m_defenseLines = 0;
};

} // namespace vd::game
