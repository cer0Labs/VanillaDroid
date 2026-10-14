#pragma once

#include "core/Types.h"
#include "game/EventRing.h"
#include "game/GameEvents.h"
#include "game/Social.h"
#include "net/Opcodes.gen.h"
#include "net/PlayerCommandMessages.h"

#include <array>
#include <span>
#include <vector>

namespace vd::game {

// The client's own lines of a ready check (ReadyCheckNotice events), worded by game::NoticeFeed.
enum class ReadyCheckLine : u8 {
    Start,         // READY_CHECK_START: the leader's DoReadyCheck went out (system)
    InProgress,    // ERR_READY_CHECK_IN_PROGRESS: the leader asked again within the 30 s (UIErrorsFrame, red)
    LeaderStarted, // ERR_RAID_LEADER_READY_CHECK_START_S: a member is asked; guid = the leader (system)
    NotReady,      // RAID_MEMBER_NOT_READY: a member answered No; guid = the member (system, the leader's)
    MembersAfk, // RAID_MEMBERS_AFK: the check ended, ReadyCheck::afk() never answered (system, the leader's)
    NoAfk,      // READY_CHECK_NO_AFK: ...everyone did
    YouWereAfk, // READY_CHECK_YOU_WERE_AFK: the member's ReadyCheckFrame timed out unanswered (system)
};

// Phase 16 (alerts): the raid's ready check as WoW.exe 1.12.1 runs it (read-only disassembly):
//  - DoReadyCheck (0x4BB1D0): only the group's leader (assistants never: vMaNGOS would accept them); within
//  the 30 s
//    of the last one the UIErrorsFrame says ERR_READY_CHECK_IN_PROGRESS (error 455); otherwise every other
//    raid member is marked as not answered yet, the empty MSG_RAID_READY_CHECK goes out, the 30 s start
//    (0x7530) and the chat says READY_CHECK_START.
//  - MSG_RAID_READY_CHECK (0x4BA360). To a member (not the leader): ERR_RAID_LEADER_READY_CHECK_START_S with
//  the
//    leader's name (error 454, a system line), its own 30 s, and READY_CHECK (event 0x218): Blizzard_RaidUI's
//    ShowReadyCheck opens ReadyCheckFrame (the leader's portrait, READY_CHECK_MESSAGE, Yes / No, PlaySound
//    "ReadyCheck"; after 30 s unanswered READY_CHECK_YOU_WERE_AFK). The leader's client gets its own request
//    back (vMaNGOS broadcasts it to the whole group): no frame. To the leader, an answer (GUID + state): that
//    member has answered; a "no" prints RAID_MEMBER_NOT_READY at once; when no ONLINE member is left
//    unanswered the check ends at once instead of waiting.
//  - CheckReadyCheckTime (0x4BB310, UIParent's OnUpdate): for the leader, when the 30 s are over,
//  RAID_MEMBERS_AFK
//    with the names of the members that never answered (", " between them, roster order; offline ones too),
//    or READY_CHECK_NO_AFK.
//  - ConfirmReadyCheck (0x4BC090): only while the check runs (the member's 30 s): u8 1 for Yes; the frame's
//  No calls
//    it without an argument, which sends 0.
// The member's ReadyCheckFrame lives here too (its leader and timer: prompt*) so the rules are tested without
// the interface; it goes, unanswered and silently, when the player is no longer in a raid (its
// RAID_ROSTER_UPDATE). Party bots never answer (vMaNGOS PartyBotAI ignores the packet). Main thread; fixed
// storage after construction.
//
// Events (Gameplay::events()): ReadyCheck (guid = the leader: show the frame), ReadyCheckNotice (value[0] =
// ReadyCheckLine, guid = whom it names, value[1] = how many in afk()).
class ReadyCheck {
public:
    using Events = EventRing<GameEvent, 64>;
    static constexpr u32 kDurationMs =
        30000; // DoReadyCheck's and the request's 0x7530; ReadyCheckFrame.timer = 30
    static constexpr u32 kMaxMembers = 40; // MAX_RAID_MEMBERS

    ReadyCheck(const Social& social, Events& events);

    void setSink(PacketSink sink) { m_sink = sink; }
    void enterWorld(u64 player, bool newLogin, u32 nowMs);
    void leaveWorld();
    // Once a frame: CheckReadyCheckTime, ReadyCheck_OnUpdate and the frame's RAID_ROSTER_UPDATE.
    void update(u32 nowMs);
    static bool handles(net::Opcode opcode) { return opcode == net::Opcode::MSG_RAID_READY_CHECK; }
    // MSG_RAID_READY_CHECK. False when malformed.
    bool handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs);

    // DoReadyCheck: false when not sent (not the leader, or one runs: InProgress).
    bool start(u32 nowMs);
    // ConfirmReadyCheck(ready) from the member's frame, which closes with it. False when nothing was sent
    // (the check is over).
    bool answer(bool ready, u32 nowMs);

    // The check runs (0x4BB2F0: its 30 s are not over), for the leader or a member.
    bool inProgress(u32 nowMs) const;
    // ReadyCheckFrame: shown, the leader who asked (0 if the group named none) and what is left of its 30 s.
    bool prompting() const { return m_prompt; }
    u64 promptLeader() const { return m_promptLeader; }
    u32 promptRemainingMs(u32 nowMs) const;
    // The members of the last check that never answered (RAID_MEMBERS_AFK), in roster order.
    std::span<const u64> afk() const { return {m_afk.data(), m_afkCount}; }
    // The members the leader still waits for (tests, the raid tab).
    bool waitingFor(u64 member) const;

    // Development (VD_READYCHECK_DEMO, the gallery): the member's frame as if `leader` had asked, without a
    // server.
    void demoPrompt(u64 leader, u32 nowMs);
    // Development (VD_READYCHECK_SELF): the leader's own request, which the server sends back, also opens its
    // ReadyCheckFrame (1.12 never does): one player tests the frame, its answer and RAID_MEMBER_NOT_READY
    // online.
    void setSelfPrompt(bool on) { m_selfPrompt = on; }

private:
    bool isLeader() const { return m_social.isLeader(); }
    void notice(ReadyCheckLine line, u64 guid, u32 nowMs);
    void showPrompt(u64 leader, u32 nowMs); // READY_CHECK
    // Ends the leader's check (CheckReadyCheckTime): who never answered, then RAID_MEMBERS_AFK or
    // READY_CHECK_NO_AFK.
    void finish(u32 nowMs);
    // Some ONLINE member is still unanswered (the request handler's test before ending at once).
    bool waitingForOnline() const;
    void forget(u64 member);

    const Social& m_social;
    Events& m_events;
    PacketSink m_sink;
    std::vector<u8> m_body;
    bool m_inWorld = false;
    u64 m_player = 0;

    u32 m_endMs = 0; // 0xB713F4: when the check's 30 s are over (0: none)
    std::array<u64, kMaxMembers>
        m_pending{}; // the leader's members not answered yet (the roster's +0x158 flags)
    u32 m_pendingCount = 0;
    std::array<u64, kMaxMembers> m_afk{};
    u32 m_afkCount = 0;
    std::array<PartyMemberView, kMaxMembers>
        m_roster{}; // scratch: GetRaidRosterInfo when a check starts or ends
    // The member's ReadyCheckFrame.
    bool m_prompt = false;
    u64 m_promptLeader = 0;
    u32 m_promptEndMs = 0;
    bool m_demo = false; // demoPrompt: shown without a raid
    bool m_selfPrompt = false;
};

} // namespace vd::game
