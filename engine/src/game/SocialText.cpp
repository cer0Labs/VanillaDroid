#include "game/SocialText.h"

#include "core/Text.h"
#include "game/ChatLinks.h"
#include "game/SocialData.h"
#include "net/ResponseCodes.gen.h"

#include <algorithm>
#include <charconv>
#include <cstring>

namespace vd::game {
namespace {

using net::ChatType;

// Appends `s` to the NUL-terminated text of `out` (`used` characters so far), cut to fit.
void put(std::span<char> out, usize& used, std::string_view s) {
    if (out.empty() || used + 1 >= out.size()) {
        return;
    }
    const usize n = std::min(s.size(), out.size() - 1 - used);
    std::memcpy(out.data() + used, s.data(), n);
    used += n;
    out[used] = 0;
}

const SocialData* g_channelNames = nullptr; // phase 21: setChannelNames

// A channel's name as ChatFrame shows it before a line: without its zone ("2. Trade - City" -> "2. Trade",
// gsub(arg4, "%s%-%s.*", "")), in the player's language (phase 21). Into `out`.
std::string_view withoutZone(std::string_view channel, std::span<char> out) {
    return channelDisplayBase(channel, out);
}

// The length of the "2. " a channel's name may start with.
usize numberPrefix(std::string_view channel) {
    usize digits = 0;
    while (digits < channel.size() && channel[digits] >= '0' && channel[digits] <= '9') {
        ++digits;
    }
    return digits > 0 && channel.substr(digits, 2) == ". " ? digits + 2 : 0;
}

// The CHAT_<type>_GET string of a message: what comes before its text, with the sender in its %s.
const char* chatGetKey(ChatType type) {
    switch (type) {
    case ChatType::Say: return "CHAT_SAY_GET";
    case ChatType::Yell: return "CHAT_YELL_GET";
    case ChatType::Whisper: return "CHAT_WHISPER_GET";
    case ChatType::WhisperInform: return "CHAT_WHISPER_INFORM_GET";
    case ChatType::Emote: return "CHAT_EMOTE_GET";
    case ChatType::Party: return "CHAT_PARTY_GET";
    case ChatType::Raid: return "CHAT_RAID_GET";
    case ChatType::RaidLeader: return "CHAT_RAID_LEADER_GET";
    case ChatType::RaidWarning: return "CHAT_RAID_WARNING_GET";
    case ChatType::Guild: return "CHAT_GUILD_GET";
    case ChatType::Officer: return "CHAT_OFFICER_GET";
    case ChatType::Battleground: return "CHAT_BATTLEGROUND_GET";
    case ChatType::BattlegroundLeader: return "CHAT_BATTLEGROUND_LEADER_GET";
    case ChatType::Channel: return "CHAT_CHANNEL_GET";
    case ChatType::Afk: return "CHAT_AFK_GET";
    case ChatType::Dnd: return "CHAT_DND_GET";
    case ChatType::MonsterSay: return "CHAT_MONSTER_SAY_GET";
    case ChatType::MonsterYell: return "CHAT_MONSTER_YELL_GET";
    case ChatType::MonsterWhisper:
    case ChatType::RaidBossWhisper: return "CHAT_MONSTER_WHISPER_GET";
    default: return nullptr;
    }
}

const char* channelNoticeKey(net::ChannelNotice notice) {
    using N = net::ChannelNotice;
    switch (notice) {
    case N::Joined: return "CHAT_CHANNEL_JOIN_GET";
    case N::Left: return "CHAT_CHANNEL_LEAVE_GET";
    case N::YouJoined: return "CHAT_YOU_JOINED_NOTICE";
    case N::YouLeft: return "CHAT_YOU_LEFT_NOTICE";
    case N::WrongPassword: return "CHAT_WRONG_PASSWORD_NOTICE";
    case N::NotMember: return "CHAT_NOT_MEMBER_NOTICE";
    case N::NotModerator: return "CHAT_NOT_MODERATOR_NOTICE";
    case N::PasswordChanged: return "CHAT_PASSWORD_CHANGED_NOTICE";
    case N::OwnerChanged: return "CHAT_OWNER_CHANGED_NOTICE";
    case N::PlayerNotFound: return "CHAT_PLAYER_NOT_FOUND_NOTICE";
    case N::NotOwner: return "CHAT_NOT_OWNER_NOTICE";
    case N::ChannelOwner: return "CHAT_CHANNEL_OWNER_NOTICE";
    case N::AnnouncementsOn: return "CHAT_ANNOUNCEMENTS_ON_NOTICE";
    case N::AnnouncementsOff: return "CHAT_ANNOUNCEMENTS_OFF_NOTICE";
    case N::ModerationOn: return "CHAT_MODERATION_ON_NOTICE";
    case N::ModerationOff: return "CHAT_MODERATION_OFF_NOTICE";
    case N::Muted: return "CHAT_MUTED_NOTICE";
    case N::PlayerKicked: return "CHAT_PLAYER_KICKED_NOTICE";
    case N::Banned: return "CHAT_BANNED_NOTICE";
    case N::PlayerBanned: return "CHAT_PLAYER_BANNED_NOTICE";
    case N::PlayerUnbanned: return "CHAT_PLAYER_UNBANNED_NOTICE";
    case N::PlayerNotBanned: return "CHAT_PLAYER_NOT_BANNED_NOTICE";
    case N::PlayerAlreadyMember: return "CHAT_PLAYER_ALREADY_MEMBER_NOTICE";
    case N::Invite: return "CHAT_INVITE_NOTICE";
    case N::InviteWrongFaction: return "CHAT_INVITE_WRONG_FACTION_NOTICE";
    case N::WrongFaction: return "CHAT_WRONG_FACTION_NOTICE";
    case N::InvalidName: return "CHAT_INVALID_NAME_NOTICE";
    case N::NotModerated: return "CHAT_NOT_MODERATED_NOTICE";
    case N::PlayerInvited: return "CHAT_PLAYER_INVITED_NOTICE";
    case N::PlayerInviteBanned: return "CHAT_PLAYER_INVITE_BANNED_NOTICE";
    case N::Throttled: return "CHAT_THROTTLED_NOTICE";
    default: return nullptr; // ModeChange: the client prints nothing for it
    }
}

} // namespace

const char* chatTypeKey(ChatType type) {
    switch (type) {
    case ChatType::Say: return "CHAT_MSG_SAY";
    case ChatType::Yell: return "CHAT_MSG_YELL";
    case ChatType::Party: return "CHAT_MSG_PARTY";
    case ChatType::Raid: return "CHAT_MSG_RAID";
    case ChatType::RaidWarning: return "CHAT_MSG_RAID_WARNING";
    case ChatType::Guild: return "CHAT_MSG_GUILD";
    case ChatType::Officer: return "CHAT_MSG_OFFICER";
    case ChatType::Emote: return "CHAT_MSG_EMOTE";
    case ChatType::Whisper: return "CHAT_MSG_WHISPER_INFORM"; // "Whisper": the one the player sends
    default: return nullptr;
    }
}

const char* lootMethodKey(u8 method) {
    switch (static_cast<net::LootMethod>(method)) {
    case net::LootMethod::FreeForAll: return "LOOT_FREE_FOR_ALL";
    case net::LootMethod::RoundRobin: return "LOOT_ROUND_ROBIN";
    case net::LootMethod::MasterLoot: return "LOOT_MASTER_LOOTER";
    case net::LootMethod::GroupLoot: return "LOOT_GROUP_LOOT";
    case net::LootMethod::NeedBeforeGreed: return "LOOT_NEED_BEFORE_GREED";
    default: return nullptr;
    }
}

const char* lootMethodChangedKey(u8 method) {
    switch (static_cast<net::LootMethod>(method)) {
    case net::LootMethod::FreeForAll: return "ERR_SET_LOOT_FREEFORALL";
    case net::LootMethod::RoundRobin: return "ERR_SET_LOOT_ROUNDROBIN";
    case net::LootMethod::MasterLoot: return "ERR_SET_LOOT_MASTER";
    case net::LootMethod::GroupLoot: return "ERR_SET_LOOT_GROUP";
    case net::LootMethod::NeedBeforeGreed: return "ERR_SET_LOOT_NBG";
    default: return nullptr;
    }
}

const char* itemQualityKey(u8 quality) {
    static constexpr const char* kKeys[] = {"ITEM_QUALITY0_DESC", "ITEM_QUALITY1_DESC", "ITEM_QUALITY2_DESC",
                                            "ITEM_QUALITY3_DESC", "ITEM_QUALITY4_DESC", "ITEM_QUALITY5_DESC",
                                            "ITEM_QUALITY6_DESC"};
    return quality < std::size(kKeys) ? kKeys[quality] : nullptr;
}

std::string_view itemQualityColor(u8 quality) {
    static constexpr std::string_view kColors[] = {"|cff9d9d9d", "|cffffffff", "|cff1eff00", "|cff0070dd",
                                                   "|cffa335ee", "|cffff8000", "|cffe6cc80"};
    return quality < std::size(kColors) ? kColors[quality] : kColors[1];
}

const char* partyResultKey(net::PartyResult result) {
    using R = net::PartyResult;
    switch (result) {
    case R::Ok: return nullptr;
    case R::BadPlayerName: return "ERR_BAD_PLAYER_NAME_S";
    case R::TargetNotInGroup: return "ERR_TARGET_NOT_IN_GROUP_S";
    case R::GroupFull: return "ERR_GROUP_FULL";
    case R::AlreadyInGroup: return "ERR_ALREADY_IN_GROUP_S";
    case R::NotInGroup: return "ERR_NOT_IN_GROUP";
    case R::NotLeader: return "ERR_NOT_LEADER";
    case R::WrongFaction: return "ERR_PLAYER_WRONG_FACTION";
    case R::IgnoringYou: return "ERR_IGNORING_YOU_S";
    default: return "VD_SOCIAL_GROUP_FAILED";
    }
}

const char* friendResultKey(net::FriendResult result) {
    using R = net::FriendResult;
    switch (result) {
    case R::DbError: return "ERR_FRIEND_DB_ERROR";
    case R::ListFull: return "ERR_FRIEND_LIST_FULL";
    case R::Online: return "ERR_FRIEND_ONLINE_SS"; // the name twice: its link and its text
    case R::Offline: return "ERR_FRIEND_OFFLINE_S";
    case R::NotFound: return "ERR_FRIEND_NOT_FOUND";
    case R::Removed: return "ERR_FRIEND_REMOVED_S";
    case R::AddedOnline:
    case R::AddedOffline: return "ERR_FRIEND_ADDED_S";
    case R::Already: return "ERR_FRIEND_ALREADY_S";
    case R::Self: return "ERR_FRIEND_SELF";
    case R::Enemy: return "ERR_FRIEND_WRONG_FACTION";
    case R::IgnoreFull: return "ERR_IGNORE_FULL";
    case R::IgnoreSelf: return "ERR_IGNORE_SELF";
    case R::IgnoreNotFound: return "ERR_IGNORE_NOT_FOUND";
    case R::IgnoreAlready: return "ERR_IGNORE_ALREADY_S";
    case R::IgnoreAdded: return "ERR_IGNORE_ADDED_S";
    case R::IgnoreRemoved: return "ERR_IGNORE_REMOVED_S";
    case R::IgnoreAmbiguous: return "ERR_IGNORE_AMBIGUOUS";
    default: return "ERR_FRIEND_ERROR";
    }
}

const char* guildErrorKey(net::GuildError error, u32 command) {
    using E = net::GuildError;
    switch (error) {
    case E::Ok: return nullptr;
    case E::Internal: return "ERR_GUILD_INTERNAL";
    case E::AlreadyInGuild: return "ERR_ALREADY_IN_GUILD";
    case E::AlreadyInGuildS: return "ERR_ALREADY_IN_GUILD_S";
    case E::InvitedToGuild: return "ERR_INVITED_TO_GUILD";
    case E::AlreadyInvitedS: return "ERR_ALREADY_INVITED_TO_GUILD_S";
    case E::NameInvalid: return "ERR_GUILD_NAME_INVALID";
    case E::NameExistsS: return "ERR_GUILD_NAME_EXISTS_S";
    case E::Permissions: // the guild master cannot quit without a successor
        return command == net::kGuildCommandQuit ? "ERR_GUILD_LEADER_LEAVE" : "ERR_GUILD_PERMISSIONS";
    case E::PlayerNotInGuild: return "ERR_GUILD_PLAYER_NOT_IN_GUILD";
    case E::PlayerNotInGuildS: return "ERR_GUILD_PLAYER_NOT_IN_GUILD_S";
    case E::PlayerNotFoundS: return "ERR_GUILD_PLAYER_NOT_FOUND_S";
    case E::NotAllied: return "GUILD_NOT_ALLIED_S";
    case E::RankTooHighS: return "ERR_GUILD_RANK_TOO_HIGH_S";
    case E::RankTooLowS: return "ERR_GUILD_RANK_TOO_LOW_S";
    case E::RanksLocked: return "ERR_GUILD_RANKS_LOCKED";
    case E::RankInUse: return "ERR_GUILD_RANK_IN_USE";
    case E::IgnoringYouS: return "ERR_IGNORING_YOU_S";
    default: return "ERR_GUILD_INTERNAL";
    }
}

const char* guildEventKey(net::GuildEventKind kind) {
    using K = net::GuildEventKind;
    switch (kind) {
    case K::Promotion: return "ERR_GUILD_PROMOTE_SSS";
    case K::Demotion: return "ERR_GUILD_DEMOTE_SSS";
    case K::Motd: return "GUILD_MOTD_TEMPLATE";
    case K::Joined: return "ERR_GUILD_JOIN_S";
    case K::Left: return "ERR_GUILD_LEAVE_S";
    case K::Removed: return "ERR_GUILD_REMOVE_SS";
    case K::LeaderIs: return "ERR_GUILD_LEADER_IS_S";
    case K::LeaderChanged: return "ERR_GUILD_LEADER_CHANGED_SS";
    case K::Disbanded: return "ERR_GUILD_DISBANDED";
    case K::SignedOn: return "ERR_FRIEND_ONLINE_SS"; // members sign on as friends do: the name twice
    case K::SignedOff: return "ERR_FRIEND_OFFLINE_S";
    default: return nullptr;
    }
}

std::string_view charResultText(u8 code) {
    using RC = net::ResponseCode;
    switch (static_cast<RC>(code)) {
    case RC::CHAR_NAME_SUCCESS: return {};
    case RC::CHAR_NAME_CONSECUTIVE_SPACES: return text::get("CHAR_NAME_INVALID_SPACE"); // no text of its own
    default: {
        const char* name = net::responseCodeName(code);
        return text::has(name) ? text::get(name) : text::get("CHAR_CREATE_UNKNOWN");
    }
    }
}

const char* customizationKey(AppearanceOption option, FeatureKind hair, FeatureKind facialHair) {
    const bool horns = hair == FeatureKind::Horns;
    switch (option) {
    case AppearanceOption::Skin: return "CHAR_CUSTOMIZATION1_DESC";
    case AppearanceOption::Face: return "CHAR_CUSTOMIZATION2_DESC";
    case AppearanceOption::HairStyle: return horns ? "HAIR_HORNS_STYLE" : "HAIR_NORMAL_STYLE";
    case AppearanceOption::HairColor: return horns ? "HAIR_HORNS_COLOR" : "HAIR_NORMAL_COLOR";
    case AppearanceOption::FacialHair:
        switch (facialHair) {
        case FeatureKind::Piercings: return "FACIAL_HAIR_PIERCINGS";
        case FeatureKind::Markings: return "FACIAL_HAIR_MARKINGS";
        case FeatureKind::Features: return "FACIAL_HAIR_FEATURES";
        case FeatureKind::Hair: return "FACIAL_HAIR_HAIR";
        case FeatureKind::Tusks: return "FACIAL_HAIR_TUSKS";
        case FeatureKind::Earrings: return "FACIAL_HAIR_EARRINGS";
        case FeatureKind::Horns: return "HAIR_HORNS_STYLE"; // no FACIAL_HAIR_HORNS in the client
        default: return "FACIAL_HAIR_NORMAL";
        }
    default: return nullptr;
    }
}

void appendChatLine(std::string& out, const ChatLine& line, std::string_view sender, u32 channelNumber) {
    switch (line.source) {
    case ChatSource::PlayerNotFound: text::append(out, "ERR_CHAT_PLAYER_NOT_FOUND_S", line.text); return;
    case ChatSource::WrongFaction: text::append(out, "ERR_CHAT_WRONG_FACTION"); return;
    case ChatSource::Restricted: text::append(out, "ERR_CHAT_RESTRICTED"); return;
    case ChatSource::Notification:
    case ChatSource::AreaTrigger:
    case ChatSource::ServerMessage: out.append(line.text); return; // the server's own text
    default: break;
    }
    switch (line.type) {
    case ChatType::MonsterEmote:
    case ChatType::RaidBossEmote: // the creature's name in the text's %s (CHAT_MONSTER_EMOTE_GET is empty)
        appendTextEmote(out, line.text, sender, {});
        return;
    case ChatType::Ignored: text::append(out, "CHAT_IGNORED", sender); return;
    case ChatType::Channel: {
        // "[2. Trade] " before the sender, as ChatFrame adds it (without the channel's zone).
        out.push_back('[');
        if (channelNumber != 0) {
            char number[16];
            const std::to_chars_result r = std::to_chars(number, number + sizeof(number), channelNumber);
            out.append(number, r.ptr);
            out.append(". ");
        }
        char shown[128];
        out.append(withoutZone(line.channel, shown));
        out.append("] ");
        break;
    }
    default: break;
    }
    const char* key = chatGetKey(line.type);
    if (line.type == ChatType::BgSystemNeutral || line.type == ChatType::BgSystemAlliance ||
        line.type == ChatType::BgSystemHorde) {
        // Phase 17 (queue): the text alone, its $n the sender's name ("The Horde flag was picked up by $n!":
        // WoW.exe 0x49D961 expands it with 0x506F70 before CHAT_MSG_BG_SYSTEM_*).
        const usize from = out.size();
        appendChatText(out, line.text);
        for (usize at = out.find('$', from); at != std::string::npos; at = out.find('$', at)) {
            if (at + 1 < out.size() && (out[at + 1] == 'n' || out[at + 1] == 'N')) {
                out.replace(at, 2, sender);
                at += sender.size();
            } else {
                ++at;
            }
        }
        return;
    }
    if (!key) {
        appendChatText(out, line.text); // system lines and the rest: the text alone
        return;
    }
    // The sender as ChatFrame writes it: players linked in brackets after their flag ("|Hplayer:Aeryn|h[Aeryn]|h",
    // what a tap on the name opens), creatures and emotes plain.
    char name[128];
    usize used = 0;
    name[0] = 0;
    const bool creature = line.type == ChatType::MonsterSay || line.type == ChatType::MonsterYell ||
                          line.type == ChatType::MonsterWhisper || line.type == ChatType::RaidBossWhisper;
    if (!creature) {
        put(name, used, line.tag == 1   ? text::get("CHAT_FLAG_AFK")
                        : line.tag == 2 ? text::get("CHAT_FLAG_DND")
                        : line.tag == 3 ? text::get("CHAT_FLAG_GM")
                                        : std::string_view{});
    }
    if (!creature && line.type != ChatType::Emote && !sender.empty()) {
        put(name, used, "|Hplayer:");
        put(name, used, sender);
        put(name, used, "|h[");
        put(name, used, sender);
        put(name, used, "]|h");
    } else {
        put(name, used, sender);
    }
    text::append(out, key, std::string_view(name, used));
    if (!line.languageName.empty()) { // phase 18 (extras, B4): ChatFrame's languageHeader, "[Orcish] "
        out.push_back('[');
        out.append(line.languageName);
        out.append("] ");
    }
    appendChatText(out, line.text);
}

void appendTextEmote(std::string& out, std::string_view line, std::string_view first, std::string_view second) {
    if (line.empty()) {
        text::append(out, "CHAT_EMOTE_GET", first);
        out.append(text::get("CHAT_EMOTE_UNKNOWN"));
        return;
    }
    // Not a format of ours: only its %s are replaced, by hand (the text comes from data).
    u32 arg = 0;
    for (usize i = 0; i < line.size(); ++i) {
        if (line[i] == '%' && i + 1 < line.size() && line[i + 1] == 's') {
            out.append(arg++ == 0 ? first : second);
            ++i;
        } else {
            out.push_back(line[i]);
        }
    }
}

void setChannelNames(const SocialData* data) {
    g_channelNames = data;
}

std::string_view channelDisplayName(std::string_view channel, std::span<char> out) {
    return g_channelNames ? g_channelNames->displayChannel(channel, out) : channel;
}

std::string_view channelDisplayBase(std::string_view channel, std::span<char> out) {
    if (g_channelNames) {
        return g_channelNames->displayChannelBase(channel, out);
    }
    const usize dash = channel.find(" - ");
    return dash == std::string_view::npos ? channel : channel.substr(0, dash);
}

std::string_view formatChannelNotice(std::span<char> out, net::ChannelNotice notice, std::string_view channel,
                                     std::string_view player, std::string_view by) {
    const char* key = channelNoticeKey(notice);
    if (out.empty()) {
        return {};
    }
    out[0] = 0;
    if (!key) {
        return {};
    }
    usize used = 0;
    if (notice == net::ChannelNotice::Joined || notice == net::ChannelNotice::Left) {
        // CHAT_CHANNEL_JOIN_GET names only the player: the channel goes before it, as ChatFrame adds it (without
        // its zone).
        put(out, used, "[");
        const usize prefix = numberPrefix(channel);
        put(out, used, channel.substr(0, prefix));
        char shown[128];
        put(out, used, withoutZone(channel.substr(prefix), shown));
        put(out, used, "] ");
        const std::string_view rest = text::format(out.subspan(used), key, player);
        return {out.data(), used + rest.size()};
    }
    char shown[128]; // phase 21: in the player's language
    return text::format(out, key, channelDisplayName(channel, shown), player, by);
}

} // namespace vd::game
