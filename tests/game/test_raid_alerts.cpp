// Phase 16 (alerts): the ready check (MSG_RAID_READY_CHECK, game::ReadyCheck) and the meeting stones
// (CMSG/SMSG_MEETINGSTONE_*, game::MeetingStone) byte for byte as vmangos-core writes and reads them
// (Server/Packets/Group.cpp RaidReadyCheck*, Server/Packets/Misc.cpp Meetingstone*), truncated packets
// rejected; the rules WoW.exe 1.12.1 runs (DoReadyCheck 0x4BB1D0, the handler 0x4BA360, CheckReadyCheckTime
// 0x4BB310, ConfirmReadyCheck 0x4BC090; MeetingStoneInfo.cpp 0x4C9F40..0x4CA5B0, the stone's use 0x5F69D0)
// through game::Gameplay with synthetic packets; and the client's own lines they print (game/RaidAlertLines
// through game::NoticeFeed), worded in English.
#include "TextTable.h"
#include "core/Text.h"
#include "game/CombatText.h"
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/MeetingStone.h"
#include "game/Notices.h"
#include "game/ReadyCheck.h"
#include "net/ByteBuffer.h"
#include "net/MeetingStoneMessages.h"
#include "net/Messages.h"
#include "net/PlayerCommandMessages.h"
#include "net/SocialMessages.h"
#include "net/UpdateFields.gen.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <ostream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace vd;
using net::Opcode;
namespace uf = net::uf;
using game::GameEventKind;
using game::MeetingStoneLine;
using game::NoticeKind;
using game::ReadyCheckLine;
using Bytes = std::vector<u8>;
using Fields = std::vector<std::pair<u16, u32>>;

constexpr u64 kMe = 0x11;
constexpr u64 kAeryn = 0x21;
constexpr u64 kBrakk = 0x22;
constexpr u64 kTolvir = 0x23; // offline in the raids below
constexpr u64 kMirelle = 0x24;
constexpr u64 kStone = 0xF110000000004A21ull; // a meeting stone (GameObject GUID)
constexpr u32 kStoneEntry = 179584;
constexpr u32 kWailingCaverns = 718;

template <typename F> void checkTruncations(const Bytes& payload, F&& parse) {
    for (usize n = 0; n < payload.size(); ++n) {
        CHECK_FALSE(parse(std::span<const u8>(payload.data(), n)));
    }
}

void writeValues(net::ByteWriter& w, Fields fields) {
    std::sort(fields.begin(), fields.end());
    u32 maxIndex = 0;
    for (const auto& [index, value] : fields) {
        maxIndex = std::max<u32>(maxIndex, index);
    }
    std::vector<u32> mask(maxIndex / 32 + 1, 0);
    for (const auto& [index, value] : fields) {
        mask[index / 32] |= 1u << (index % 32);
    }
    w.u8v(static_cast<u8>(mask.size()));
    for (const u32 m : mask) {
        w.u32v(m);
    }
    for (const auto& [index, value] : fields) {
        w.u32v(value);
    }
}

Bytes values(u64 guid, const Fields& fields) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(0).packedGuid(guid);
    writeValues(w, fields);
    return out;
}

Bytes created(u64 guid, game::ObjectType type, const Fields& fields) {
    Bytes out;
    net::ByteWriter w(out);
    w.u32v(1).u8v(0).u8v(2).packedGuid(guid).u8v(static_cast<u8>(type)).u8v(0x40);
    w.f32v(0.0f).f32v(0.0f).f32v(0.0f).f32v(0.0f);
    writeValues(w, fields);
    return out;
}

// SMSG_GROUP_LIST: (name, GUID, status) members; the player's subgroup 0 (raid) or a party.
Bytes groupList(u64 leader, std::vector<std::tuple<const char*, u64, u8>> members, bool raid) {
    Bytes out;
    net::ByteWriter w(out);
    w.u8v(raid ? net::kGroupTypeRaid : 0).u8v(0).u32v(static_cast<u32>(members.size()));
    for (const auto& [name, guid, status] : members) {
        w.cstring(name).u64v(guid).u8v(status).u8v(0);
    }
    w.u64v(leader);
    if (!members.empty()) {
        w.u8v(3).u64v(0).u8v(2).u8v(0);
    }
    return out;
}

Bytes answerBody(u64 member, u8 state) {
    Bytes out;
    net::ByteWriter(out).u64v(member).u8v(state);
    return out;
}

Bytes setQueue(u32 area, u8 status) {
    Bytes out;
    net::ByteWriter(out).u32v(area).u8v(status);
    return out;
}

struct Sent {
    Opcode opcode;
    Bytes body;
};

// Stub texts for the lines' keys, with the formats of GlobalStrings.lua 1.12.1 (the game shows the client's own).
const std::vector<std::pair<std::string, std::string>> kEnglish = {
    {"READY_CHECK_START", "Starting ready check..."},
    {"READY_CHECK_NO_AFK", "No players are AFK"},
    {"READY_CHECK_YOU_WERE_AFK", "Missed a ready check"},
    {"RAID_MEMBERS_AFK", "AFK: %s"},
    {"RAID_MEMBER_NOT_READY", "%s is not ready"},
    {"ERR_RAID_LEADER_READY_CHECK_START_S", "%s: ready check"},
    {"ERR_READY_CHECK_IN_PROGRESS", "Ready check running"},
    {"ERR_MEETING_STONE_IN_QUEUE_S", "Stone queue for %s."},
    {"ERR_MEETING_STONE_LEFT_QUEUE_S", "Left stone queue for %s."},
    {"ERR_MEETING_STONE_MEMBER_ADDED_S", "Stone added %s."},
    {"ERR_MEETING_STONE_SUCCESS", "Stone group complete."},
    {"ERR_MEETING_STONE_IN_PROGRESS", "Stone still seeking."},
    {"ERR_MEETING_STONE_MUST_BE_LEADER", "Stone: leader only"},
    {"ERR_MEETING_STONE_GROUP_FULL", "Stone: group full"},
    {"ERR_MEETING_STONE_INVALID_LEVEL", "Stone: wrong level."},
    {"ERR_MEETING_STONE_NO_RAID_GROUP", "Stone: no raids"},
    {"ERR_MEETING_STONE_NOT_LEADER", "Stone: leader leaves"},
    {"UNKNOWN", "Unknown"},
};

struct Line {
    NoticeKind kind;
    std::string text;
    bool operator==(const Line&) const = default;
};
std::ostream& operator<<(std::ostream& os, const Line& l) {
    return os << static_cast<u32>(l.kind) << ": " << l.text;
}

// Gameplay over its own entity store, its sent packets and the client's lines; the player is a level-20
// character whose name the cache knows (as everyone's of the group list).
struct Harness {
    game::EntityStore store;
    game::Gameplay play{store};
    game::NoticeFeed feed;
    std::vector<Sent> sent;
    std::vector<u8> texts = test::textTable(kEnglish);
    u32 now = 1000;
    u32 cursor = 0;
    u32 lineCursor = 0;

    Harness() {
        text::setTable(texts);
        game::PacketSink sink;
        sink.context = this;
        sink.send = [](void* context, Opcode opcode, std::span<const u8> body) {
            static_cast<Harness*>(context)->sent.push_back({opcode, Bytes(body.begin(), body.end())});
            return true;
        };
        play.setSink(sink);
        play.enterWorld(kMe, 0, true, now);
        REQUIRE(store.applyUpdateObject(created(kMe, game::ObjectType::Player,
                                                {{uf::UNIT_FIELD_HEALTH, 100}, {uf::UNIT_FIELD_LEVEL, 20}}),
                                        now));
        net::NameQueryInfo me;
        me.guid = kMe;
        me.name = "Arthel";
        play.queries().onName(std::move(me));
        feed.init(nullptr, nullptr);
        feed.restart(play);
        cursor = play.events().end();
    }
    ~Harness() { text::unload(); }
    game::Gameplay::Handled packet(Opcode opcode, const Bytes& body) {
        return play.handle(opcode, body, now);
    }
    void tick(u32 ms = 16) {
        now += ms;
        play.update(now);
    }
    std::vector<Bytes> sentWith(Opcode opcode) const {
        std::vector<Bytes> out;
        for (const Sent& s : sent) {
            if (s.opcode == opcode) {
                out.push_back(s.body);
            }
        }
        return out;
    }
    std::vector<game::GameEvent> events() {
        std::vector<game::GameEvent> out;
        while (const game::GameEvent* e = play.events().next(cursor)) {
            out.push_back(*e);
        }
        return out;
    }
    std::vector<Line> lines() {
        game::CombatNames names;
        feed.update(play, store.find(kMe), names, now);
        std::vector<Line> out;
        while (const game::Notice* n = feed.lines().next(lineCursor)) {
            out.push_back({n->kind, n->text.data()});
        }
        return out;
    }
};

std::vector<ReadyCheckLine> readyLines(const std::vector<game::GameEvent>& events) {
    std::vector<ReadyCheckLine> out;
    for (const game::GameEvent& e : events) {
        if (e.kind == GameEventKind::ReadyCheckNotice) {
            out.push_back(static_cast<ReadyCheckLine>(e.value[0]));
        }
    }
    return out;
}

bool has(const std::vector<game::GameEvent>& events, GameEventKind kind) {
    return std::any_of(events.begin(), events.end(),
                       [&](const game::GameEvent& e) { return e.kind == kind; });
}

} // namespace

TEST_CASE("Raid alerts: MSG_RAID_READY_CHECK and SMSG_MEETINGSTONE_* byte for byte") {
    // The server's request is empty; a member's answer forwarded to the leader is its full GUID and the byte
    // it sent.
    net::ReadyCheckMessage msg;
    REQUIRE(net::parseReadyCheck({}, msg));
    CHECK_FALSE(msg.answer);
    const Bytes answer = {0x22, 0, 0, 0, 0, 0, 0, 0, 1};
    REQUIRE(net::parseReadyCheck(answer, msg));
    CHECK(msg.answer);
    CHECK(msg.member == kBrakk);
    CHECK(msg.state == 1);
    for (usize n = 1; n < answer.size(); ++n) { // truncated: neither a request nor an answer
        CHECK_FALSE(net::parseReadyCheck(std::span<const u8>(answer.data(), n), msg));
    }
    Bytes longer = answer;
    longer.push_back(0);
    CHECK_FALSE(net::parseReadyCheck(longer, msg));
    // The member's answer: one byte (ConfirmReadyCheck(1) / ConfirmReadyCheck()).
    Bytes yes, no;
    net::writeReadyCheckAnswer(yes, true);
    net::writeReadyCheckAnswer(no, false);
    CHECK(yes == Bytes{1});
    CHECK(no == Bytes{0});

    // CMSG_MEETINGSTONE_JOIN: the stone's full GUID (MeetingStoneJoin::ReadFromWorldPacket).
    Bytes join;
    net::writeMeetingStoneJoin(join, kStone);
    CHECK(join == Bytes{0x21, 0x4A, 0, 0, 0, 0, 0x10, 0xF1});
    // SMSG_MEETINGSTONE_SETQUEUE: u32 area, u8 status (MeetingstoneSetQueue::AppendBodyTo, build > 1.4.2).
    const Bytes queue = {0xCE, 0x02, 0, 0, 1};
    net::MeetingStoneQueue q;
    REQUIRE(net::parseMeetingStoneSetQueue(queue, q));
    CHECK(q.areaId == kWailingCaverns);
    CHECK(q.status == net::kMeetingStoneJoinedQueue);
    checkTruncations(queue, [](std::span<const u8> p) {
        net::MeetingStoneQueue out;
        return net::parseMeetingStoneSetQueue(p, out);
    });
    // SMSG_MEETINGSTONE_JOINFAILED: u8 reason; SMSG_MEETINGSTONE_MEMBER_ADDED: the player's full GUID.
    u8 reason = 0;
    REQUIRE(net::parseMeetingStoneJoinFailed(Bytes{3}, reason));
    CHECK(reason == net::kMeetingStoneFailRaidGroup);
    CHECK_FALSE(net::parseMeetingStoneJoinFailed({}, reason));
    const Bytes added = {0x22, 0, 0, 0, 0, 0, 0, 0};
    u64 player = 0;
    REQUIRE(net::parseMeetingStoneMemberAdded(added, player));
    CHECK(player == kBrakk);
    checkTruncations(added, [](std::span<const u8> p) {
        u64 out = 0;
        return net::parseMeetingStoneMemberAdded(p, out);
    });
}

TEST_CASE("Raid alerts: the leader's ready check (DoReadyCheck, the answers, CheckReadyCheckTime)") {
    Harness h;
    game::ReadyCheck& check = h.play.readyCheck();
    // Not in a group: DoReadyCheck does nothing (WoW.exe compares the group's leader with the player).
    CHECK_FALSE(h.play.commands().readyCheck());
    CHECK(h.sentWith(Opcode::MSG_RAID_READY_CHECK).empty());

    // A raid the player leads: Aeryn, Brakk and Mirelle online, Tolvir offline.
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, groupList(kMe,
                                                        {{"Aeryn", kAeryn, net::kMemberOnline},
                                                         {"Brakk", kBrakk, net::kMemberOnline},
                                                         {"Tolvir", kTolvir, 0},
                                                         {"Mirelle", kMirelle, net::kMemberOnline}},
                                                        true)) == game::Gameplay::Handled::Yes);
    h.tick();
    h.events();
    h.lines();
    h.sent.clear();

    // /readycheck and the raid tab's button: the empty request, 30 s, "Starting ready check...".
    REQUIRE(h.play.commands().readyCheck());
    CHECK(h.sentWith(Opcode::MSG_RAID_READY_CHECK) == std::vector<Bytes>{Bytes{}});
    CHECK(check.inProgress(h.now));
    CHECK(check.waitingFor(kAeryn));
    CHECK(check.waitingFor(kTolvir));
    CHECK_FALSE(check.waitingFor(kMe));
    CHECK(readyLines(h.events()) == std::vector<ReadyCheckLine>{ReadyCheckLine::Start});
    CHECK(h.lines() == std::vector<Line>{{NoticeKind::System, "Starting ready check..."}});
    // Again before the 30 s are over: the UIErrorsFrame says so, nothing goes out.
    h.tick(1000);
    CHECK_FALSE(h.play.commands().readyCheck());
    CHECK(h.sentWith(Opcode::MSG_RAID_READY_CHECK).size() == 1);
    CHECK(h.lines() == std::vector<Line>{{NoticeKind::Error, "Ready check running"}});

    // The leader's own request comes back (vMaNGOS broadcasts it): no frame for the leader, nothing ends.
    REQUIRE(h.packet(Opcode::MSG_RAID_READY_CHECK, {}) == game::Gameplay::Handled::Yes);
    auto events = h.events();
    CHECK_FALSE(has(events, GameEventKind::ReadyCheck));
    CHECK_FALSE(check.prompting());
    CHECK(check.inProgress(h.now));

    // Brakk says no (at once, in the chat), Aeryn yes (nothing): Mirelle still online and waited for.
    REQUIRE(h.packet(Opcode::MSG_RAID_READY_CHECK, answerBody(kBrakk, 0)) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::MSG_RAID_READY_CHECK, answerBody(kAeryn, 1)) == game::Gameplay::Handled::Yes);
    CHECK(readyLines(h.events()) == std::vector<ReadyCheckLine>{ReadyCheckLine::NotReady});
    CHECK(h.lines() == std::vector<Line>{{NoticeKind::System, "Brakk is not ready"}});
    CHECK(check.inProgress(h.now));
    // Mirelle answers: no online member is left, the check ends now and the offline one is the AFK list.
    REQUIRE(h.packet(Opcode::MSG_RAID_READY_CHECK, answerBody(kMirelle, 1)) == game::Gameplay::Handled::Yes);
    CHECK_FALSE(check.inProgress(h.now));
    CHECK(std::vector<u64>(check.afk().begin(), check.afk().end()) == std::vector<u64>{kTolvir});
    CHECK(readyLines(h.events()) == std::vector<ReadyCheckLine>{ReadyCheckLine::MembersAfk});
    h.play.queries().onName({kTolvir, "Tolvir"});
    CHECK(h.lines() == std::vector<Line>{{NoticeKind::System, "AFK: Tolvir"}});

    // A second check nobody answers: after 30 s every member waited for, in roster order, offline ones too.
    h.tick(2000);
    REQUIRE(h.play.commands().readyCheck());
    h.play.queries().onName({kAeryn, "Aeryn"});
    h.play.queries().onName({kMirelle, "Mirelle"});
    h.tick(game::ReadyCheck::kDurationMs - 100);
    CHECK(check.inProgress(h.now));
    h.tick(200);
    CHECK_FALSE(check.inProgress(h.now));
    CHECK(std::vector<u64>(check.afk().begin(), check.afk().end()) ==
          std::vector<u64>{kAeryn, kBrakk, kTolvir, kMirelle});
    h.lines();
    // Answers that come late still clear their flags (WoW.exe resets them at the next DoReadyCheck only):
    // when the last online one comes, the check "ends" again with the offline one.
    REQUIRE(h.packet(Opcode::MSG_RAID_READY_CHECK, answerBody(kAeryn, 1)) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::MSG_RAID_READY_CHECK, answerBody(kBrakk, 1)) == game::Gameplay::Handled::Yes);
    CHECK(h.lines().empty());
    REQUIRE(h.packet(Opcode::MSG_RAID_READY_CHECK, answerBody(kMirelle, 1)) == game::Gameplay::Handled::Yes);
    CHECK(h.lines() == std::vector<Line>{{NoticeKind::System, "AFK: Tolvir"}});
    // VD_READYCHECK_SELF (development): the leader's own request opens its frame, its "no" comes back to it.
    check.setSelfPrompt(true);
    h.tick(100);
    REQUIRE(h.play.commands().readyCheck());
    h.events();
    REQUIRE(h.packet(Opcode::MSG_RAID_READY_CHECK, {}) == game::Gameplay::Handled::Yes);
    CHECK(check.prompting());
    CHECK(check.promptLeader() == kMe);
    h.sent.clear();
    REQUIRE(check.answer(false, h.now));
    CHECK(h.sentWith(Opcode::MSG_RAID_READY_CHECK) == std::vector<Bytes>{Bytes{0}});
    REQUIRE(h.packet(Opcode::MSG_RAID_READY_CHECK, answerBody(kMe, 0)) == game::Gameplay::Handled::Yes);
    CHECK(h.lines() == std::vector<Line>{{NoticeKind::System, "Starting ready check..."},
                                         {NoticeKind::System, "Arthel is not ready"}});
    check.setSelfPrompt(false);
    h.tick(game::ReadyCheck::kDurationMs);
    h.lines();
    // Everybody answers a third one, nobody offline left out: READY_CHECK_NO_AFK at once.
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST,
                     groupList(kMe,
                               {{"Aeryn", kAeryn, net::kMemberOnline}, {"Brakk", kBrakk, net::kMemberOnline}},
                               true)) == game::Gameplay::Handled::Yes);
    h.tick(100);
    h.events();
    REQUIRE(h.play.commands().readyCheck());
    REQUIRE(h.packet(Opcode::MSG_RAID_READY_CHECK, answerBody(kAeryn, 1)) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::MSG_RAID_READY_CHECK, answerBody(kBrakk, 1)) == game::Gameplay::Handled::Yes);
    CHECK(readyLines(h.events()) ==
          std::vector<ReadyCheckLine>{ReadyCheckLine::Start, ReadyCheckLine::NoAfk});
    const std::vector<Line> lines = h.lines();
    REQUIRE(lines.size() == 2);
    CHECK(lines.back() == Line{NoticeKind::System, "No players are AFK"});
    CHECK(h.packet(Opcode::MSG_RAID_READY_CHECK, Bytes{1, 2, 3}) == game::Gameplay::Handled::Malformed);
}

TEST_CASE("Raid alerts: a member's ReadyCheckFrame (READY_CHECK, ConfirmReadyCheck, the 30 s)") {
    Harness h;
    game::ReadyCheck& check = h.play.readyCheck();
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST,
                     groupList(kAeryn,
                               {{"Aeryn", kAeryn, net::kMemberOnline}, {"Brakk", kBrakk, net::kMemberOnline}},
                               true)) == game::Gameplay::Handled::Yes);
    h.play.queries().onName({kAeryn, "Aeryn"});
    h.tick();
    h.events();
    h.lines();
    h.sent.clear();
    // Not the leader: the raid tab hides the button, and DoReadyCheck itself would send nothing.
    CHECK_FALSE(h.play.commands().readyCheck());
    CHECK(h.sentWith(Opcode::MSG_RAID_READY_CHECK).empty());

    // Aeryn's request: the line with her name, then READY_CHECK (the frame and its sound).
    REQUIRE(h.packet(Opcode::MSG_RAID_READY_CHECK, {}) == game::Gameplay::Handled::Yes);
    auto events = h.events();
    REQUIRE(has(events, GameEventKind::ReadyCheck));
    CHECK(std::find_if(events.begin(), events.end(), [](const game::GameEvent& e) {
              return e.kind == GameEventKind::ReadyCheck;
          })->guid == kAeryn);
    CHECK(check.prompting());
    CHECK(check.promptLeader() == kAeryn);
    CHECK(check.promptRemainingMs(h.now) == game::ReadyCheck::kDurationMs);
    CHECK(h.lines() == std::vector<Line>{{NoticeKind::System, "Aeryn: ready check"}});
    // Yes: one byte 1, the frame closes.
    REQUIRE(check.answer(true, h.now));
    CHECK(h.sentWith(Opcode::MSG_RAID_READY_CHECK) == std::vector<Bytes>{Bytes{1}});
    CHECK_FALSE(check.prompting());

    // Another request left unanswered: READY_CHECK_YOU_WERE_AFK after 30 s, and No then sends nothing.
    h.tick(1000);
    REQUIRE(h.packet(Opcode::MSG_RAID_READY_CHECK, {}) == game::Gameplay::Handled::Yes);
    h.lines();
    h.tick(game::ReadyCheck::kDurationMs);
    CHECK_FALSE(check.prompting());
    CHECK(readyLines(h.events()) ==
          std::vector<ReadyCheckLine>{ReadyCheckLine::LeaderStarted, ReadyCheckLine::YouWereAfk});
    CHECK(h.lines() == std::vector<Line>{{NoticeKind::System, "Missed a ready check"}});
    h.sent.clear();
    CHECK_FALSE(check.answer(false, h.now));
    CHECK(h.sent.empty());

    // No (ConfirmReadyCheck() without an argument): a byte 0.
    REQUIRE(h.packet(Opcode::MSG_RAID_READY_CHECK, {}) == game::Gameplay::Handled::Yes);
    REQUIRE(check.answer(false, h.now));
    CHECK(h.sentWith(Opcode::MSG_RAID_READY_CHECK) == std::vector<Bytes>{Bytes{0}});

    // The raid goes while the frame is up: it hides unanswered, without the line.
    REQUIRE(h.packet(Opcode::MSG_RAID_READY_CHECK, {}) == game::Gameplay::Handled::Yes);
    REQUIRE(check.prompting());
    h.events();
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, groupList(0, {}, false)) == game::Gameplay::Handled::Yes);
    h.tick();
    CHECK_FALSE(check.prompting());
    h.tick(game::ReadyCheck::kDurationMs);
    CHECK(readyLines(h.events()) == std::vector<ReadyCheckLine>{});
}

TEST_CASE("Raid alerts: meeting stones (the stone's checks, JOIN, the queue's answers, leaving)") {
    Harness h;
    game::MeetingStone& stone = h.play.meetingStone();
    // Entering the world asked for the queue (PLAYER_ENTERING_WORLD).
    CHECK(h.sentWith(Opcode::CMSG_MEETINGSTONE_INFO) == std::vector<Bytes>{Bytes{}});
    CHECK_FALSE(stone.inQueue());
    CHECK_FALSE(stone.statusKnown());
    REQUIRE(h.packet(Opcode::SMSG_MEETINGSTONE_SETQUEUE, setQueue(0, net::kMeetingStoneNone)) ==
            game::Gameplay::Handled::Yes);
    CHECK(stone.statusKnown());
    CHECK(h.lines().empty()); // status 5: silently

    // The Wailing Caverns stone (levels 15-25) in reach: tapping it is CMSG_MEETINGSTONE_JOIN, not
    // CMSG_GAMEOBJ_USE.
    REQUIRE(h.store.applyUpdateObject(created(kStone, game::ObjectType::GameObject,
                                              {{uf::OBJECT_FIELD_ENTRY, kStoneEntry},
                                               {uf::GAMEOBJECT_TYPE_ID, game::kGameObjectMeetingStone}}),
                                      h.now));
    net::GameObjectQueryInfo info;
    info.entry = kStoneEntry;
    info.found = true;
    info.type = game::kGameObjectMeetingStone;
    info.name = "Meeting Stone";
    info.data[0] = 15;
    info.data[1] = 25;
    info.data[2] = kWailingCaverns;
    h.play.queries().onGameObject(info);
    h.sent.clear();
    CHECK(h.play.interact(kStone) == game::Interaction::UseObject);
    Bytes join;
    net::writeMeetingStoneJoin(join, kStone);
    CHECK(h.sentWith(Opcode::CMSG_MEETINGSTONE_JOIN) == std::vector<Bytes>{join});
    CHECK(h.sentWith(Opcode::CMSG_GAMEOBJ_USE).empty());

    // Queued: the line with the zone (no AreaTable here: UNKNOWN), the minimap's button, the join's visual.
    h.events();
    REQUIRE(h.packet(Opcode::SMSG_MEETINGSTONE_SETQUEUE,
                     setQueue(kWailingCaverns, net::kMeetingStoneJoinedQueue)) ==
            game::Gameplay::Handled::Yes);
    CHECK(stone.inQueue());
    CHECK(stone.areaId() == kWailingCaverns);
    auto events = h.events();
    CHECK(has(events, GameEventKind::MeetingStoneChanged));
    CHECK(has(events, GameEventKind::MeetingStoneJoined));
    CHECK(h.lines() == std::vector<Line>{{NoticeKind::System, "Stone queue for Unknown."}});
    // The same queue again (a new world's answer to CMSG_MEETINGSTONE_INFO): nothing said.
    REQUIRE(h.packet(Opcode::SMSG_MEETINGSTONE_SETQUEUE,
                     setQueue(kWailingCaverns, net::kMeetingStoneJoinedQueue)) ==
            game::Gameplay::Handled::Yes);
    CHECK_FALSE(has(h.events(), GameEventKind::MeetingStoneJoined));
    CHECK(h.lines().empty());
    // Every 5 minutes, the group the stone filled, the complete group.
    REQUIRE(h.packet(Opcode::SMSG_MEETINGSTONE_IN_PROGRESS, {}) == game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_MEETINGSTONE_MEMBER_ADDED, Bytes{0x22, 0, 0, 0, 0, 0, 0, 0}) ==
            game::Gameplay::Handled::Yes);
    CHECK(h.sentWith(Opcode::CMSG_NAME_QUERY).size() == 1); // its name asked for; the line waits for it
    h.play.queries().onName({kBrakk, "Brakk"});
    REQUIRE(h.packet(Opcode::SMSG_MEETINGSTONE_COMPLETE, {}) == game::Gameplay::Handled::Yes);
    CHECK(h.lines() ==
          std::vector<Line>{
              {NoticeKind::System, "Stone still seeking."},
              {NoticeKind::System, "Stone added Brakk."},
              {NoticeKind::System, "Stone group complete."}});
    CHECK(h.packet(Opcode::SMSG_MEETINGSTONE_MEMBER_ADDED, Bytes{1, 2}) ==
          game::Gameplay::Handled::Malformed);

    // CONFIRM_LEAVE_QUEUE's accept: CMSG_MEETINGSTONE_LEAVE; the answer names the queue left.
    h.sent.clear();
    REQUIRE(stone.leave(h.now));
    CHECK(h.sentWith(Opcode::CMSG_MEETINGSTONE_LEAVE) == std::vector<Bytes>{Bytes{}});
    REQUIRE(h.packet(Opcode::SMSG_MEETINGSTONE_SETQUEUE, setQueue(0, net::kMeetingStoneLeftQueue)) ==
            game::Gameplay::Handled::Yes);
    CHECK_FALSE(stone.inQueue());
    CHECK(h.lines() == std::vector<Line>{{NoticeKind::System, "Left stone queue for Unknown."}});

    // The server's refusals (UIErrorsFrame).
    REQUIRE(h.packet(Opcode::SMSG_MEETINGSTONE_JOINFAILED, Bytes{net::kMeetingStoneFailFullGroup}) ==
            game::Gameplay::Handled::Yes);
    REQUIRE(h.packet(Opcode::SMSG_MEETINGSTONE_JOINFAILED, Bytes{9}) ==
            game::Gameplay::Handled::Yes); // unknown
    CHECK(h.lines() == std::vector<Line>{{NoticeKind::Error, "Stone: group full"}});
    CHECK(h.packet(Opcode::SMSG_MEETINGSTONE_SETQUEUE, Bytes{1, 2, 3}) == game::Gameplay::Handled::Malformed);

    // The client's own checks, in WoW.exe's order: a level out of 15-25...
    REQUIRE(h.store.applyUpdateObject(values(kMe, {{uf::UNIT_FIELD_LEVEL, 30}}), h.now));
    h.sent.clear();
    CHECK(h.play.interact(kStone) == game::Interaction::None);
    CHECK(h.sent.empty());
    CHECK(h.lines() == std::vector<Line>{{NoticeKind::Error, "Stone: wrong level."}});
    REQUIRE(h.store.applyUpdateObject(values(kMe, {{uf::UNIT_FIELD_LEVEL, 20}}), h.now));
    // ...a party member that is not the leader (also to leave the queue)...
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, groupList(kAeryn, {{"Aeryn", kAeryn, net::kMemberOnline}},
                                                        false)) == game::Gameplay::Handled::Yes);
    CHECK(h.play.interact(kStone) == game::Interaction::None);
    CHECK_FALSE(stone.leave(h.now));
    CHECK(h.lines() ==
          std::vector<Line>{
              {NoticeKind::Error, "Stone: leader only"},
              {NoticeKind::Error, "Stone: leader leaves"}});
    // ...a full party the player leads, then a raid.
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, groupList(kMe,
                                                        {{"Aeryn", kAeryn, net::kMemberOnline},
                                                         {"Brakk", kBrakk, net::kMemberOnline},
                                                         {"Tolvir", kTolvir, net::kMemberOnline},
                                                         {"Mirelle", kMirelle, net::kMemberOnline}},
                                                        false)) == game::Gameplay::Handled::Yes);
    CHECK(h.play.interact(kStone) == game::Interaction::None);
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, groupList(kMe, {{"Aeryn", kAeryn, net::kMemberOnline}},
                                                        true)) == game::Gameplay::Handled::Yes);
    CHECK(h.play.interact(kStone) == game::Interaction::None);
    CHECK(h.lines() ==
          std::vector<Line>{{NoticeKind::Error, "Stone: group full"},
                            {NoticeKind::Error, "Stone: no raids"}});
    CHECK(h.sentWith(Opcode::CMSG_MEETINGSTONE_JOIN).empty());
    // Every world entry asks again (a new map of the same character too).
    h.sent.clear();
    h.play.enterWorld(kMe, 1, false, h.now);
    CHECK(h.sentWith(Opcode::CMSG_MEETINGSTONE_INFO).size() == 1);
    CHECK_FALSE(stone.statusKnown());
}

TEST_CASE("Raid alerts: cost of a 40-member raid's ready check") {
    // The leader of a full raid (39 others, online): the per-frame update while the check runs, the 39
    // answers, and the end after 30 s with every name in RAID_MEMBERS_AFK (no allocation in any of them once
    // warmed up).
    Harness h;
    std::vector<std::tuple<const char*, u64, u8>> members;
    std::vector<std::string> names;
    names.reserve(39);
    for (u32 i = 0; i < 39; ++i) {
        names.push_back("Raider" + std::to_string(i));
    }
    for (u32 i = 0; i < 39; ++i) {
        members.emplace_back(names[i].c_str(), 0x100 + i, net::kMemberOnline);
    }
    REQUIRE(h.packet(Opcode::SMSG_GROUP_LIST, groupList(kMe, members, true)) == game::Gameplay::Handled::Yes);
    for (u32 i = 0; i < 39; ++i) {
        h.play.queries().onName({0x100 + i, names[i]});
    }
    h.tick();
    game::ReadyCheck& check = h.play.readyCheck();
    using Clock = std::chrono::steady_clock;
    REQUIRE(check.start(h.now));
    constexpr u32 kFrames = 20000;
    auto t0 = Clock::now();
    for (u32 i = 0; i < kFrames; ++i) {
        check.update(h.now);
    }
    const f64 frameNs = std::chrono::duration<f64, std::nano>(Clock::now() - t0).count() / kFrames;
    Bytes answer;
    t0 = Clock::now();
    for (u32 i = 0; i < 39; ++i) {
        answer.clear();
        net::ByteWriter(answer).u64v(0x100 + i).u8v(1);
        REQUIRE(check.handle(Opcode::MSG_RAID_READY_CHECK, answer, h.now));
    }
    const f64 answersUs = std::chrono::duration<f64, std::micro>(Clock::now() - t0).count();
    CHECK_FALSE(check.inProgress(h.now)); // the last answer ended it
    h.events();
    h.lines();
    // Nobody answers the next one: its end, then the line with 39 names.
    h.tick(1000);
    REQUIRE(check.start(h.now));
    h.now += game::ReadyCheck::kDurationMs;
    t0 = Clock::now();
    check.update(h.now);
    const f64 endUs = std::chrono::duration<f64, std::micro>(Clock::now() - t0).count();
    CHECK(check.afk().size() == 39);
    t0 = Clock::now();
    const std::vector<Line> lines = h.lines();
    const f64 lineUs = std::chrono::duration<f64, std::micro>(Clock::now() - t0).count();
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].text.rfind("AFK: Raider0, Raider1, ", 0) == 0);
    CHECK(lines[1].text.find("Raider38") != std::string::npos);
    MESSAGE("ReadyCheck::update " << frameNs << " ns per frame; 39 answers " << answersUs << " us; the end "
                                  << endUs << " us; the AFK line (39 names) " << lineUs << " us");
}
