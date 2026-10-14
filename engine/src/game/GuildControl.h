#pragma once

#include "core/Types.h"
#include "game/GameEvents.h"
#include "net/Opcodes.gen.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace vd::game {

class Social;
struct GuildState;
struct GuildMember;

// SortGuildRoster's keys (WoW.exe 0x4D1CB0: "rank", "level", "name", "zone", "class", "group", "online",
// "note" and the ids its key table keeps them by; "group" compares nothing).
enum class GuildSort : u8 { Rank, Level, Name, Zone, Class, Group, Online, Note, Count };
constexpr u32 kGuildSortKeys = static_cast<u32>(GuildSort::Count);

// GuildControlPopupFrameCheckbox1..13 (GUILDCONTROL_OPTION1..13: guild chat listen and speak, officer chat
// listen and speak, promote, demote, invite, remove, set MOTD, edit public note, view officer note, edit
// officer note, modify guild info): the rank right bit each one toggles (WoW.exe 0x849670; vMaNGOS
// GuildRankRights in that order).
constexpr u32 kGuildControlOptions = 13;
constexpr std::array<u32, kGuildControlOptions> kGuildControlFlags = {
    0x1, 0x2, 0x4, 0x8, 0x80, 0x100, 0x10, 0x20, 0x1000, 0x2000, 0x4000, 0x8000, 0x10000};
// The bits the client tests for each right (CanEditMOTD 0x4D1A80 tests 0x1000 alone, CanEditGuildInfo
// 0x10000...).
namespace guildbit {
constexpr u32 kInvite = 0x10;
constexpr u32 kRemove = 0x20;
constexpr u32 kPromote = 0x80;
constexpr u32 kDemote = 0x100;
constexpr u32 kSetMotd = 0x1000;
constexpr u32 kEditPublicNote = 0x2000;
constexpr u32 kViewOfficerNote = 0x4000;
constexpr u32 kEditOfficerNote = 0x8000;
constexpr u32 kModifyInfo = 0x10000;
} // namespace guildbit

// What the guild frame lets the player do with a member (GuildStatus_Update's button rules,
// FriendsFrame.lua).
struct GuildMemberRights {
    bool promote = false; // GuildFramePromoteButton
    bool demote = false;  // GuildFrameDemoteButton
    bool remove = false;  // GuildMemberRemoveButton
    bool groupInvite = false;
    bool editPublicNote = false;
    bool viewOfficerNote = false;
    bool editOfficerNote = false;
};

// Phase 15 (guild): the rest of the guild's management behind 1.12's GuildFrame (FriendsFrame.lua / .xml):
// the roster as GetGuildRosterInfo lists it (sorted by SortGuildRoster's key list), the selected member
// (SetGuildRosterSelection), the rights the frame tests (CanEditMOTD, CanEditPublicNote, CanViewOfficerNote,
// CanEditOfficerNote, CanEditGuildInfo, CanGuildInvite / Promote / Demote / Remove, IsGuildLeader), the notes
// (GuildRosterSetPublicNote / OfficerNote), the guild information text (SetGuildInfoText) and the guild
// master's ranks (GuildControlPopupFrame: GuildControlSetRank / GetRankFlags / SetRankFlag / SaveRank /
// AddRank / DelRank). The roster itself, the MOTD, invitations and promotions are game::Social's; leader and
// disband are game::PlayerCommands'. Packets as vmangos-core 4641790 handles them (net/GuildMessages). Main
// thread; storage reused (no allocation per frame once warmed up).
class GuildControl {
public:
    explicit GuildControl(const Social& social);

    void setSink(PacketSink sink) { m_sink = sink; }
    // The names the zone and class keys compare (AreaTable, ChrClasses: the screens' tables); without them
    // those keys compare the ids.
    using NameOf = std::string_view (*)(const void* context, u32 id);
    void setNames(NameOf zone, NameOf cls, const void* context) {
        m_zoneName = zone;
        m_className = cls;
        m_namesContext = context;
        m_sortChanged = true;
    }
    // `newLogin`: another character (the sort keys and the selection start over).
    void enterWorld(u64 player, bool newLogin);
    void leaveWorld();

    // --- The roster ---
    // GetGuildRosterInfo(1 .. GetNumGuildMembers()): indices into Social::guild().members, the online members
    // only — 1.12.1's roster never shows the offline ones (GetNumGuildMembers counts them only with
    // SetGuildRosterShowOffline, whose "Show Offline Members" check box is a virtual template
    // FriendsFrame.xml never creates) — sorted by the key list (WoW.exe 0x4D0D50). Rebuilt when the roster or
    // the sort changes.
    std::span<const u32> roster();
    // SortGuildRoster(key): the key goes first in the list; the first one again turns its order round.
    void sort(GuildSort key);
    GuildSort sortKey(u32 position) const { return m_keys[position]; }
    bool sortReversed(u32 position) const { return m_reversed[position]; }
    // SetGuildRosterSelection: the member by name (kept across roster refreshes); empty clears it.
    void select(std::string_view name);
    std::string_view selection() const { return {m_selection.data(), m_selectionLength}; }
    // The selected member (nullptr: none, or no longer listed).
    const GuildMember* selected() const;

    // --- Rights ---
    bool inGuild() const;
    bool isGuildLeader() const; // IsGuildLeader: rank 0
    // The player's rank has the right bit (guildbit::*; false before the roster tells the ranks' rights).
    bool hasRight(u32 bit) const;
    // GuildStatus_Update's rules for the member's buttons and notes (the player's rank against the member's).
    GuildMemberRights memberRights(const GuildMember& member) const;

    // --- Notes and the information text ---
    bool setPublicNote(std::string_view member, std::string_view note);  // CMSG_GUILD_SET_PUBLIC_NOTE
    bool setOfficerNote(std::string_view member, std::string_view note); // CMSG_GUILD_SET_OFFICER_NOTE
    // SetGuildInfoText: CMSG_GUILD_INFO_TEXT, then the roster again (GuildInfoSaveButton's GuildRoster(): the
    // server keeps the text without telling anyone).
    bool setInfoText(std::string_view text);

    // --- Ranks (the guild master's GuildControlPopupFrame) ---
    // GuildControlGetNumRanks: the ranks the roster's rights list has (the names' count before it came).
    u32 rankCount() const;
    std::string_view rankName(u32 rank) const;
    u32 rankRights(u32 rank) const; // 0 unknown
    // GuildControlGetRankFlags: the check boxes of `rank` (a right bit set).
    std::array<bool, kGuildControlOptions> rankFlags(u32 rank) const;
    // GuildControlSaveRank: the rank's rights with each check box's bit set or cleared (the others kept, the
    // 0x40 "empty" right with them), and its new name (1..15 letters): CMSG_GUILD_RANK. False when not sent.
    bool saveRank(u32 rank, std::string_view name, const std::array<bool, kGuildControlOptions>& flags);
    bool addRank(std::string_view name); // GuildControlAddRank: below 10 ranks, CMSG_GUILD_ADD_RANK
    bool deleteLastRank(); // GuildControlDelRank: above 5 ranks, CMSG_GUILD_DEL_RANK (the lowest)
    // GuildControlPopupFrameRemoveRankButton_OnUpdate: the lowest rank is chosen, there are more than five
    // and nobody holds it (FriendsFrame.playersInBotRank, counted among the listed members).
    bool canDeleteRank(u32 rank) const;
    u32 membersInRank(u32 rank) const;

    // Pure rules (unit tested): GuildStatus_Update's.
    static bool canPromote(u32 playerRank, u32 memberRank, bool right);
    static bool canDemote(u32 playerRank, u32 memberRank, u32 lowestRank, bool right);
    static bool canRemove(u32 playerRank, u32 memberRank, bool right);
    // The key list's comparison of two members (< 0: a first), WoW.exe 0x4D0D50: online members first, then
    // the keys in order, each turned round when reversed.
    i32 compare(const GuildMember& a, const GuildMember& b) const;

private:
    bool sendBody(net::Opcode opcode);
    const GuildState& guild() const;

    const Social& m_social;
    PacketSink m_sink;
    NameOf m_zoneName = nullptr;
    NameOf m_className = nullptr;
    const void* m_namesContext = nullptr;
    std::vector<u8> m_body;
    bool m_inWorld = false;
    u64 m_player = 0;

    std::array<GuildSort, kGuildSortKeys> m_keys{};
    std::array<bool, kGuildSortKeys> m_reversed{};
    std::vector<u32> m_roster;
    u32 m_rosterVersion = ~0u; // Social's roster version the list was built for
    bool m_sortChanged = true;
    std::array<char, 16> m_selection{}; // a member's name (12 letters at most)
    u32 m_selectionLength = 0;
};

} // namespace vd::game
