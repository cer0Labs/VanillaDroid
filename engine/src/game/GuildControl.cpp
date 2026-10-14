#include "game/GuildControl.h"

#include "core/Profile.h"
#include "game/Social.h"
#include "net/GuildMessages.h"
#include "net/SocialMessages.h"

#include <algorithm>

namespace vd::game {

using net::Opcode;

namespace {

i32 compareText(std::string_view a, std::string_view b) {
    const i32 c = a.compare(b);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

} // namespace

GuildControl::GuildControl(const Social& social) : m_social(social) {
    m_body.reserve(160);
    m_roster.reserve(64);
    enterWorld(0, true);
    m_inWorld = false;
}

const GuildState& GuildControl::guild() const {
    return m_social.guild();
}

void GuildControl::enterWorld(u64 player, bool newLogin) {
    m_inWorld = true;
    m_player = player;
    if (newLogin) {
        // WoW.exe 0x4D0A50: the key list starts in id order, none turned round.
        for (u32 i = 0; i < kGuildSortKeys; ++i) {
            m_keys[i] = static_cast<GuildSort>(i);
            m_reversed[i] = false;
        }
        m_selectionLength = 0;
        m_selection[0] = '\0';
        m_roster.clear();
        m_rosterVersion = ~0u;
        m_sortChanged = true;
    }
}

void GuildControl::leaveWorld() {
    m_inWorld = false;
}

bool GuildControl::sendBody(Opcode opcode) {
    return m_inWorld && m_sink(opcode, m_body);
}

// --- The roster
// ------------------------------------------------------------------------------------------------

i32 GuildControl::compare(const GuildMember& a, const GuildMember& b) const {
    const bool aOn = a.presence != 0, bOn = b.presence != 0;
    if (aOn != bOn) {
        return aOn ? -1 : 1; // without "show offline": the online members first, whatever the keys
    }
    for (u32 i = 0; i < kGuildSortKeys; ++i) {
        i32 c = 0;
        switch (m_keys[i]) {
        case GuildSort::Rank: c = a.rank == b.rank ? 0 : (a.rank > b.rank ? -1 : 1); break;
        case GuildSort::Level: c = a.level == b.level ? 0 : (a.level > b.level ? 1 : -1); break;
        case GuildSort::Name: c = compareText(a.name, b.name); break;
        case GuildSort::Zone:
            c = m_zoneName
                    ? compareText(m_zoneName(m_namesContext, a.zone), m_zoneName(m_namesContext, b.zone))
                    : (a.zone == b.zone ? 0 : (a.zone < b.zone ? -1 : 1));
            break;
        case GuildSort::Class:
            c = m_className
                    ? compareText(m_className(m_namesContext, a.cls), m_className(m_namesContext, b.cls))
                    : (a.cls == b.cls ? 0 : (a.cls < b.cls ? -1 : 1));
            break;
        case GuildSort::Group: break; // "group": nothing to compare in a guild
        case GuildSort::Online:
            if (!aOn) { // both offline: the more recently seen first
                c = a.offlineDays < b.offlineDays ? -1 : 1;
            }
            break;
        case GuildSort::Note: c = compareText(a.note, b.note); break;
        case GuildSort::Count: break;
        }
        if (c != 0) {
            return m_reversed[i] ? -c : c;
        }
    }
    return 0;
}

std::span<const u32> GuildControl::roster() {
    const GuildState& g = guild();
    if (g.rosterVersion != m_rosterVersion || m_sortChanged) {
        VD_PROFILE_ZONE("GuildControl::roster");
        m_rosterVersion = g.rosterVersion;
        m_sortChanged = false;
        m_roster.clear();
        for (u32 i = 0; i < g.members.size(); ++i) {
            if (g.members[i].presence != 0) {
                m_roster.push_back(i);
            }
        }
        std::stable_sort(m_roster.begin(), m_roster.end(),
                         [&](u32 a, u32 b) { return compare(g.members[a], g.members[b]) < 0; });
    }
    return m_roster;
}

void GuildControl::sort(GuildSort key) {
    u32 at = 0;
    while (at < kGuildSortKeys && m_keys[at] != key) {
        ++at;
    }
    if (at == kGuildSortKeys) {
        return;
    }
    bool reversed = m_reversed[at];
    if (at == 0) {
        reversed = !reversed; // the first key again: its order turns round
    }
    for (u32 i = at; i > 0; --i) {
        m_keys[i] = m_keys[i - 1];
        m_reversed[i] = m_reversed[i - 1];
    }
    m_keys[0] = key;
    m_reversed[0] = reversed;
    m_sortChanged = true;
}

void GuildControl::select(std::string_view name) {
    m_selectionLength = static_cast<u32>(std::min(name.size(), m_selection.size() - 1));
    std::copy_n(name.data(), m_selectionLength, m_selection.data());
    m_selection[m_selectionLength] = '\0';
}

const GuildMember* GuildControl::selected() const {
    if (m_selectionLength == 0) {
        return nullptr;
    }
    const std::string_view name = selection();
    for (const GuildMember& m : guild().members) {
        if (m.name == name && m.presence != 0) {
            return &m;
        }
    }
    return nullptr;
}

// --- Rights
// ----------------------------------------------------------------------------------------------------

bool GuildControl::inGuild() const {
    return guild().id != 0;
}

bool GuildControl::isGuildLeader() const {
    return inGuild() && guild().rank == 0;
}

bool GuildControl::hasRight(u32 bit) const {
    const GuildState& g = guild();
    return g.id != 0 && g.rank < g.rankRights.size() && (g.rankRights[g.rank] & bit) != 0;
}

bool GuildControl::canPromote(u32 playerRank, u32 memberRank, bool right) {
    // CanGuildPromote() and rankIndex > 1 and rankIndex > guildRankIndex + 1 (1-based Lua indices: the
    // member's rank above officer and two below the player's).
    return right && memberRank > 1 && memberRank > playerRank + 1;
}

bool GuildControl::canDemote(u32 playerRank, u32 memberRank, u32 lowestRank, bool right) {
    // CanGuildDemote() and rankIndex >= 1 and rankIndex > guildRankIndex and rankIndex ~= maxRankIndex.
    return right && memberRank >= 1 && memberRank > playerRank && memberRank != lowestRank;
}

bool GuildControl::canRemove(u32 playerRank, u32 memberRank, bool right) {
    // CanGuildRemove() and rankIndex >= 1 and rankIndex > guildRankIndex.
    return right && memberRank >= 1 && memberRank > playerRank;
}

GuildMemberRights GuildControl::memberRights(const GuildMember& member) const {
    const GuildState& g = guild();
    GuildMemberRights r;
    const u32 ranks = rankCount();
    const u32 lowest = ranks > 0 ? ranks - 1 : 0;
    r.promote = canPromote(g.rank, member.rank, hasRight(guildbit::kPromote));
    r.demote = canDemote(g.rank, member.rank, lowest, hasRight(guildbit::kDemote));
    r.remove = canRemove(g.rank, member.rank, hasRight(guildbit::kRemove));
    // (UnitName("player") == name) or (not online): no group invitation.
    r.groupInvite = member.guid != m_player && member.presence != 0;
    r.editPublicNote = hasRight(guildbit::kEditPublicNote);
    r.viewOfficerNote = hasRight(guildbit::kViewOfficerNote);
    r.editOfficerNote = r.viewOfficerNote && hasRight(guildbit::kEditOfficerNote);
    return r;
}

// --- Notes and the information text
// ----------------------------------------------------------------------------

bool GuildControl::setPublicNote(std::string_view member, std::string_view note) {
    if (!inGuild() || member.empty()) {
        return false;
    }
    m_body.clear();
    net::writeGuildSetNote(m_body, member, note.substr(0, net::kGuildNoteMax));
    return sendBody(Opcode::CMSG_GUILD_SET_PUBLIC_NOTE);
}

bool GuildControl::setOfficerNote(std::string_view member, std::string_view note) {
    if (!inGuild() || member.empty()) {
        return false;
    }
    m_body.clear();
    net::writeGuildSetNote(m_body, member, note.substr(0, net::kGuildNoteMax));
    return sendBody(Opcode::CMSG_GUILD_SET_OFFICER_NOTE);
}

bool GuildControl::setInfoText(std::string_view text) {
    if (!inGuild()) {
        return false;
    }
    m_body.clear();
    net::writeCString(m_body, text.substr(0, net::kGuildInfoMax));
    if (!sendBody(Opcode::CMSG_GUILD_INFO_TEXT)) {
        return false;
    }
    m_body.clear();
    sendBody(Opcode::CMSG_GUILD_ROSTER);
    return true;
}

// --- Ranks
// -----------------------------------------------------------------------------------------------------

u32 GuildControl::rankCount() const {
    const GuildState& g = guild();
    if (!g.rankRights.empty()) {
        return static_cast<u32>(g.rankRights.size());
    }
    u32 n = 0;
    while (n < g.ranks.size() && !g.ranks[n].empty()) {
        ++n;
    }
    return n;
}

std::string_view GuildControl::rankName(u32 rank) const {
    return guild().rankName(rank);
}

u32 GuildControl::rankRights(u32 rank) const {
    const GuildState& g = guild();
    return rank < g.rankRights.size() ? g.rankRights[rank] : 0;
}

std::array<bool, kGuildControlOptions> GuildControl::rankFlags(u32 rank) const {
    std::array<bool, kGuildControlOptions> out{};
    const u32 rights = inGuild() ? rankRights(rank) : 0;
    for (u32 i = 0; i < kGuildControlOptions; ++i) {
        out[i] = (rights & kGuildControlFlags[i]) != 0;
    }
    return out;
}

bool GuildControl::saveRank(u32 rank, std::string_view name,
                            const std::array<bool, kGuildControlOptions>& flags) {
    // GuildControlSaveRank (WoW.exe 0x4D20D0): a name, a rank that exists, the rank's rights with each box
    // applied.
    if (!isGuildLeader() || name.empty() || name.size() > net::kGuildRankNameMax || rank >= rankCount()) {
        return false;
    }
    u32 rights = rankRights(rank);
    for (u32 i = 0; i < kGuildControlOptions; ++i) {
        rights = flags[i] ? (rights | kGuildControlFlags[i]) : (rights & ~kGuildControlFlags[i]);
    }
    m_body.clear();
    net::writeGuildRank(m_body, rank, rights, name);
    return sendBody(Opcode::CMSG_GUILD_RANK);
}

bool GuildControl::addRank(std::string_view name) {
    if (!isGuildLeader() || name.empty() || name.size() > net::kGuildRankNameMax ||
        rankCount() >= net::kGuildRanksMax) {
        return false;
    }
    m_body.clear();
    net::writeCString(m_body, name);
    return sendBody(Opcode::CMSG_GUILD_ADD_RANK);
}

bool GuildControl::deleteLastRank() {
    if (!isGuildLeader() || rankCount() <= net::kGuildRanksMin) {
        return false;
    }
    m_body.clear();
    return sendBody(Opcode::CMSG_GUILD_DEL_RANK);
}

u32 GuildControl::membersInRank(u32 rank) const {
    u32 n = 0;
    for (const GuildMember& m : guild().members) {
        n += m.presence != 0 && m.rank == rank ? 1u
                                               : 0u; // GuildStatus_Update counts the listed (online) members
    }
    return n;
}

bool GuildControl::canDeleteRank(u32 rank) const {
    const u32 ranks = rankCount();
    return isGuildLeader() && ranks > net::kGuildRanksMin && rank + 1 == ranks && membersInRank(rank) == 0;
}

} // namespace vd::game
