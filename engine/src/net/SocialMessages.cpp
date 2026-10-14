#include "net/SocialMessages.h"

namespace vd::net {

// -------------------------------------------------------------------------------------------------------
// Characters.

void writeCharCreate(std::vector<u8>& out, const CharCreateRequest& c) {
    ByteWriter(out)
        .cstring(c.name)
        .u8v(c.race)
        .u8v(c.cls)
        .u8v(c.gender)
        .u8v(c.skin)
        .u8v(c.face)
        .u8v(c.hairStyle)
        .u8v(c.hairColor)
        .u8v(c.facialHair)
        .u8v(c.outfit);
}

bool parseU8(std::span<const u8> payload, u8& value) {
    ByteReader r(payload);
    value = r.u8v();
    return r.ok();
}

// -------------------------------------------------------------------------------------------------------
// Groups.

bool parseGroupList(std::span<const u8> payload, GroupList& out) {
    ByteReader r(payload);
    out.groupType = r.u8v();
    const u8 own = r.u8v();
    out.ownSubgroup = own & 0x7F;
    out.ownAssistant = (own & 0x80) != 0;
    const u32 count = r.u32v();
    out.members.clear();
    out.lootMethod = 0;
    out.looter = 0;
    out.lootThreshold = 0;
    // Each member takes at least 11 bytes: a count the packet cannot hold is malformed (and never reserved).
    if (r.failed() || count > kMaxRaidSize || count * 11u > r.remaining()) {
        return false;
    }
    for (u32 i = 0; i < count && r.ok(); ++i) {
        GroupList::Member m;
        m.name = r.cstring();
        m.guid = r.u64v();
        m.status = r.u8v();
        const u8 flags = r.u8v();
        m.subgroup = flags & 0x7F;
        m.assistant = (flags & 0x80) != 0;
        out.members.push_back(m);
    }
    out.leader = r.u64v();
    if (count > 0) {
        out.lootMethod = r.u8v();
        out.looter = r.u64v();
        out.lootThreshold = r.u8v();
        r.u8v(); // dungeon difficulty (always 0 in 1.x)
    }
    if (!r.ok()) {
        out.members.clear();
        return false;
    }
    return true;
}

bool parsePartyMemberStats(std::span<const u8> payload, PartyMemberStats& out) {
    ByteReader r(payload);
    out = PartyMemberStats{};
    out.guid = r.packedGuid();
    const u32 mask = r.u32v();
    out.mask = mask;
    if (mask & kGroupUpdateStatus) {
        out.status = r.u8v();
    }
    if (mask & kGroupUpdateCurHp) {
        out.health = r.u16v();
    }
    if (mask & kGroupUpdateMaxHp) {
        out.maxHealth = r.u16v();
    }
    if (mask & kGroupUpdatePowerType) {
        out.powerType = r.u8v();
    }
    if (mask & kGroupUpdateCurPower) {
        out.power = r.u16v();
    }
    if (mask & kGroupUpdateMaxPower) {
        out.maxPower = r.u16v();
    }
    if (mask & kGroupUpdateLevel) {
        out.level = r.u16v();
    }
    if (mask & kGroupUpdateZone) {
        out.zone = r.u16v();
    }
    if (mask & kGroupUpdatePosition) {
        out.x = static_cast<i16>(r.u16v());
        out.y = static_cast<i16>(r.u16v());
    }
    if (mask & kGroupUpdateAuras) {
        out.auraMask = r.u32v();
        for (u32 i = 0; i < 32; ++i) {
            if (out.auraMask & (1u << i)) {
                out.auras[i] = r.u16v();
            }
        }
    }
    if (mask & kGroupUpdateAurasNegative) {
        out.negativeAuraMask = r.u16v();
        for (u32 i = 0; i < 16; ++i) {
            if (out.negativeAuraMask & (1u << i)) {
                out.negativeAuras[i] = r.u16v();
            }
        }
    }
    if (mask & kGroupUpdatePetGuid) {
        out.petGuid = r.u64v();
    }
    if (mask & kGroupUpdatePetName) {
        out.petName = r.cstring();
    }
    if (mask & kGroupUpdatePetModel) {
        out.petDisplay = r.u16v();
    }
    if (mask & kGroupUpdatePetCurHp) {
        out.petHealth = r.u16v();
    }
    if (mask & kGroupUpdatePetMaxHp) {
        out.petMaxHealth = r.u16v();
    }
    if (mask & kGroupUpdatePetPowerType) {
        out.petPowerType = r.u8v();
    }
    if (mask & kGroupUpdatePetCurPower) {
        out.petPower = r.u16v();
    }
    if (mask & kGroupUpdatePetMaxPower) {
        out.petMaxPower = r.u16v();
    }
    if (mask & kGroupUpdatePetAuras) { // phase 15 (pets): kept for PartyMemberPetFrame's debuffs
        out.petAuraMask = r.u32v();
        for (u32 i = 0; i < 32; ++i) {
            if (out.petAuraMask & (1u << i)) {
                out.petAuras[i] = r.u16v();
            }
        }
    }
    if (mask & kGroupUpdatePetAurasNegative) {
        out.petNegativeAuraMask = r.u16v();
        for (u32 i = 0; i < 16; ++i) {
            if (out.petNegativeAuraMask & (1u << i)) {
                out.petNegativeAuras[i] = r.u16v();
            }
        }
    }
    return r.ok();
}

bool parsePartyCommandResult(std::span<const u8> payload, PartyCommandResult& out) {
    ByteReader r(payload);
    out.operation = r.u32v();
    out.name = r.cstring();
    out.result = r.u32v();
    return r.ok();
}

void writeCString(std::vector<u8>& out, std::string_view text) {
    ByteWriter(out).cstring(text);
}

void writeLootMethod(std::vector<u8>& out, u32 method, u64 master, u32 threshold) {
    ByteWriter(out).u32v(method).u64v(master).u32v(threshold);
}

void writeGroupChangeSubGroup(std::vector<u8>& out, std::string_view name, u8 subgroup) {
    ByteWriter(out).cstring(name).u8v(subgroup);
}

void writeGroupSwapSubGroup(std::vector<u8>& out, std::string_view name, std::string_view other) {
    ByteWriter(out).cstring(name).cstring(other);
}

void writeRandomRoll(std::vector<u8>& out, u32 minimum, u32 maximum) {
    ByteWriter(out).u32v(minimum).u32v(maximum);
}

bool parseRandomRoll(std::span<const u8> payload, RandomRoll& out) {
    ByteReader r(payload);
    out.minimum = r.u32v();
    out.maximum = r.u32v();
    out.roll = r.u32v();
    out.guid = r.u64v();
    return r.ok();
}

// -------------------------------------------------------------------------------------------------------
// Chat channels.

bool parseChannelNotify(std::span<const u8> payload, ChannelNotify& out) {
    ByteReader r(payload);
    out = ChannelNotify{};
    const u8 notice = r.u8v();
    out.notice = static_cast<ChannelNotice>(notice);
    out.channel = r.cstring();
    switch (out.notice) {
    case ChannelNotice::Joined:
    case ChannelNotice::Left:
    case ChannelNotice::PasswordChanged:
    case ChannelNotice::OwnerChanged:
    case ChannelNotice::AnnouncementsOn:
    case ChannelNotice::AnnouncementsOff:
    case ChannelNotice::ModerationOn:
    case ChannelNotice::ModerationOff:
    case ChannelNotice::PlayerAlreadyMember:
    case ChannelNotice::Invite: out.guid = r.u64v(); break;
    case ChannelNotice::YouJoined:
        out.flags = r.u32v();
        r.u32v(); // a number the client appends to the name: always 0
        break;
    case ChannelNotice::ModeChange:
        out.guid = r.u64v();
        out.oldFlags = r.u8v();
        out.newFlags = r.u8v();
        break;
    case ChannelNotice::PlayerKicked:
    case ChannelNotice::PlayerBanned:
    case ChannelNotice::PlayerUnbanned:
        out.guid = r.u64v();
        out.source = r.u64v();
        break;
    case ChannelNotice::PlayerNotFound:
    case ChannelNotice::ChannelOwner:
    case ChannelNotice::PlayerNotBanned:
    case ChannelNotice::PlayerInvited:
    case ChannelNotice::PlayerInviteBanned: out.name = r.cstring(); break;
    default:
        if (notice > static_cast<u8>(ChannelNotice::Throttled)) {
            r.fail(); // unknown layout
        }
        break; // the others carry nothing more
    }
    return r.ok();
}

bool parseChannelList(std::span<const u8> payload, ChannelList& out) {
    ByteReader r(payload);
    out.channel = r.cstring();
    out.flags = r.u8v();
    const u32 count = r.u32v();
    out.members.clear();
    if (r.failed() || count > r.remaining() / 9) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        ChannelList::Member m;
        m.guid = r.u64v();
        m.flags = r.u8v();
        out.members.push_back(m);
    }
    return r.ok();
}

void writeJoinChannel(std::vector<u8>& out, std::string_view channel, std::string_view password) {
    ByteWriter(out).cstring(channel).cstring(password);
}

void writeTextEmote(std::vector<u8>& out, u32 textEmote, u32 emoteNum, u64 target) {
    ByteWriter(out).u32v(textEmote).u32v(emoteNum).u64v(target);
}

// -------------------------------------------------------------------------------------------------------
// Friends and ignore list.

bool parseFriendList(std::span<const u8> payload, std::vector<FriendEntry>& out) {
    ByteReader r(payload);
    const u8 count = r.u8v();
    out.clear();
    if (r.failed() || count * 9u > r.remaining()) {
        return false;
    }
    for (u32 i = 0; i < count && r.ok(); ++i) {
        FriendEntry f;
        f.guid = r.u64v();
        f.status = r.u8v();
        if (f.status != kFriendOffline) {
            f.area = r.u32v();
            f.level = r.u32v();
            f.cls = r.u32v();
        }
        out.push_back(f);
    }
    if (!r.ok()) {
        out.clear();
        return false;
    }
    return true;
}

bool parseIgnoreList(std::span<const u8> payload, std::vector<u64>& out) {
    ByteReader r(payload);
    const u8 count = r.u8v();
    out.clear();
    if (r.failed() || count * 8u > r.remaining()) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        out.push_back(r.u64v());
    }
    return r.ok();
}

bool parseFriendStatus(std::span<const u8> payload, FriendStatus& out) {
    ByteReader r(payload);
    out = FriendStatus{};
    out.result = r.u8v();
    out.guid = r.u64v();
    const FriendResult result = static_cast<FriendResult>(out.result);
    if (result == FriendResult::AddedOnline || result == FriendResult::Online) {
        out.hasInfo = true;
        out.status = r.u8v();
        out.area = r.u32v();
        out.level = r.u32v();
        out.cls = r.u32v();
    }
    return r.ok();
}

// -------------------------------------------------------------------------------------------------------
// Who.

void writeWho(std::vector<u8>& out, const WhoRequest& request) {
    ByteWriter w(out);
    w.u32v(request.minLevel).u32v(request.maxLevel).cstring(request.name).cstring(request.guild);
    w.u32v(request.raceMask).u32v(request.classMask);
    const u32 zones = static_cast<u32>(request.zones.size() < 10 ? request.zones.size() : 10);
    w.u32v(zones);
    for (u32 i = 0; i < zones; ++i) {
        w.u32v(request.zones[i]);
    }
    const u32 terms = static_cast<u32>(request.terms.size() < 4 ? request.terms.size() : 4);
    w.u32v(terms);
    for (u32 i = 0; i < terms; ++i) {
        w.cstring(request.terms[i]);
    }
}

bool parseWhoList(std::span<const u8> payload, WhoList& out) {
    ByteReader r(payload);
    const u32 listed = r.u32v();
    out.online = r.u32v();
    out.entries.clear();
    // Each entry takes at least 18 bytes (two empty strings and four u32).
    if (r.failed() || listed > 50 || listed * 18u > r.remaining()) {
        return false;
    }
    for (u32 i = 0; i < listed && r.ok(); ++i) {
        WhoList::Entry e;
        e.name = r.cstring();
        e.guild = r.cstring();
        e.level = r.u32v();
        e.cls = r.u32v();
        e.race = r.u32v();
        e.zone = r.u32v();
        out.entries.push_back(e);
    }
    if (!r.ok()) {
        out.entries.clear();
        return false;
    }
    return true;
}

// -------------------------------------------------------------------------------------------------------
// Guilds.

bool parseGuildQueryResponse(std::span<const u8> payload, GuildQueryResponse& out) {
    ByteReader r(payload);
    out.guildId = r.u32v();
    out.name = r.cstring();
    for (std::string_view& rank : out.ranks) {
        rank = r.cstring();
    }
    out.emblemStyle = r.i32v();
    out.emblemColor = r.i32v();
    out.borderStyle = r.i32v();
    out.borderColor = r.i32v();
    out.backgroundColor = r.i32v();
    return r.ok();
}

bool parseGuildRoster(std::span<const u8> payload, GuildRoster& out) {
    ByteReader r(payload);
    const u32 count = r.u32v();
    out.motd = r.cstring();
    out.info = r.cstring();
    const u32 ranks = r.u32v();
    out.rankRights.clear();
    out.members.clear();
    if (r.failed() || ranks > kGuildRankCount) {
        return false;
    }
    for (u32 i = 0; i < ranks; ++i) {
        out.rankRights.push_back(r.u32v());
    }
    // A member takes at least 21 bytes (GUID, flags, empty name, rank, level, class, zone, two empty notes).
    if (r.failed() || count > r.remaining() / 21) {
        out.rankRights.clear();
        return false;
    }
    for (u32 i = 0; i < count && r.ok(); ++i) {
        GuildRoster::Member m;
        m.guid = r.u64v();
        m.presence = r.u8v();
        m.name = r.cstring();
        m.rank = r.u32v();
        m.level = r.u8v();
        m.cls = r.u8v();
        m.zone = r.u32v();
        if (m.presence == 0) {
            m.offlineDays = r.f32v();
        }
        m.note = r.cstring();
        m.officerNote = r.cstring();
        out.members.push_back(m);
    }
    if (!r.ok()) {
        out.rankRights.clear();
        out.members.clear();
        return false;
    }
    return true;
}

bool parseGuildEvent(std::span<const u8> payload, GuildEvent& out) {
    ByteReader r(payload);
    out = GuildEvent{};
    out.event = r.u8v();
    out.count = r.u8v();
    for (u32 i = 0; i < out.count && r.ok(); ++i) {
        const std::string_view s = r.cstring();
        if (i < out.params.size()) {
            out.params[i] = s;
        }
    }
    if (r.remaining() >= 8) {
        out.guid = r.u64v(); // only when the event is about a player (joined, left, signed on / off)
    }
    return r.ok() && r.atEnd();
}

bool parseGuildInvite(std::span<const u8> payload, GuildInvite& out) {
    ByteReader r(payload);
    out.inviter = r.cstring();
    out.guild = r.cstring();
    return r.ok();
}

bool parseGuildCommandResult(std::span<const u8> payload, GuildCommandResult& out) {
    ByteReader r(payload);
    out.command = r.u32v();
    out.name = r.cstring();
    out.result = r.u32v();
    return r.ok();
}

bool parseGuildInfo(std::span<const u8> payload, GuildInfo& out) {
    ByteReader r(payload);
    out.name = r.cstring();
    out.day = r.u32v();
    out.month = r.u32v();
    out.year = r.u32v();
    out.members = r.u32v();
    out.accounts = r.u32v();
    return r.ok();
}

// -------------------------------------------------------------------------------------------------------
// Duels.

bool parseDuelRequested(std::span<const u8> payload, DuelRequested& out) {
    ByteReader r(payload);
    out.arbiter = r.u64v();
    out.initiator = r.u64v();
    return r.ok();
}

bool parseDuelWinner(std::span<const u8> payload, DuelWinner& out) {
    ByteReader r(payload);
    out.fled = r.u8v() != 0;
    out.winner = r.cstring();
    out.loser = r.cstring();
    return r.ok();
}

} // namespace vd::net
