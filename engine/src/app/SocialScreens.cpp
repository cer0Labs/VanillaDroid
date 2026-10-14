#include "app/SocialScreens.h"

#include "app/AuraScreens.h"
#include "app/ChatLinkInsert.h"
#include "app/CombatScreens.h"
#include "app/DialogKit.h"
#include "app/GameSession.h"
#include "app/GuildScreens.h"
#include "app/Portraits.h"
#include "app/PopupScreens.h"
#include "app/UiResources.h"
#include "app/UnitMenuScreens.h"
#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/ChatLinks.h"
#include "game/GameData.h"
#include "game/Minimap.h"
#include "game/Notices.h"
#include "game/SocialData.h"
#include "game/SocialText.h"
#include "game/UnitInfo.h"
#include "game/Languages.h" // phase 18 (extras)
#include "net/ResponseCodes.gen.h"
#include "net/UpdateFields.gen.h"
#include "ui/ChatDock.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace vd {
namespace {

namespace uf = net::uf;
using game::Entity;
using game::SocialEventKind;
using net::ChatType;
using ui::rgba;

constexpr u32 kErrorColor = rgba(255, 26, 26);
constexpr u32 kSystemColor = rgba(255, 255, 0);
constexpr u32 kGuildColor = rgba(64, 255, 64);
constexpr u32 kPartyColor = rgba(170, 170, 255);
constexpr u32 kWhisperColor = rgba(255, 128, 255);
constexpr u32 kChannelColor = rgba(255, 192, 192);
constexpr u32 kDuelColor = rgba(255, 209, 0);
// Phase 15 (options): PartyMemberBackground's backdrop (PartyFrame.xml).
constexpr u64 kPartyBackground = assetPathHash("Interface\\CharacterFrame\\UI-Party-Background.blp");
constexpr u64 kPartyBorder = assetPathHash("Interface\\CharacterFrame\\UI-Party-Border.blp");
constexpr u64 kChatTextReserve = 256;

// Line colors of the client's default chat settings (ChatTypeInfo).
u32 chatColor(ChatType type) {
    switch (type) {
    case ChatType::Say:
    case ChatType::MonsterSay: return rgba(255, 255, 255);
    case ChatType::Yell:
    case ChatType::MonsterYell: return rgba(255, 64, 64);
    case ChatType::Whisper:
    case ChatType::WhisperInform:
    case ChatType::MonsterWhisper:
    case ChatType::Afk:
    case ChatType::Dnd: return kWhisperColor;
    case ChatType::Party: return kPartyColor;
    case ChatType::Raid: return rgba(255, 127, 0);
    // Phase 16 (alerts): WoW.exe's own defaults for these (its colour table, 0x805DD5..0x805E9E), not later clients'.
    case ChatType::RaidLeader:
    case ChatType::RaidWarning:
    case ChatType::RaidBossEmote: return rgba(255, 219, 183);
    case ChatType::Guild: return kGuildColor;
    case ChatType::Officer: return rgba(64, 192, 64);
    // Phase 17 (queue): WoW.exe's defaults too (the same table: BG_SYSTEM_NEUTRAL / _ALLIANCE / _HORDE at
    // 0x805C86, 0x805CC9, 0x805D0C, BATTLEGROUND / _LEADER at 0x805F24, 0x805F67: 64 bytes of name, RGB).
    case ChatType::Battleground: return rgba(255, 127, 0);
    case ChatType::BattlegroundLeader: return rgba(255, 219, 183);
    case ChatType::BgSystemNeutral: return rgba(255, 120, 10);
    case ChatType::BgSystemAlliance: return rgba(0, 174, 239);
    case ChatType::BgSystemHorde: return rgba(255, 0, 0);
    case ChatType::Emote:
    case ChatType::TextEmote:
    case ChatType::MonsterEmote: return rgba(255, 128, 64);
    case ChatType::Channel:
    case ChatType::ChannelJoin:
    case ChatType::ChannelLeave:
    case ChatType::ChannelNotice:
    case ChatType::ChannelNoticeUser: return kChannelColor;
    default: return kSystemColor;
    }
}

// Parity: the chat frames docked together (FloatingChatFrame.lua's dock). Phase 18 (p3): the windows of the
// character's chat-cache.txt (game::ChatWindows): ChatFrame1 "General" and ChatFrame2 "Combat Log" by default, the
// player's own after (FCF_OpenNewWindow), each showing the message groups and channels it listens to; the combat log's
// lines (app/CombatScreens) show in the windows with a combat group.
constexpr u8 kDockGeneral = 0;
constexpr u32 kNoEntry = 0xFFFFFFFFu;    // a view line with no chat entry behind it (the combat log's)
constexpr u32 kLogKey = 0x80000000u;     // the combat log's line keys (the chat's never reach it)
constexpr u32 kChatActiveMs = 10000;     // how long a touch keeps the background (the mouse-over)

// Phase 18 (p3): the message group of a line (ChatTypeGroup: CHAT_MSG_<type> -> its group), or the channel's own.
u8 chatGroup(const game::ChatLine& line, u8 channelMessage) {
    if (line.source != game::ChatSource::Message && line.source != game::ChatSource::TextEmote) {
        return game::kGroupSystem;
    }
    switch (line.type) {
    case ChatType::Say:
    case ChatType::Emote:
    case ChatType::TextEmote: return game::kGroupSay;
    case ChatType::Yell: return game::kGroupYell;
    case ChatType::Whisper:
    case ChatType::WhisperInform: return game::kGroupWhisper;
    case ChatType::Party:
    case ChatType::Raid:
    case ChatType::RaidLeader:
    case ChatType::RaidWarning:
    case ChatType::Battleground:
    case ChatType::BattlegroundLeader: return game::kGroupParty;
    case ChatType::Guild:
    case ChatType::Officer: return game::kGroupGuild;
    case ChatType::MonsterSay:
    case ChatType::MonsterYell:
    case ChatType::MonsterEmote:
    case ChatType::MonsterWhisper:
    case ChatType::RaidBossWhisper:
    case ChatType::RaidBossEmote: return game::kGroupCreature;
    case ChatType::Channel: return channelMessage;
    case ChatType::ChannelJoin:
    case ChatType::ChannelLeave:
    case ChatType::ChannelNotice:
    case ChatType::ChannelNoticeUser: return game::kGroupChannel;
    case ChatType::Skill: return game::kGroupSkill;
    case ChatType::Loot: return game::kGroupLoot;
    default: return game::kGroupSystem;
    }
}

std::string_view baseChannel(std::string_view name) {
    const usize dash = name.find(" - ");
    return dash == std::string_view::npos ? name : name.substr(0, dash);
}

// The chat category of a line (what each docked frame shows).
u32 chatCategory(const game::ChatLine& line) {
    if (line.source != game::ChatSource::Message && line.source != game::ChatSource::TextEmote) {
        return 1u << 5; // system
    }
    switch (line.type) {
    case ChatType::Say:
    case ChatType::Yell:
    case ChatType::MonsterSay:
    case ChatType::MonsterYell: return 1u << 0;
    case ChatType::Emote:
    case ChatType::TextEmote:
    case ChatType::MonsterEmote: return (1u << 0) | (1u << 6);
    case ChatType::Whisper:
    case ChatType::WhisperInform:
    case ChatType::MonsterWhisper:
    case ChatType::RaidBossWhisper:
    case ChatType::Afk:
    case ChatType::Dnd:
    case ChatType::Ignored: return 1u << 1;
    case ChatType::Party:
    case ChatType::Raid:
    case ChatType::RaidLeader:
    case ChatType::RaidWarning:
    case ChatType::Battleground: // phase 17 (queue): ChatTypeGroup["PARTY"] has them too
    case ChatType::BattlegroundLeader: return 1u << 2;
    case ChatType::Guild:
    case ChatType::Officer: return 1u << 3;
    case ChatType::Channel: return 1u << 4;
    default: return 1u << 5;
    }
}

// The emote menu: the most used, with the slash command they are (EmotesText tokens) and their labels' keys.
struct EmoteDef {
    const char* key;
    std::string_view token;
};
constexpr EmoteDef kEmotes[] = {
    {"VD_SOCIAL_EMOTE_WAVE", "wave"},       {"VD_SOCIAL_EMOTE_HELLO", "hello"},
    {"VD_SOCIAL_EMOTE_BOW", "bow"},         {"VD_SOCIAL_EMOTE_DANCE", "dance"},
    {"VD_SOCIAL_EMOTE_CHEER", "cheer"},     {"VD_SOCIAL_EMOTE_LAUGH", "laugh"},
    {"VD_SOCIAL_EMOTE_CLAP", "clap"},       {"VD_SOCIAL_EMOTE_THANK", "thank"},
    {"VD_SOCIAL_EMOTE_NOD", "nod"},         {"VD_SOCIAL_EMOTE_NO", "no"},
    {"VD_SOCIAL_EMOTE_CRY", "cry"},         {"VD_SOCIAL_EMOTE_KNEEL", "kneel"},
    {"VD_SOCIAL_EMOTE_SIT", "sit"},         {"VD_SOCIAL_EMOTE_SLEEP", "sleep"},
    {"VD_SOCIAL_EMOTE_POINT", "point"},     {"VD_SOCIAL_EMOTE_FLEX", "flex"},
    {"VD_SOCIAL_EMOTE_KISS", "kiss"},       {"VD_SOCIAL_EMOTE_HUG", "hug"},
    {"VD_SOCIAL_EMOTE_ROAR", "roar"},       {"VD_SOCIAL_EMOTE_SALUTE", "salute"},
    {"VD_SOCIAL_EMOTE_CHICKEN", "chicken"}, {"VD_SOCIAL_EMOTE_RUDE", "rude"},
    {"VD_SOCIAL_EMOTE_SHY", "shy"},         {"VD_SOCIAL_EMOTE_CHARGE", "charge"},
    {"VD_SOCIAL_EMOTE_FLEE", "flee"},       {"VD_SOCIAL_EMOTE_HEALME", "healme"},
    {"VD_SOCIAL_EMOTE_OOM", "oom"},         {"VD_SOCIAL_EMOTE_FOLLOW", "follow"},
    {"VD_SOCIAL_EMOTE_WAIT", "wait"},       {"VD_SOCIAL_EMOTE_INCOMING", "incoming"},
    {"VD_SOCIAL_EMOTE_ATTACKMYTARGET", "attackmytarget"}, {"VD_SOCIAL_EMOTE_TRAIN", "train"},
};

// Player names of the server's packets go with no capital checks: compare as the server does (case insensitive).
bool sameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        const char x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        const char y = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
        if (x != y) {
            return false;
        }
    }
    return true;
}

bool isPlayerGuid(u64 guid) {
    return guid != 0 && (guid >> 48) == 0;
}

// Code's own formats only (decoration: brackets, separators, numbers), never a text of the table.
void appendf(std::string& out, const char* format, ...) VD_PRINTF_FORMAT(2, 3);
void appendf(std::string& out, const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (n > 0) {
        out.append(buffer, static_cast<usize>(std::min<int>(n, sizeof(buffer) - 1)));
    }
}

// The parts one after the other into `out` (NUL-terminated, cut to fit).
std::string_view concat(std::span<char> out, std::initializer_list<std::string_view> parts) {
    usize used = 0;
    for (const std::string_view part : parts) {
        const usize n = std::min(part.size(), out.size() - 1 - used);
        std::memcpy(out.data() + used, part.data(), n);
        used += n;
    }
    out[used] = 0;
    return {out.data(), used};
}

// A client string meant to lead into something ("Tell %s: ", "Invite to guild: ") as a label: without its trailing
// colon and spaces.
std::string_view trimLabel(std::string_view s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == ':')) {
        s.remove_suffix(1);
    }
    return s;
}

// A part of a row's detail line, after a " · " (nothing when empty).
void appendPart(std::string& out, std::string_view part) {
    if (!part.empty()) {
        out.append(out.empty() ? "" : " · ");
        out.append(part);
    }
}

// An item quality's name in its color ("|cff1eff00Uncommon|r").
std::string_view coloredQuality(std::span<char> out, u8 quality) {
    const char* key = game::itemQualityKey(quality);
    return concat(out, {game::itemQualityColor(quality), key ? text::get(key) : std::string_view("?"), "|r"});
}

// How long ago a guild member was last online (the roster's LASTONLINE_* texts).
std::string_view lastOnline(std::span<char> out, f32 days) {
    const f32 hours = days * 24.0f;
    if (hours < 1.0f) {
        return text::format(out, "LASTONLINE_MINS");
    }
    const auto count = [&](f32 value, const char* one, const char* many) {
        const i32 n = std::max(1, static_cast<i32>(value));
        return text::format(out, n == 1 ? one : many, n);
    };
    if (hours < 24.0f) {
        return count(hours, "LASTONLINE_HOURS", "LASTONLINE_HOURS_P1");
    }
    if (days < 30.0f) {
        return count(days, "LASTONLINE_DAYS", "LASTONLINE_DAYS_P1");
    }
    if (days < 365.0f) {
        return count(days / 30.0f, "LASTONLINE_MONTHS", "LASTONLINE_MONTHS_P1");
    }
    return count(days / 365.0f, "LASTONLINE_YEARS", "LASTONLINE_YEARS_P1");
}

// Sheet actions.
enum SheetAction : u32 {
    kActWhisper,
    kActTarget,
    kActInvite,
    kActUninvite,
    kActPromote,
    kActAddFriend,
    kActRemoveFriend,
    kActIgnore,
    kActUnignore,
    kActDuel,
    kActGuildInvite,
    kActGuildPromote,
    kActGuildDemote,
    kActGuildRemove,
    kActEmote,
    kActTrade, // phase 8: game::Trade
};

} // namespace

template <typename... Args> void SocialScreens::systemLine(u32 color, text::Key key, const Args&... args) {
    ChatEntry& e = addLine(kCatSystem, color);
    text::append(e.text, key, args...);
    indexLinks(e); // ERR_FRIEND_ONLINE_SS links the name
}

void SocialScreens::systemText(u32 color, std::string_view text) {
    ChatEntry& e = addLine(kCatSystem, color);
    e.text.assign(text);
    indexLinks(e);
}

template <typename... Args> void SocialScreens::message(u32 color, bool big, text::Key key, const Args&... args) {
    char buffer[256];
    messageText(color, big, text::format(buffer, key, args...));
}

std::string_view SocialScreens::raceText(u32 race) const {
    return m_socialData ? m_socialData->raceName(race) : std::string_view{};
}

std::string_view SocialScreens::classText(u32 cls) const {
    return m_socialData ? m_socialData->className(cls) : std::string_view{};
}

void SocialScreens::init(const game::GameData* data, game::SocialData* socialData, const game::CharCreateData* create,
                         UiResources* resources) {
    m_data = data;
    m_socialData = socialData && socialData->ready() ? socialData : nullptr;
    m_resources = resources;
    m_trace = VD_DEV_ENV("VD_TRACE_GAMEPLAY") != nullptr;
    m_chatLines.resize(kChatLines);
    for (ChatEntry& e : m_chatLines) {
        e.text.reserve(kChatTextReserve);
        e.senderName.reserve(16);
        e.raw.text.reserve(kChatTextReserve);
        e.raw.channel.reserve(32);
        e.raw.senderName.reserve(32);
    }
    m_chatViews.reserve(256); // the combat log's lines too (CombatScreens keeps 256)
    m_chatViewEntries.reserve(256);
    // Development: VD_CHAT_VISIBLE_MS shortens the lines' two minutes (to see them fade); VD_HUD_DEMO fills the chat
    // with sample lines without a server.
    const char* visible = VD_DEV_ENV("VD_CHAT_VISIBLE_MS");
    m_visibleMs = visible ? static_cast<u32>(std::max(1000, std::atoi(visible))) : ui::kChatVisibleMs;
    m_chatDemo = VD_DEV_ENV("VD_HUD_DEMO") != nullptr;
    m_chatInput.reserve(256);
    m_outgoing.reserve(512); // phase 13: escapeChatInput's line (at most 255 bytes go)
    m_lastWhisper.reserve(16);
    m_whoRows.reserve(64); // retouches (friends frame): SMSG_WHO lists at most 50
    m_rowGuids.reserve(128);
    m_friendRows.reserve(128); // retouches (friends frame)
    m_rowOnline.reserve(128);
    m_rowNames.resize(128);
    m_rowDetails.resize(128);
    m_rowRights.resize(128);
    m_whoOrder.reserve(64); // phase 18 (p3): SMSG_WHO lists at most 50
    for (u32 i = 0; i < 128; ++i) {
        m_rowNames[i].reserve(16);
        m_rowDetails[i].reserve(64);
        m_rowRights[i].reserve(24);
    }
    m_subtitle.reserve(256);
    m_whoSelected.reserve(16);
    m_whoInput.reserve(64);
    m_sheetActions.reserve(16);
    m_sheetActionIds.reserve(16);
    m_choices.reserve(24);
    m_choiceTargets.reserve(24);
    m_choiceTexts.resize(24);
    m_partyViews.reserve(4);
    m_afkMessage.reserve(64);
    m_dndMessage.reserve(64);
    m_charCreate.init(create); // phase 18 (create); VD_CREATE_DEMO opens it without a server
}

void SocialScreens::reset() {
    m_chatNext = m_chatCount = 0;
    m_dock = kDockGeneral;
    m_flashSince = {};
    m_revealMs = m_activeUntilMs = 0;
    m_backgroundAlpha = 0.0f;
    m_chatExpanded = false;
    m_chatInput.clear();
    m_chatTarget = ChatTarget{};
    m_stickyTarget = ChatTarget{};
    m_lastWhisper.clear();
    m_focusInput = false; // phase 13
    m_modal = Modal::None;
    m_confirm = Confirm::None;
    m_messageCount = 0;
    m_duelShownSecond = 0;
    m_lootMethodSeen = m_lootThresholdSeen = -1;
    m_raidSeen = false; // phase 16 (raidframe)
    m_raid.hidden();
}

bool SocialScreens::popMessage(Message& out) {
    if (m_messageCount == 0) {
        return false;
    }
    out = m_messages[m_messageFirst];
    m_messageFirst = (m_messageFirst + 1) % m_messages.size();
    --m_messageCount;
    return true;
}

void SocialScreens::messageText(u32 color, bool big, std::string_view text) {
    if (m_messageCount == m_messages.size()) {
        m_messageFirst = (m_messageFirst + 1) % m_messages.size(); // the oldest goes
        --m_messageCount;
    }
    Message& m = m_messages[(m_messageFirst + m_messageCount++) % m_messages.size()];
    concat(m.text, {text});
    m.color = color;
    m.big = big;
}

// --- Character deletion ------------------------------------------------------------------------------------------

void SocialScreens::askDelete(u64 guid, std::string_view name) {
    m_deleteGuid = guid;
    m_deleteName.assign(name);
    m_waitingDelete = false;
}

bool SocialScreens::drawDeleteConfirm(GameSession& session, DialogContext& c) {
    if (m_deleteGuid == 0 && !m_waitingDelete) {
        return false;
    }
    if (session.phase() != GameSession::Phase::CharacterSelect) {
        m_deleteGuid = 0; // disconnected
        m_waitingDelete = false;
        return false;
    }
    if (m_waitingDelete) {
        if (session.charResultVersion() == m_resultVersion) {
            confirmDialog(c, text::get("CHAR_DELETE_IN_PROGRESS"), text::get("VD_SOCIAL_PLEASE_WAIT"), {});
            return true;
        }
        m_waitingDelete = false;
        const u8 code = session.charResult();
        if (code != static_cast<u8>(net::ResponseCode::CHAR_DELETE_SUCCESS)) {
            m_createStatus.assign(game::charResultText(code));
            m_deleteGuid = 1; // shows the error once more
            m_deleteName.clear();
        }
        return false;
    }
    if (m_deleteName.empty()) {
        if (confirmDialog(c, m_createStatus, text::get("OKAY"), {}).yes) {
            m_deleteGuid = 0;
        }
        return true;
    }
    // The original's question: the name, the level and the class.
    u32 level = 0;
    std::string_view cls;
    for (const net::CharacterInfo& ch : session.characters()) {
        if (ch.guid == m_deleteGuid) {
            level = ch.level;
            cls = classText(ch.cls);
        }
    }
    char question[256];
    const ConfirmResult r = confirmDialog(c, text::format(question, "CONFIRM_CHAR_DELETE", m_deleteName, level, cls),
                                          text::get("DELETE"), text::get("CANCEL"));
    if (r.yes) {
        m_resultVersion = session.charResultVersion();
        m_waitingDelete = session.deleteCharacter(m_deleteGuid);
        m_deleteGuid = 0;
        m_deleteName.clear();
    } else if (r.no) {
        m_deleteGuid = 0;
        m_deleteName.clear();
    }
    return true;
}

// --- Chat --------------------------------------------------------------------------------------------------------

SocialScreens::ChatEntry& SocialScreens::addLine(u32 category, u32 color, u8 group, std::string_view channel) {
    ChatEntry& e = m_chatLines[m_chatNext++ % kChatLines];
    e.group = group;
    e.channel.assign(channel);
    m_chatCount = std::min(m_chatCount + 1, kChatLines);
    e.text.clear();
    e.color = color;
    e.category = category;
    e.sender = 0;
    e.senderName.clear();
    e.nameMissing = false;
    e.raw.source = game::ChatSource::Notification;
    e.raw.sender = 0;
    e.timeMs = m_nowMs;
    e.key = ++m_lineKey & ~kLogKey;
    e.linkCount = 0;
    return e;
}

bool SocialScreens::shows(const game::ChatWindowConfig& window, const ChatEntry& e) const {
    // Phase 18 (p3): ChatFrame_OnEvent's filters: the frame's message groups, and for a channel's messages its
    // channel list (by the channel's name without its zone).
    return e.group == kChannelMessage ? window.hasChannel(e.channel) : window.hasGroup(e.group);
}

void SocialScreens::systemNotice(u32 color, std::string_view text, u8 group) {
    ChatEntry& e = addLine(kCatSystem, color, group);
    e.text.assign(text);
    indexLinks(e);
}

void SocialScreens::nameChatWindow(u32 chatFrame, std::string_view name) {
    // NAME_CHAT's OnAccept: FCF_SetWindowName on the window, or FCF_OpenNewWindow(name).
    if (chatFrame >= 1 && chatFrame <= game::kChatWindows) {
        m_chatWindows.setName(chatFrame - 1, name);
        m_chatWindows.markChanged();
    } else if (const i32 opened = m_chatWindows.openNew(name); opened >= 0) {
        m_dock = static_cast<u8>(opened); // FCF_DockFrame selects it
    }
}

void SocialScreens::indexLinks(ChatEntry& e) {
    e.linkCount = 0;
    game::ChatLink link;
    usize from = 0;
    while (e.linkCount < e.links.size() && e.text.size() <= 0xFFFF && game::nextChatLink(e.text, from, link)) {
        e.links[e.linkCount++] = {static_cast<u16>(link.begin), static_cast<u16>(link.end)};
        from = link.end;
    }
}

void SocialScreens::systemNotice(u32 color, std::string_view text) {
    systemText(color, text);
}

void SocialScreens::readChat(GameSession& session) {
    game::Gameplay& play = session.play();
    const game::Social& social = play.social();
    while (const game::ChatLine* line = play.chat().next(m_chatCursor)) {
        // Ignored players are not heard (the client filters them).
        if (line->sender != 0 && social.isIgnored(line->sender)) {
            continue;
        }
        const u32 category = chatCategory(*line);
        ChatEntry& entry = addLine(category, chatColor(line->type), chatGroup(*line, kChannelMessage),
                                   baseChannel(line->channel));
        entry.raw = *line;
        formatLine(session, entry);
        if (line->type == ChatType::Whisper && isPlayerGuid(line->sender) && line->source == game::ChatSource::Message) {
            if (!entry.senderName.empty()) {
                m_lastWhisper = entry.senderName;
            }
            flashTabs(entry); // FCF_FlashTab
        }
        if (line->source == game::ChatSource::Notification || line->source == game::ChatSource::AreaTrigger) {
            messageText(kSystemColor, false, line->text); // in the middle too, as the client does
        }
        if (m_trace) {
            VD_LOG_INFO("Chat: %s", entry.text.c_str());
        }
    }
}

void SocialScreens::formatLine(GameSession& session, ChatEntry& e) {
    game::Gameplay& play = session.play();
    const game::ChatLine& line = e.raw;
    e.text.clear();
    const std::string_view sender = play.senderName(line);
    e.senderName.assign(sender);
    e.sender = isPlayerGuid(line.sender) ? line.sender : 0;
    e.nameMissing = line.sender != 0 && sender.empty();
    // Phase 17 (honor): a line without a sender (the defense channels' "[2. LocalDefense] : ...") keeps the
    // name empty, as ChatFrame writes it; "..." only for a sender whose name is not known yet.
    const std::string_view shown = sender.empty() && line.sender != 0 ? std::string_view("...") : sender;
    if (line.source == game::ChatSource::TextEmote) {
        // EmotesText lines as the receiving client builds them: "You wave.", "Aeryn waves at you."...
        const u64 me = session.playerGuid();
        const std::string* own = session.playerName(me);
        const bool mine = line.sender == me;
        const bool atMe = !mine && own && !line.text.empty() && sameName(line.text, *own);
        const bool targeted = !line.text.empty();
        const u32 variant = mine ? (targeted ? game::SocialData::kEmoteYouTarget : game::SocialData::kEmoteYou)
                            : atMe ? game::SocialData::kEmoteToYou
                            : targeted ? game::SocialData::kEmoteOthersTarget
                                       : game::SocialData::kEmoteOthers;
        const std::string_view text =
            m_socialData ? m_socialData->textEmoteLine(line.textEmote, variant) : std::string_view{};
        if (text.empty()) {
            game::appendTextEmote(e.text, {}, shown, {}); // "Aeryn makes some strange gestures."
        } else if (mine) {
            game::appendTextEmote(e.text, text, line.text, {});
        } else {
            game::appendTextEmote(e.text, text, shown, line.text);
        }
        indexLinks(e);
        return;
    }
    const u32 number = line.type == ChatType::Channel ? play.social().channelNumber(line.channel) : 0;
    game::appendChatLine(e.text, line, shown, number);
    indexLinks(e);
}

void SocialScreens::help() {
    static constexpr const char* kLines[] = {"VD_SOCIAL_HELP1", "VD_SOCIAL_HELP2", "VD_SOCIAL_HELP3", "VD_SOCIAL_HELP4",
                                             "VD_SOCIAL_HELP5", "VD_SLASH_HELP"}; // phase 13: VD_SLASH_HELP
    for (const char* key : kLines) {
        systemLine(kSystemColor, key);
    }
}

std::string_view SocialScreens::targetName(GameSession& session) {
    const u64 target = session.target();
    if (!isPlayerGuid(target)) {
        return {};
    }
    const std::string* name = session.playerName(target);
    return name ? std::string_view(*name) : std::string_view{};
}

u64 SocialScreens::playerInSight(GameSession& session, std::string_view name) {
    u64 found = 0;
    const game::QueryCache& queries = session.play().queries();
    session.entities().forEach([&](const Entity& e) {
        if (found == 0 && e.type == game::ObjectType::Player) {
            const std::string* known = queries.findPlayerName(e.guid);
            if (known && sameName(*known, name)) {
                found = e.guid;
            }
        }
    });
    return found;
}

u64 SocialScreens::guidByName(GameSession& session, std::string_view name) {
    const game::Social& social = session.play().social();
    const game::QueryCache& queries = session.play().queries();
    for (const game::GroupMember& m : social.group().members) {
        if (sameName(m.name, name)) {
            return m.guid;
        }
    }
    for (const game::FriendInfo& f : social.friends()) {
        const std::string* known = queries.findPlayerName(f.guid);
        if (known && sameName(*known, name)) {
            return f.guid;
        }
    }
    for (const u64 guid : social.ignored()) {
        const std::string* known = queries.findPlayerName(guid);
        if (known && sameName(*known, name)) {
            return guid;
        }
    }
    for (const game::GuildMember& m : social.guild().members) {
        if (sameName(m.name, name)) {
            return m.guid;
        }
    }
    return playerInSight(session, name);
}

void SocialScreens::sendTo(GameSession& session, const ChatTarget& to, std::string_view text) {
    game::Gameplay& play = session.play();
    m_spoke = true;
    if (to.type == ChatType::Channel) {
        const game::ChatChannel* channel = play.social().channelByNumber(to.channel);
        if (!channel) {
            message(kErrorColor, false, "CHAT_NOT_MEMBER_NOTICE", to.channel);
            return;
        }
        game::escapeChatInput(m_outgoing, text); // phase 13: typed bars escaped, inserted links whole
        play.sendChat(ChatType::Channel, m_outgoing, channel->name);
        return;
    }
    if (to.type == ChatType::Whisper && to.target.empty()) {
        message(kErrorColor, false, "ERR_COMMAND_NEEDS_TARGET");
        return;
    }
    // A server command goes as typed; a line, as the client's edit box keeps it (game::escapeChatInput).
    if (!text.empty() && text.front() == '.') {
        play.sendChat(to.type, text, to.target);
        return;
    }
    game::escapeChatInput(m_outgoing, text);
    play.sendChat(to.type, m_outgoing, to.target);
}

void SocialScreens::execute(GameSession& session, const game::ChatCommand& cmd) {
    using K = game::ChatCommandKind;
    game::Gameplay& play = session.play();
    game::Social& social = play.social();
    // Commands that take a player: the name typed, else the target.
    const std::string_view who = !cmd.target.empty() ? cmd.target : targetName(session);
    const auto needName = [&]() {
        if (who.empty()) {
            message(kErrorColor, false, "ERR_COMMAND_NEEDS_TARGET");
            return false;
        }
        return true;
    };
    switch (cmd.kind) {
    case K::None: break;
    case K::Message: {
        ChatTarget to;
        to.type = cmd.type;
        to.target.assign(cmd.target);
        to.channel = cmd.channel;
        sendTo(session, to, cmd.text);
        // Sticky like the client's ChatTypeInfo (ChatFrame.lua): only SAY, PARTY, RAID, GUILD and BATTLEGROUND
        // stay; the others (yells, emotes, whispers, officers, channels, raid warnings) speak once.
        const bool sticky = cmd.type == ChatType::Say || cmd.type == ChatType::Party || cmd.type == ChatType::Raid ||
                            cmd.type == ChatType::Guild || cmd.type == ChatType::Battleground;
        if (sticky && !(cmd.text.size() > 0 && cmd.text[0] == '.')) {
            m_stickyTarget = to;
        }
        m_chatTarget = m_stickyTarget; // ChatEdit_OnEnterPressed: the next line starts in the sticky type
        break;
    }
    case K::SetChannel:
        if (cmd.type == ChatType::Channel && !social.channelByNumber(cmd.channel)) {
            message(kErrorColor, false, "CHAT_NOT_MEMBER_NOTICE", cmd.channel);
            break;
        }
        m_chatTarget.type = cmd.type;
        m_chatTarget.target.assign(cmd.target);
        m_chatTarget.channel = cmd.channel;
        break;
    case K::Reply:
        if (m_lastWhisper.empty()) {
            message(kErrorColor, false, "ERR_NO_REPLY_TARGET");
            break;
        }
        m_chatTarget.type = ChatType::Whisper;
        m_chatTarget.target = m_lastWhisper;
        m_chatTarget.channel = 0;
        if (!cmd.text.empty()) {
            sendTo(session, m_chatTarget, cmd.text);
            m_chatTarget = m_stickyTarget; // whispers are not sticky
        }
        break;
    case K::TextEmote: {
        // The commands of the client's language first (phase 21: EMOTE<n>_CMD<m>, "/vale"), then the emote's token.
        const game::SocialData::TextEmote* emote =
            m_socialData ? m_socialData->textEmoteByCommand(cmd.target) : nullptr;
        if (!emote && m_socialData) {
            emote = m_socialData->textEmoteByToken(cmd.target);
        }
        if (!emote) {
            message(kErrorColor, false, "VD_SOCIAL_UNKNOWN_COMMAND", cmd.target);
            systemLine(kSystemColor, "HELP_TEXT_SIMPLE");
            break;
        }
        social.textEmote(emote->id, session.target());
        break;
    }
    case K::Join:
        if (!social.joinChannel(cmd.target, cmd.text)) {
            message(kErrorColor, false, "ERR_TOO_MANY_CHAT_CHANNELS");
        }
        break;
    case K::Leave:
    case K::ChannelList: {
        const game::ChatChannel* channel = cmd.channel ? social.channelByNumber(cmd.channel) : nullptr;
        const std::string_view name = channel ? std::string_view(channel->name) : cmd.target;
        if (name.empty()) {
            message(kErrorColor, false, "VD_SOCIAL_WHICH_CHANNEL");
            break;
        }
        if (cmd.kind == K::Leave) {
            social.leaveChannel(name);
        } else {
            social.listChannel(name);
        }
        break;
    }
    case K::Invite:
        if (needName()) {
            social.inviteToGroup(who);
        }
        break;
    case K::Uninvite:
        if (needName()) {
            const u64 guid = guidByName(session, who);
            if (guid != 0 && social.inGroupWith(guid)) {
                social.uninvite(guid);
            } else {
                message(kErrorColor, false, "ERR_TARGET_NOT_IN_GROUP_S", who);
            }
        }
        break;
    case K::LeaveGroup: social.leaveGroup(); break;
    case K::Promote:
        if (needName()) {
            const u64 guid = guidByName(session, who);
            if (guid != 0 && social.inGroupWith(guid)) {
                social.promoteToLeader(guid);
            } else {
                message(kErrorColor, false, "ERR_TARGET_NOT_IN_GROUP_S", who);
            }
        }
        break;
    case K::Afk:
        m_afkMessage.assign(cmd.text);
        social.setAfk(cmd.text);
        break;
    case K::Dnd:
        m_dndMessage.assign(cmd.text);
        social.setDnd(cmd.text);
        break;
    case K::Who: {
        // SlashCmdList["WHO"]: an empty /who asks WhoFrame_GetDefaultWhoCommand (the player's zone and the levels
        // around theirs); the text goes into WhoFrameEditBox and to SendWho (retouches, friends frame: as WoW.exe
        // reads it, game/WhoQuery).
        m_whoInput.assign(cmd.text);
        if (m_whoInput.empty()) {
            const u32 zone = m_socialData && m_areaId != 0 ? m_socialData->zoneOf(m_areaId) : 0;
            const std::string_view zoneName = m_socialData && zone != 0 ? m_socialData->areaName(zone) : std::string_view{};
            const game::Entity* me = session.entities().find(session.playerGuid());
            m_whoInput = game::defaultWhoQuery(zoneName, me ? me->value(uf::UNIT_FIELD_LEVEL) : 1);
        }
        sendWho(session, m_whoInput);
        openWindow(SocialTab::Who);
        break;
    }
    case K::AddFriend:
        if (needName()) {
            social.addFriend(who);
        }
        break;
    case K::RemoveFriend:
    case K::Unignore:
        if (needName()) {
            const u64 guid = guidByName(session, who);
            if (guid == 0) {
                message(kErrorColor, false, "ERR_BAD_PLAYER_NAME_S", who);
            } else if (cmd.kind == K::RemoveFriend) {
                social.removeFriend(guid);
            } else {
                social.removeIgnore(guid);
            }
        }
        break;
    case K::Ignore:
        if (needName()) {
            social.addIgnore(who);
        }
        break;
    case K::GuildInvite:
        if (needName()) {
            social.guildInvite(who);
        }
        break;
    case K::GuildRemove:
        if (needName()) {
            social.guildRemove(who);
        }
        break;
    case K::GuildPromote:
        if (needName()) {
            social.guildPromote(who);
        }
        break;
    case K::GuildDemote:
        if (needName()) {
            social.guildDemote(who);
        }
        break;
    case K::GuildLeave: social.leaveGuild(); break;
    case K::GuildMotd: social.setGuildMotd(cmd.text); break;
    case K::GuildRoster: openWindow(SocialTab::Guild); break;
    case K::Duel: {
        const u64 guid = !cmd.target.empty() ? playerInSight(session, cmd.target) : session.target();
        if (!isPlayerGuid(guid) || guid == session.playerGuid() || !session.entities().find(guid)) {
            message(kErrorColor, false, "VD_SOCIAL_DUEL_NEED_TARGET");
            break;
        }
        if (play.castSpell(game::kDuelSpell, guid)) {
            social.challengeSent(guid);
        }
        break;
    }
    case K::Forfeit: social.declineDuel(); break;
    case K::Roll: social.roll(cmd.minimum, cmd.maximum); break;
    case K::Help: help(); break;
    case K::Logout: session.requestLogout(false); break; // parity (popups): then CAMP (app/PopupScreens)
    case K::Quit: session.requestLogout(true); break;
    case K::RaidInfo: // ChatFrame.lua: the frame when something is saved, else the chat says so once answered
        play.travel().requestRaidInfo(true);
        if (!play.travel().savedInstances().empty()) {
            m_modal = Modal::RaidInfo;
            m_returnToWindow = false;
        }
        break;
    case K::Unknown: message(kErrorColor, false, "HELP_TEXT_SIMPLE"); break;
    default: executeMore(session, cmd); break; // phase 13 (social)
    }
}

void SocialScreens::sendTell(std::string_view name) {
    if (name.empty()) {
        return;
    }
    // ChatFrame_SendTell: "/w name " parsed (ChatEdit_ParseText): the box's type becomes WHISPER, focused.
    m_chatTarget.type = ChatType::Whisper;
    m_chatTarget.target.assign(name);
    m_chatTarget.channel = 0;
    m_chatExpanded = true;
    m_focusInput = true;
    m_returnToWindow = false;
    if (m_modal == Modal::Sheet) {
        m_modal = Modal::None;
    }
}

std::string_view SocialScreens::channelName(GameSession& session, const game::ChatCommand& cmd) {
    // The client takes a channel's number for its name (GetChannelName) where it takes a name.
    if (cmd.channel != 0) {
        if (const game::ChatChannel* channel = session.play().social().channelByNumber(cmd.channel)) {
            return channel->name;
        }
    }
    return cmd.target;
}

void SocialScreens::openChatType(GameSession& session) {
    const game::Social& social = session.play().social();
    if (m_chatTarget.type != m_stickyTarget.type) {
        return;
    }
    if ((m_stickyTarget.type == ChatType::Party && social.partyMemberCount() == 0) ||
        (m_stickyTarget.type == ChatType::Raid && social.raidMemberCount() == 0) ||
        (m_stickyTarget.type == ChatType::Battleground && social.raidMemberCount() == 0)) { // phase 17
        m_chatTarget = ChatTarget{}; // SAY
    }
}

void SocialScreens::runLine(GameSession& session, std::string_view line) {
    // Phase 15 (options): ChatFrame's EXECUTE_CHAT_LINE: SetText, ChatEdit_SendText, ChatEdit_OnEscapePressed (the
    // edit box goes back to its sticky type: a "/p" line of a macro speaks in the party once).
    if (line.empty() || !session.inWorld()) {
        return;
    }
    const ChatTarget sticky = m_chatTarget;
    const game::ChatCommand cmd =
        game::parseChatInput(line, m_chatTarget.type, m_chatTarget.target, m_chatTarget.channel);
    execute(session, cmd);
    if (cmd.kind == game::ChatCommandKind::Message || cmd.kind == game::ChatCommandKind::SetChannel) {
        m_chatTarget = sticky;
    }
}

void SocialScreens::castByName(GameSession& session, std::string_view text) {
    // CastSpellByName("Fireball(Rank 3)"): a spell of the book by its name, that rank or the highest known.
    game::Gameplay& play = session.play();
    if (text.empty() || !m_data) {
        return;
    }
    std::string_view name = text, rank;
    const usize open = text.find('(');
    if (open != std::string_view::npos && text.back() == ')') {
        name = text.substr(0, open);
        rank = text.substr(open + 1, text.size() - open - 2);
    }
    while (!name.empty() && name.back() == ' ') {
        name.remove_suffix(1);
    }
    u32 best = 0;
    for (const u32 id : play.spells().known()) {
        const std::optional<game::SpellInfo> info = m_data->spell(id);
        if (!info || !sameName(info->name, name) || (!rank.empty() && !sameName(info->rank, rank))) {
            continue;
        }
        best = id; // the book lists the ranks in order: the last one is the highest
    }
    if (best != 0) {
        play.castSpell(best, play.spellTarget(best));
    }
}

void SocialScreens::executeMore(GameSession& session, const game::ChatCommand& cmd) {
    using K = game::ChatCommandKind;
    game::Gameplay& play = session.play();
    game::Social& social = play.social();
    game::PlayerCommands& commands = play.commands();
    const u64 target = session.target();
    switch (cmd.kind) {
    case K::Follow: // FollowByName(name), or FollowUnit("target")
        if (m_unitMenus) {
            if (cmd.target.empty()) {
                m_unitMenus->followUnit(session, target);
            } else {
                m_unitMenus->followByName(session, cmd.target, false);
            }
        }
        break;
    case K::Assist: // AssistByName(name), or AssistUnit("target")
        if (m_unitMenus) {
            if (cmd.target.empty()) {
                m_unitMenus->assistUnit(session, target);
            } else {
                m_unitMenus->assistByName(session, cmd.target);
            }
        }
        break;
    case K::TargetByName:
        if (m_unitMenus && !cmd.target.empty()) {
            m_unitMenus->targetByName(session, cmd.target, false);
        }
        break;
    case K::Inspect:
        if (m_unitMenus) {
            m_unitMenus->inspectUnit(session, target, true); // InspectUnit("target")
        }
        break;
    case K::Trade:
        if (m_unitMenus) {
            m_unitMenus->initiateTrade(session, target); // InitiateTrade("target")
        }
        break;
    case K::Pvp: commands.togglePvp(session.lastUpdateMs()); break;
    case K::Played: commands.requestTimePlayed(); break;
    case K::Time: {
        // ChatFrame_DisplayGameTime: GameTime_GetTime(), the game's time as the minimap's clock says it.
        if (const std::optional<f32> halfMinutes = session.gameTimeNow(session.lastUpdateMs())) { // phase 18 (p3): now
            char line[64];
            const u32 minutes = static_cast<u32>(*halfMinutes * 0.5f) % (24 * 60);
            systemText(kSystemColor, game::gameTimeText(line, minutes,
                                                        text::get("VD_MINIMAP_TWENTYFOUR_HOUR_TIME") == "1"));
        }
        break;
    }
    case K::Cast: castByName(session, cmd.text); break;
    case K::GuildLeader: {
        const std::string_view who = !cmd.target.empty() ? cmd.target : targetName(session);
        if (!who.empty()) {
            commands.guildSetLeader(who); // GuildSetLeaderByName
        }
        break;
    }
    case K::GuildDisband:
        if (m_popups && social.guild().id != 0 && social.guild().rank == 0) { // IsGuildLeader()
            m_popups->askGuildDisband(session);
        }
        break;
    case K::ReadyCheck:
        if (social.isLeader() && social.group().raid) { // IsRaidLeader() and GetNumRaidMembers() > 0
            commands.readyCheck();
        }
        break;
    case K::LootFreeForAll:
    case K::LootRoundRobin:
        social.setLootMethod(cmd.kind == K::LootFreeForAll ? net::LootMethod::FreeForAll : net::LootMethod::RoundRobin,
                             session.playerGuid(), social.group().lootThreshold);
        break;
    case K::LootMaster: {
        const std::string_view who = !cmd.target.empty() ? cmd.target : targetName(session);
        const std::string* me = session.playerName(session.playerGuid());
        const u64 guid = me && sameName(*me, who) ? session.playerGuid() : guidByName(session, who);
        if (guid != 0) {
            social.setLootMethod(net::LootMethod::MasterLoot, guid, social.group().lootThreshold);
        }
        break;
    }
    case K::Macro: m_macroWanted = true; break; // phase 15 (options): ShowMacroFrame
    case K::MacroHelp: // ChatFrame_DisplayMacroHelpText: MACRO_HELP_TEXT_LINE1.. while the table has them
        for (u32 i = 1; i < 20; ++i) {
            char key[32];
            std::snprintf(key, sizeof(key), "MACRO_HELP_TEXT_LINE%u", i);
            if (!text::has(text::Key(std::string_view(key)))) {
                break;
            }
            systemLine(kSystemColor, text::Key(std::string_view(key)));
        }
        break;
    case K::ChatHelp: // ChatFrame_DisplayChatHelp: CHAT_HELP_TEXT_LINE1.. while the table has them
        for (u32 i = 1; i < 40; ++i) {
            char key[32];
            std::snprintf(key, sizeof(key), "CHAT_HELP_TEXT_LINE%u", i);
            if (!text::has(text::Key(std::string_view(key)))) {
                break;
            }
            systemLine(kSystemColor, text::Key(std::string_view(key)));
        }
        break;
    case K::ChannelPassword: // SetChannelPassword(name, password): an empty password takes it off
        commands.channelCommand(net::Opcode::CMSG_CHANNEL_PASSWORD, channelName(session, cmd), cmd.text);
        break;
    case K::ChannelOwner: // SetChannelOwner, or DisplayChannelOwner without a player
        if (cmd.text.empty()) {
            commands.channelCommand(net::Opcode::CMSG_CHANNEL_OWNER, channelName(session, cmd));
        } else {
            commands.channelCommand(net::Opcode::CMSG_CHANNEL_SET_OWNER, channelName(session, cmd), cmd.text);
        }
        break;
    case K::ChannelModerator:
    case K::ChannelUnmoderator:
    case K::ChannelMute:
    case K::ChannelUnmute:
    case K::ChannelInvite:
    case K::ChannelKick:
    case K::ChannelBan:
    case K::ChannelUnban: {
        if (cmd.text.empty()) {
            break; // ChatFrame.lua: nothing without a player
        }
        const net::Opcode opcode = cmd.kind == K::ChannelModerator     ? net::Opcode::CMSG_CHANNEL_MODERATOR
                                   : cmd.kind == K::ChannelUnmoderator ? net::Opcode::CMSG_CHANNEL_UNMODERATOR
                                   : cmd.kind == K::ChannelMute        ? net::Opcode::CMSG_CHANNEL_MUTE
                                   : cmd.kind == K::ChannelUnmute      ? net::Opcode::CMSG_CHANNEL_UNMUTE
                                   : cmd.kind == K::ChannelInvite      ? net::Opcode::CMSG_CHANNEL_INVITE
                                   : cmd.kind == K::ChannelKick        ? net::Opcode::CMSG_CHANNEL_KICK
                                   : cmd.kind == K::ChannelBan         ? net::Opcode::CMSG_CHANNEL_BAN
                                                                       : net::Opcode::CMSG_CHANNEL_UNBAN;
        commands.channelCommand(opcode, channelName(session, cmd), cmd.text);
        break;
    }
    case K::ChannelAnnounce:
        commands.channelCommand(net::Opcode::CMSG_CHANNEL_ANNOUNCEMENTS, channelName(session, cmd));
        break;
    case K::ChannelModerate:
        commands.channelCommand(net::Opcode::CMSG_CHANNEL_MODERATE, channelName(session, cmd));
        break;
    case K::Script: message(kErrorColor, false, "VD_SLASH_NO_SCRIPT"); break; // RunScript: no Lua here
    case K::GuildInfo: // GuildInfo(): the answer's two lines (readSocialEvents)
        if (commands.guildInfo()) {
            m_guildInfoWanted = true;
        }
        break;
    default: break;
    }
}

void SocialScreens::chatTabs(u32 nowMs, u32& selected) {
    // Phase 18 (p3): the docked windows in their order (DOCKED_CHAT_FRAMES), named as FCF_SetWindowName does.
    std::array<u8, game::kChatWindowSlots> docked{};
    const u32 count = std::min(m_chatWindows.docked(docked), kDocks);
    m_tabCount = 0;
    selected = 0;
    bool found = false;
    for (u32 i = 0; i < count; ++i) {
        const u8 d = docked[i];
        if (m_flashSince[d] != 0 && ui::flashOver(nowMs - m_flashSince[d])) {
            m_flashSince[d] = 0; // UIFrameFlash's 60 seconds are over
        }
        if (d == m_dock) {
            selected = m_tabCount;
            found = true;
        }
        const game::ChatWindowConfig& w = m_chatWindows.window(d);
        std::string_view name = w.name;
        if (name.empty()) {
            name = d == 0   ? text::get("GENERAL")
                   : d == 1 ? text::get("COMBAT_LOG")
                            : text::format(m_tabNames[d], "CHAT_NAME_TEMPLATE", d + 1u);
        }
        m_tabDocks[m_tabCount] = d;
        m_tabViews[m_tabCount++] = {name, m_flashSince[d] != 0 ? ui::flashAlpha(nowMs - m_flashSince[d]) : 0.0f};
    }
    if (!found) {
        m_dock = count > 0 ? docked[0] : kDockGeneral;
    }
}

void SocialScreens::chatLines(u32 nowMs) {
    m_chatViews.clear();
    m_chatViewEntries.clear();
    // At rest a line fades two minutes after it came in or a tap last brought the lines back; open, every
    // line shows (as a ScrollingMessageFrame scrolled up).
    const auto shade = [&](u32 color, u32 timeMs) {
        return m_chatExpanded ? color
                              : ui::withAlpha(color, ui::chatLineAlpha(std::max(timeMs, m_revealMs), nowMs,
                                                                       m_visibleMs));
    };
    // Phase 18 (p3): the window's groups and channels in the chat's lines, and, with a combat group, the combat log's
    // lines among them by time (about others too only with one of the "others" groups on: ChatFrame2's defaults keep
    // to the player and its pet).
    const game::ChatWindowConfig& window = m_chatWindows.window(m_dock);
    const bool combat = m_combatLog && window.anyCombat();
    const bool others = window.combatOthers();
    u32 size = 0, total = 0;
    if (combat) {
        total = m_combatLog->logTotal();
        size = m_combatLog->logSize();
    }
    u32 log = 0;
    const auto logUntil = [&](u32 timeMs, bool all) {
        for (; log < size; ++log) {
            const CombatScreens::LogEntry e = m_combatLog->logEntry(log);
            if (!all && static_cast<i32>(e.timeMs - timeMs) > 0) {
                return;
            }
            if (!others && !e.mine) {
                continue;
            }
            m_chatViews.push_back(
                {e.text, shade(e.color, e.timeMs), kLogKey | ((total - size + log) & ~kLogKey), {}});
            m_chatViewEntries.push_back(kNoEntry);
        }
    };
    for (u32 i = 0; i < m_chatCount; ++i) {
        const u32 index = (m_chatNext - m_chatCount + i) % kChatLines;
        const ChatEntry& e = m_chatLines[index];
        if (shows(window, e)) {
            logUntil(e.timeMs, false);
            m_chatViews.push_back({e.text, shade(e.color, e.timeMs), e.key, {e.links.data(), e.linkCount}});
            m_chatViewEntries.push_back(index);
        }
    }
    logUntil(0, true);
}

std::string_view SocialScreens::editHeader(GameSession& session, std::array<char, 96>& buffer) {
    // ChatEdit_UpdateHeader: CHAT_<type>_SEND ("Say: "), "Tell Aeryn: ", "[2. Trade]: " (here without the channel's
    // zone: the box is short), the player's name for an emote.
    switch (m_chatTarget.type) {
    case ChatType::Whisper: return text::format(buffer, "CHAT_WHISPER_SEND", m_chatTarget.target);
    case ChatType::Channel:
        if (const game::ChatChannel* channel = session.play().social().channelByNumber(m_chatTarget.channel)) {
            char shown[128]; // phase 21: in the player's language
            return text::format(buffer, "CHAT_CHANNEL_SEND", m_chatTarget.channel,
                                game::channelDisplayBase(channel->name, shown));
        }
        return text::get("CHAT_SAY_SEND");
    case ChatType::Emote: {
        const std::string* me = session.playerName(session.playerGuid());
        return text::format(buffer, "CHAT_EMOTE_SEND", me ? std::string_view(*me) : std::string_view("..."));
    }
    case ChatType::Yell: return text::get("CHAT_YELL_SEND");
    case ChatType::Party: return text::get("CHAT_PARTY_SEND");
    case ChatType::Raid: return text::get("CHAT_RAID_SEND");
    case ChatType::RaidWarning: return text::get("CHAT_RAID_WARNING_SEND");
    case ChatType::Guild: return text::get("CHAT_GUILD_SEND");
    case ChatType::Officer: return text::get("CHAT_OFFICER_SEND");
    case ChatType::Battleground: return text::get("CHAT_BATTLEGROUND_SEND"); // phase 17 (queue): /bg
    default: return text::get("CHAT_SAY_SEND");
    }
}

void SocialScreens::touchChat(u32 nowMs) {
    // The client's mouse-over: the tabs and the background for a while; ours also brings every line back.
    m_activeUntilMs = nowMs + kChatActiveMs;
    m_revealMs = nowMs;
}

void SocialScreens::flashTabs(const ChatEntry& e) {
    // FCF_FlashTab: the docked frames that got it and are not shown flash (unless they already do).
    for (u8 d = 0; d < kDocks; ++d) {
        const game::ChatWindowConfig& w = m_chatWindows.window(d);
        if (d != m_dock && w.docked != 0 && shows(w, e) && m_flashSince[d] == 0) {
            m_flashSince[d] = std::max(m_nowMs, 1u);
        }
    }
}

void SocialScreens::linkTapped(GameSession& session, std::string_view line, u32 at, const ui::Rect& where,
                               bool held) {
    // Phase 13: SetItemRef's modifiers. With the edit box open a tap is the Shift + click: the item's link, or the
    // player's name, goes into it. A player's name held is the right click (FriendsDropDown); tapped, the left one
    // whispers (ChatFrame_SendTell).
    game::ChatLink link;
    if (!game::chatLinkAt(line, at, link)) {
        return;
    }
    if (!held && m_chatLink && m_chatLink->active()) {
        if (link.kind == game::ChatLinkKind::Player) {
            m_chatLink->insert(link.player);
        } else {
            // The whole link with its colour: from the "|c" before its "|H" to the "|r" after it.
            usize begin = link.begin, end = link.end;
            if (begin >= 10 && line[begin - 10] == '|' && line[begin - 9] == 'c') {
                begin -= 10;
            }
            if (end + 1 < line.size() && line[end] == '|' && line[end + 1] == 'r') {
                end += 2;
            }
            m_chatLink->insert(line.substr(begin, end - begin));
        }
        return;
    }
    if (link.kind == game::ChatLinkKind::Player) {
        const std::string* me = session.playerName(session.playerGuid());
        if (held) {
            if (m_unitMenus) {
                m_unitMenus->openFriendMenu(session, link.player, where.x, where.bottom());
            }
        } else if (!me || !sameName(*me, link.player)) {
            sendTell(link.player);
        }
        return;
    }
    if (!held) {
        openLink(session, line, at, where);
    }
}

void SocialScreens::openLink(GameSession& session, std::string_view line, u32 at, const ui::Rect& where) {
    // SetItemRef (ItemRef.lua): an item shows ItemRefTooltip (here app/InventoryScreens' item tooltip, beside the
    // link); a player gets a whisper on a left click and FriendsFrame's menu on a right one (here the player's
    // sheet, whose first action whispers).
    game::ChatLink link;
    if (!game::chatLinkAt(line, at, link)) {
        return;
    }
    if (m_trace) {
        VD_LOG_INFO("Chat link: %.*s", static_cast<int>(link.link.size()), link.link.data());
    }
    switch (link.kind) {
    case game::ChatLinkKind::Item: kit::requestItemTooltip(link.id, where); break;
    case game::ChatLinkKind::Player: {
        const std::string* me = session.playerName(session.playerGuid());
        if (!me || !sameName(*me, link.player)) {
            openSheet(session, guidByName(session, link.player), link.player);
        }
        break;
    }
    default: break; // enchants and quests: no tooltip of theirs here yet
    }
}

void SocialScreens::tabMenu(GameSession& session, DialogContext& c) {
    // Phase 18 (p3): the tab's FCFOptionsDropDown (app/ChatConfigScreens).
    if (!m_chatConfig.draw(&session, c, m_chatWindows)) {
        m_modal = Modal::None;
    }
}

void SocialScreens::demoChat(GameSession& session, u32 nowMs) {
    // Development (VD_HUD_DEMO, no server): what a session in Elwynn Forest fills the chat with, worded by the
    // game's keys: channels joined, an item linked on the trade channel, loot, a skill-up, the party, a whisper (its
    // tab flashes) and the combat log's lines. Names and what players say are sample data.
    (void)session;
    m_chatDemoFilled = true;
    m_nowMs = nowMs;
    char buffer[256];
    char link[128];
    const auto said = [&](ChatType type, std::string_view sender, std::string_view what, std::string_view channel,
                          u32 number) {
        game::ChatLine line;
        line.type = type;
        line.text.assign(what);
        line.channel.assign(channel);
        ChatEntry& e = addLine(chatCategory(line), chatColor(type));
        game::appendChatLine(e.text, line, sender, number);
        indexLinks(e);
        if (type == ChatType::Whisper) {
            flashTabs(m_chatLines[(m_chatNext - 1) % kChatLines]);
        }
    };
    const auto rgb = [](game::NoticeKind kind) {
        const game::NoticeColor color = game::noticeColor(kind);
        return rgba(color.r, color.g, color.b);
    };
    systemText(kChannelColor, game::formatChannelNotice(buffer, net::ChannelNotice::YouJoined,
                                                        "1. General - Elwynn Forest", {}, {}));
    systemText(kChannelColor,
               game::formatChannelNotice(buffer, net::ChannelNotice::YouJoined, "2. Trade - City", {}, {}));
    std::string trade("WTS ");
    trade.append(game::formatItemLink(link, 2132, game::itemQualityColor(u8{2}), "Militia Shortsword"));
    trade.append(" 1g, pst");
    said(ChatType::Channel, "Galen", trade, "Trade - City", 2);
    said(ChatType::Say, "Arthel", "almost got the 10 tails", {}, 0);
    systemText(rgb(game::NoticeKind::Loot),
               text::format(buffer, "LOOT_ITEM_SELF_MULTIPLE",
                            game::formatItemLink(link, 2589, game::itemQualityColor(u8{1}), "Linen Cloth"), 2));
    systemText(rgb(game::NoticeKind::Skill), text::format(buffer, "ERR_SKILL_UP_SI", "Swords", 12));
    said(ChatType::Party, "Aeryn", "the kobolds at the entrance are mine", {}, 0);
    systemText(rgb(game::NoticeKind::Loot),
               text::format(buffer, "LOOT_ITEM_SELF",
                            game::formatItemLink(link, 2970, game::itemQualityColor(u8{2}), "Wolfskin Bracers")));
    said(ChatType::Whisper, "Aeryn", "coming to the mine?", {}, 0);
    if (m_combatLog) {
        m_combatLog->logLine(text::format(buffer, "COMBATHITSELFOTHER", "Kobold Vermin", 14), ui::kWhite);
        m_combatLog->logLine(text::format(buffer, "SPELLLOGSELFOTHER", "Heroic Strike", "Kobold Vermin", 23),
                             ui::kWhite);
        m_combatLog->logLine(text::format(buffer, "COMBATHITOTHERSELF", "Kobold Vermin", 3), rgba(255, 90, 90));
        m_combatLog->logLine(text::format(buffer, "COMBATLOG_XPGAIN_FIRSTPERSON", "Kobold Vermin", 45),
                             rgb(game::NoticeKind::XpGain));
        char money[64];
        m_combatLog->logLine(text::format(buffer, "YOU_LOOT_MONEY", game::moneyText(money, 7)),
                             rgb(game::NoticeKind::Money));
    }
}

void SocialScreens::chat(GameSession& session, DialogContext& c, ui::TextInput& text, const ui::Rect& actionBar,
                         u32 nowMs) {
    VD_PROFILE_ZONE("SocialScreens::chat");
    m_nowMs = nowMs;
    if (!session.inWorld()) {
        if (!m_chatDemo) {
            return;
        }
        if (!m_chatDemoFilled) {
            demoChat(session, nowMs);
        }
    }
    constexpr ui::WidgetId kChatField = ui::widgetId("chat.input");
    game::Gameplay& play = session.play();
    m_chatLink = c.chatLink; // phase 13: a link tapped with the edit box open goes into it
    if (m_focusInput) { // phase 13: ChatFrame_SendTell focuses the edit box
        m_focusInput = false;
        m_chatExpanded = true;
        if (!(text.active() && text.field() == kChatField)) {
            openChatType(session);
            text.begin(kChatField, m_chatInput, 255, ui::TextKind::Message);
        }
    }
    const bool editing = text.active() && text.field() == kChatField;
    if (editing) {
        m_chatInput.assign(text.text());
    }
    const auto send = [&]() {
        if (!m_chatInput.empty()) {
            const game::ChatCommand cmd =
                game::parseChatInput(m_chatInput, m_chatTarget.type, m_chatTarget.target, m_chatTarget.channel);
            execute(session, cmd);
        }
        m_chatInput.clear();
        if (text.active() && text.field() == kChatField) {
            text.begin(kChatField, {}, 255, ui::TextKind::Message); // stays open for the next line
        }
    };
    if (editing && text.submitted()) {
        send();
    }

    // The docked frames and the selected one's lines.
    u32 selected = 0;
    chatTabs(nowMs, selected);
    chatLines(nowMs);
    // The background fades in while the chat is touched (the client's mouse-over), and out after. Phase 20:
    // at rest the tabs never show, not even while one flashes (FCF_OnUpdate); open, they do.
    const f32 dt = m_lastChatMs != 0 ? static_cast<f32>(nowMs - m_lastChatMs) : 0.0f;
    m_lastChatMs = nowMs;
    const bool active = static_cast<i32>(m_activeUntilMs - nowMs) > 0;
    m_backgroundAlpha = ui::fadeTowards(m_backgroundAlpha, active ? 1.0f : 0.0f, dt);

    std::array<char, 96> header;
    ChatFrameView view;
    view.lines = m_chatViews;
    view.tabs = {m_tabViews.data(), m_tabCount};
    view.selected = selected;
    view.expanded = m_chatExpanded;
    view.backgroundAlpha = m_backgroundAlpha;
    {
        // Phase 18 (p3): the selected window's font size and background.
        const game::ChatWindowConfig& w = m_chatWindows.window(m_dock);
        view.fontScale = w.fontSize != 0 ? static_cast<f32>(w.fontSize) / 14.0f : 1.0f;
        view.backgroundColor = rgba(w.r, w.g, w.b);
        view.restAlpha = static_cast<f32>(w.a) / 255.0f;
    }
    view.header = editHeader(session, header);
    view.headerColor = chatColor(m_chatTarget.type);
    view.input = m_chatInput;
    view.editing = editing;
    const bool away = play.social().afk() || play.social().dnd();
    view.hint = text::get(!away ? "VD_SOCIAL_CHAT_HINT" : play.social().afk() ? "VD_SOCIAL_AFK_HINT"
                                                                               : "VD_SOCIAL_DND_HINT");
    const ui::Rect frame = chatFrameRect(c, m_chatExpanded, actionBar, m_partyRect, editing, view.fontScale);
    m_chatRect = chatFrameBounds(c, frame, m_chatExpanded);
    const ChatFrameResult r = chatFrame(c, frame, view, m_chatState);

    if (r.touched) {
        touchChat(nowMs);
    }
    if (r.tab >= 0 && static_cast<u32>(r.tab) < m_tabCount) {
        const u8 dock = m_tabDocks[static_cast<u32>(r.tab)];
        m_dock = dock;
        m_flashSince[dock] = 0; // FCF_SelectDockFrame stops its flash
    }
    if (r.tabMenu >= 0 && static_cast<u32>(r.tabMenu) < m_tabCount) { // phase 18 (p3): every tab has its menu
        m_menuDock = m_tabDocks[static_cast<u32>(r.tabMenu)];
        m_chatConfig.open(m_menuDock);
        m_modal = Modal::ChatTabMenu;
    }
    if (r.toggle) { // a double tap on the frame
        m_chatExpanded = !m_chatExpanded;
        if (!m_chatExpanded && editing) {
            text.end();
        }
    }
    if (r.tappedInput) {
        if (editing) {
            text.end();
        } else {
            openChatType(session);
            text.begin(kChatField, m_chatInput, 255, ui::TextKind::Message);
        }
    }
    if (r.send) {
        send();
    }
    if (r.pickChannel) {
        m_modal = Modal::Channels;
    }
    if (r.emotes) {
        m_modal = Modal::Emotes;
    }
    if (r.linkLine >= 0 && static_cast<u32>(r.linkLine) < m_chatViews.size()) {
        linkTapped(session, m_chatViews[static_cast<u32>(r.linkLine)].text, r.linkBegin, r.linkRect, false);
    } else if (r.linkHeldLine >= 0 && static_cast<u32>(r.linkHeldLine) < m_chatViews.size()) {
        linkTapped(session, m_chatViews[static_cast<u32>(r.linkHeldLine)].text, r.linkHeldBegin, r.linkHeldRect,
                   true);
    } else if (r.line >= 0 && static_cast<u32>(r.line) < m_chatViewEntries.size() &&
               m_chatViewEntries[static_cast<u32>(r.line)] != kNoEntry) {
        // A tap on a line outside its links: its sender's menu (FriendsDropDown, the right click on its name: a
        // finger finds a line more easily than a name).
        const ChatEntry& e = m_chatLines[m_chatViewEntries[static_cast<u32>(r.line)]];
        if (e.sender != 0 && e.sender != session.playerGuid() && m_unitMenus && !e.senderName.empty()) {
            m_unitMenus->openFriendMenu(session, e.senderName, r.lineRect.x, r.lineRect.bottom());
        }
    }
}

// --- Social events -----------------------------------------------------------------------------------------------

void SocialScreens::update(GameSession& session, u32 nowMs) {
    VD_PROFILE_ZONE("SocialScreens::update");
    if (!session.inWorld()) {
        return;
    }
    game::Gameplay& play = session.play();
    m_nowMs = nowMs;
    if (m_cursorsPlayer != session.playerGuid()) {
        m_cursorsPlayer = session.playerGuid();
        reset();
        m_chatCursor = play.chat().end() > kChatLines ? play.chat().end() - kChatLines : 0;
        m_socialCursor = play.social().events().end();
    }
    // The zone channels follow the zone the player stands in.
    if (m_socialData && m_areaId != 0) {
        const u32 zone = m_socialData->zoneOf(m_areaId);
        if (zone != 0 && zone != play.social().zone()) {
            play.social().setZone(zone, m_socialData->zoneChannels(zone));
        }
    }
    readChat(session);
    readSocialEvents(session, nowMs);
    // Lines formatted before a name arrived.
    if (session.queryVersion() != m_queryVersion) {
        m_queryVersion = session.queryVersion();
        for (u32 i = 0; i < m_chatCount; ++i) {
            ChatEntry& e = m_chatLines[(m_chatNext - m_chatCount + i) % kChatLines];
            if (e.nameMissing) {
                formatLine(session, e);
                if (e.raw.type == ChatType::Whisper && !e.senderName.empty()) {
                    m_lastWhisper = e.senderName;
                }
            }
        }
    }
    // The duel's countdown, big in the middle.
    const game::DuelState& duel = play.social().duel();
    if (duel.phase == game::DuelPhase::Countdown) {
        const i32 left = static_cast<i32>(duel.startsAtMs - nowMs);
        const u32 second = left > 0 ? static_cast<u32>(left + 999) / 1000 : 0;
        if (second != 0 && second != m_duelShownSecond) {
            m_duelShownSecond = second;
            char number[16];
            std::snprintf(number, sizeof(number), "%u", second);
            messageText(kDuelColor, true, number);
        }
    } else if (duel.phase == game::DuelPhase::Fighting && m_duelShownSecond != 0) {
        m_duelShownSecond = 0;
        message(kDuelColor, true, "VD_SOCIAL_DUEL_FIGHT");
    }
}

void SocialScreens::readSocialEvents(GameSession& session, u32 nowMs) {
    (void)nowMs;
    game::Gameplay& play = session.play();
    game::Social& social = play.social();
    const auto playerOr = [&](u64 guid) -> std::string_view {
        const std::string* known = session.playerName(guid);
        return known ? std::string_view(*known) : std::string_view("...");
    };
    while (const game::SocialEvent* e = social.events().next(m_socialCursor)) {
        if (m_trace) {
            VD_LOG_INFO("Social event %u: %s / %s values %d %d %d", static_cast<u32>(e->kind), e->name.c_str(),
                        e->text.c_str(), e->value[0], e->value[1], e->value[2]);
        }
        const std::string_view name = e->name;
        switch (e->kind) {
        case SocialEventKind::GroupInvite:
            systemLine(kSystemColor, "ERR_INVITED_TO_GROUP_S", name); // the question: app/PopupScreens
            break;
        case SocialEventKind::PartyResult: {
            const auto result = static_cast<net::PartyResult>(e->value[1]);
            if (result == net::PartyResult::Ok) {
                if (static_cast<u32>(e->value[0]) == net::kPartyOpInvite && !name.empty()) {
                    systemLine(kSystemColor, "ERR_INVITE_PLAYER_S", name);
                }
            } else if (const char* key = game::partyResultKey(result)) {
                message(kErrorColor, false, key, name);
                systemLine(kSystemColor, key, name);
            }
            break;
        }
        case SocialEventKind::GroupDeclined: systemLine(kSystemColor, "ERR_DECLINE_GROUP_S", name); break;
        case SocialEventKind::GroupJoined:
            if (!social.group().raid) { // phase 16 (raidframe): a raid's first list: ERR_RAID_YOU_JOINED
                systemLine(kSystemColor, "VD_SOCIAL_GROUP_JOINED");
            }
            m_lootMethodSeen = social.group().lootMethod;
            m_lootThresholdSeen = social.group().lootThreshold;
            break;
        case SocialEventKind::GroupLeft:
            systemLine(kSystemColor, "NOT_IN_GROUP");
            m_lootMethodSeen = m_lootThresholdSeen = -1;
            break;
        case SocialEventKind::GroupUninvited: systemLine(kSystemColor, "ERR_UNINVITE_YOU"); break;
        case SocialEventKind::GroupDestroyed: systemLine(kSystemColor, "ERR_GROUP_DISBANDED"); break;
        // Phase 16 (raidframe): in a raid, WoW.exe's roster (0x4BA5F0) names who joins or leaves only once
        // the raid existed before this list (a raid's first list: ERR_RAID_YOU_JOINED alone, below).
        case SocialEventKind::MemberJoined:
            if (!social.group().raid) {
                systemLine(kSystemColor, "ERR_JOINED_GROUP_S", name);
            } else if (m_raidSeen) {
                systemLine(kSystemColor, "ERR_RAID_MEMBER_ADDED_S", name);
            }
            break;
        case SocialEventKind::MemberLeft:
            if (!social.group().raid) {
                systemLine(kSystemColor, "ERR_LEFT_GROUP_S", name);
            } else if (m_raidSeen) {
                systemLine(kSystemColor, "ERR_RAID_MEMBER_REMOVED_S", name);
            }
            break;
        case SocialEventKind::LeaderChanged: {
            const std::string* own = session.playerName(session.playerGuid());
            if (own && sameName(*own, name)) {
                systemLine(kSystemColor, "ERR_NEW_LEADER_YOU");
            } else {
                systemLine(kSystemColor, "ERR_NEW_LEADER_S", name);
            }
            break;
        }
        case SocialEventKind::LootMethodChanged: {
            // What changed of the group's loot rules, as the client prints it.
            const u8 method = static_cast<u8>(e->value[0]), threshold = static_cast<u8>(e->value[1]);
            if (static_cast<i32>(method) != m_lootMethodSeen) {
                if (const char* key = game::lootMethodChangedKey(method)) {
                    systemLine(kSystemColor, key);
                }
            }
            if (static_cast<i32>(threshold) != m_lootThresholdSeen) {
                char quality[64];
                systemLine(kSystemColor, "ERR_SET_LOOT_THRESHOLD_S", coloredQuality(quality, threshold));
            }
            m_lootMethodSeen = method;
            m_lootThresholdSeen = threshold;
            break;
        }
        case SocialEventKind::ChannelNotice: {
            // The channel as the chat names it ("2. Trade - City"), the player the notice is about and who did it.
            char channel[96];
            if (e->value[1] > 0) {
                std::snprintf(channel, sizeof(channel), "%d. %.*s", e->value[1], static_cast<int>(name.size()),
                              name.data());
            } else {
                std::snprintf(channel, sizeof(channel), "%.*s", static_cast<int>(name.size()), name.data());
            }
            const std::string_view player = e->guid != 0 ? playerOr(e->guid) : std::string_view(e->text);
            const std::string_view by = e->other != 0 ? playerOr(e->other) : std::string_view{};
            char line[256];
            const std::string_view text = game::formatChannelNotice(
                line, static_cast<net::ChannelNotice>(e->value[0]), channel, player, by);
            if (!text.empty()) {
                systemText(kChannelColor, text);
            }
            break;
        }
        case SocialEventKind::ChannelMembers: {
            const net::ChannelList& list = social.channelMembers();
            std::string& line = addLine(kCatSystem, kChannelColor).text;
            text::append(line, "CHAT_CHANNEL_LIST_GET", name);
            for (u32 i = 0; i < list.members.size() && i < 40; ++i) {
                line.append(i == 0 ? "" : ", ");
                line.append(playerOr(list.members[i].guid));
            }
            break;
        }
        case SocialEventKind::FriendStatus: {
            const auto result = static_cast<net::FriendResult>(e->value[0]);
            const std::string_view who = name.empty() ? std::string_view("...") : name;
            const char* key = game::friendResultKey(result);
            switch (result) {
            case net::FriendResult::Online:
            case net::FriendResult::Offline:
            case net::FriendResult::AddedOnline:
            case net::FriendResult::AddedOffline:
            case net::FriendResult::Removed:
            case net::FriendResult::IgnoreAdded:
            case net::FriendResult::IgnoreRemoved:
                systemLine(kSystemColor, key, who, who); // ERR_FRIEND_ONLINE_SS: the name for its link and its text
                break;
            default:
                message(kErrorColor, false, key, who);
                systemLine(kSystemColor, key, who);
                break;
            }
            break;
        }
        case SocialEventKind::WhoResults: {
            const i32 total = std::max(e->value[0], e->value[1]);
            systemLine(kSystemColor, total == 1 ? "WHO_NUM_RESULTS" : "WHO_NUM_RESULTS_P1", total);
            break;
        }
        case SocialEventKind::GuildInvite:
            systemLine(kGuildColor, "ERR_INVITED_TO_GUILD_SS", name, e->text); // the question: PopupScreens
            break;
        case SocialEventKind::GuildEvent: {
            // The event's strings in its order (officer, member, rank; removed, by whom; old, new leader...).
            const auto kind = static_cast<net::GuildEventKind>(e->value[0]);
            const char* key = game::guildEventKey(kind);
            if (!key || (kind == net::GuildEventKind::Motd && e->name.empty())) {
                break;
            }
            if (!m_guildMemberAlert &&
                (kind == net::GuildEventKind::SignedOn || kind == net::GuildEventKind::SignedOff)) {
                break; // phase 15 (options): GUILDMEMBER_ALERT off
            }
            if (kind == net::GuildEventKind::SignedOn) {
                systemLine(kGuildColor, key, name, name); // ERR_FRIEND_ONLINE_SS
            } else {
                systemLine(kGuildColor, key, name, e->text, e->extra);
            }
            break;
        }
        case SocialEventKind::GuildCommandResult: {
            const auto error = static_cast<net::GuildError>(e->value[1]);
            const u32 command = static_cast<u32>(e->value[0]);
            if (error == net::GuildError::Ok) {
                if (command == net::kGuildCommandQuit) {
                    systemLine(kGuildColor, "ERR_GUILD_QUIT_S", name);
                } else if (command == net::kGuildCommandInvite && !name.empty()) {
                    systemLine(kGuildColor, "ERR_GUILD_INVITE_S", name);
                }
            } else if (const char* key = game::guildErrorKey(error, command)) {
                message(kErrorColor, false, key, name);
                systemLine(kGuildColor, key, name);
            }
            break;
        }
        case SocialEventKind::GuildDeclined: systemLine(kGuildColor, "ERR_GUILD_DECLINE_S", name); break;
        case SocialEventKind::DuelRequested:
            if (e->value[0] == 1) {
                systemLine(kSystemColor, "ERR_DUEL_REQUESTED");
            } else {
                systemLine(kSystemColor, "DUEL_REQUESTED", playerOr(e->guid)); // the question: PopupScreens
            }
            break;
        case SocialEventKind::DuelCountdown: m_duelShownSecond = 0; break;
        // DuelOutOfBounds: the DUEL_OUTOFBOUNDS popup's countdown (app/PopupScreens).
        case SocialEventKind::DuelComplete:
            if (e->value[0] == 0) {
                systemLine(kSystemColor, "ERR_DUEL_CANCELLED");
            }
            break;
        case SocialEventKind::DuelWinner:
            // The winner, then the loser (who fled, or was knocked out).
            systemLine(kSystemColor, e->value[0] == 1 ? "DUEL_WINNER_RETREAT" : "DUEL_WINNER_KNOCKOUT", name, e->text);
            if (e->value[1] == 1) {
                message(kDuelColor, true, "VD_SOCIAL_DUEL_WINNER", name);
            }
            break;
        case SocialEventKind::AwayChanged: {
            const bool afk = e->value[0] != 0, dnd = e->value[1] != 0;
            if (afk != m_lastAfk) {
                if (!afk) {
                    systemLine(kSystemColor, "CLEARED_AFK");
                } else if (m_afkMessage.empty()) {
                    systemLine(kSystemColor, "MARKED_AFK");
                } else {
                    systemLine(kSystemColor, "MARKED_AFK_MESSAGE", m_afkMessage);
                }
            }
            if (dnd != m_lastDnd) {
                if (!dnd) {
                    systemLine(kSystemColor, "CLEARED_DND");
                } else {
                    const std::string_view why =
                        m_dndMessage.empty() ? text::get("DEFAULT_DND_MESSAGE") : std::string_view(m_dndMessage);
                    systemLine(kSystemColor, "MARKED_DND", why);
                }
            }
            m_lastAfk = afk;
            m_lastDnd = dnd;
            break;
        }
        case SocialEventKind::Roll:
            systemLine(kSystemColor, "RANDOM_ROLL_RESULT", playerOr(e->guid), e->value[0], e->value[1], e->value[2]);
            break;
        case SocialEventKind::GuildInfo: // phase 13: /ginfo (WoW.exe's SMSG_GUILD_INFO handler, two system lines)
            if (m_guildInfoWanted) {
                m_guildInfoWanted = false;
                systemLine(kSystemColor, "GUILD_NAME_TEMPLATE", name);
                systemLine(kSystemColor, "GUILD_INFO_TEMPLATE", e->value[0], e->value[1], e->value[2],
                           social.guild().memberCount, social.guild().accounts);
            }
            break;
        default: break;
        }
    }
    // Phase 16 (raidframe): WoW.exe's raid roster says when the player's raid starts (0x4BA5F0: the first
    // raid list, a party converted, a raid joined or found at login) and ends (0x4BA550).
    const bool inRaid = social.raidMemberCount() > 0;
    if (inRaid != m_raidSeen) {
        m_raidSeen = inRaid;
        systemLine(kSystemColor, inRaid ? "ERR_RAID_YOU_JOINED" : "ERR_RAID_YOU_LEFT");
    }
}

// --- Party frames ------------------------------------------------------------------------------------------------

f32 SocialScreens::partyFrames(GameSession& session, DialogContext& c, f32 top) {
    VD_PROFILE_ZONE("SocialScreens::party");
    m_partyViews.clear();
    m_partyRect = {};
    m_partyFrames = {}; // phase 20 (assists)
    m_partyGuids = {};
    if (!session.inWorld()) {
        return 0.0f;
    }
    game::Gameplay& play = session.play();
    // Phase 15 (options): HIDE_PARTY_INTERFACE hides them in a raid (PartyMemberFrame_UpdateMember).
    if (m_hidePartyInRaid && play.social().group().raid) {
        return 0.0f;
    }
    const u32 count = play.social().partyMembers(m_partyMembers);
    for (u32 i = 0; i < count; ++i) {
        const game::PartyMemberView& m = m_partyMembers[i];
        PartyFrameView v;
        v.name = m.name;
        v.level = m.level;
        v.health = m.maxHealth > 0 ? static_cast<f32>(m.health) / static_cast<f32>(m.maxHealth) : 0.0f;
        v.power = m.maxPower > 0 ? static_cast<f32>(m.power) / static_cast<f32>(m.maxPower) : 0.0f;
        v.powerType = m.powerType;
        v.race = m.race;
        v.gender = m.gender;
        v.online = m.online;
        v.dead = m.dead;
        v.ghost = m.ghost;
        v.afk = m.afk;
        v.dnd = m.dnd;
        v.leader = m.leader;
        v.inSight = m.inSight;
        v.targeted = m.guid == play.target();
        v.portrait = m_portraits ? m_portraits->unit(m.guid) : TextureHandle{};
        if (m_auras) {
            m_auras->partyDebuffs(session, m.guid, v, static_cast<u32>(c.ui.nowMs())); // parity: debuffs, glow
        }
        // Phase 15 (pets): PartyMemberFrame_UpdatePet: the member connected, "partypetN" known, SHOW_PARTY_PETS on.
        if (m_showPartyPets && m.online && m.petGuid != 0 && m.petMaxHealth > 0) {
            v.pet = true;
            v.petHealth = static_cast<f32>(m.petHealth) / static_cast<f32>(m.petMaxHealth);
            v.petTargeted = m.petGuid == play.target();
            v.petPortrait = m_portraits && m.petInSight ? m_portraits->unit(m.petGuid) : TextureHandle{};
            if (m_auras) {
                m_auras->partyPetDebuffs(session, m.petGuid, v);
            }
        }
        m_partyViews.push_back(v);
    }
    if (m_partyViews.empty()) {
        return 0.0f;
    }
    ui::Gui g{c.ui, c.draw};
    ui::Rect area{c.safeArea.x + g.px(4.0f), top, g.unit(128.0f) * 1.6f, 0.0f};
    if (m_partyDock.w > 0.0f && m_partyDock.h > 0.0f) { // phase 20 (assists): beside the touch cluster, at its foot
        area.x = m_partyDockLeft ? m_partyDock.x : std::max(m_partyDock.x, m_partyDock.right() - area.w);
        area.y = top = std::max(m_partyDock.y, m_partyDock.bottom() - partyFramesHeight(c, m_partyViews));
    }
    i32 held = -1;
    ui::Rect heldFrame;
    ui::Rect tappedFrame;
    if (m_partyBackground && !m_hidePartyInRaid) {
        // SHOW_PARTY_BACKGROUND (UpdatePartyMemberBackground): from 6 units over the first frame to 5 under the last,
        // 5 units left of them and 134 wide, at PARTYBACKGROUND_OPACITY 0.5; background inset 32, edge 32.
        const f32 s = area.w / 128.0f;
        const f32 frames = partyFramesHeight(c, static_cast<u32>(m_partyViews.size()));
        const ui::Rect r{area.x - 5.0f * s, top - 6.0f * s, 134.0f * s, frames + 11.0f * s};
        const u32 tint = ui::withAlpha(ui::kWhite, 0.5f);
        const f32 edge = std::min(32.0f * s, r.h * 0.5f);
        if (r.h > 2.0f * edge) {
            g.draw.sprite(r.inset(edge), kPartyBackground, tint);
        }
        g.draw.backdropBorder(r, kPartyBorder, edge, tint);
    }
    i32 tappedPet = -1; // phase 15 (pets)
    const i32 tapped =
        vd::partyFrames(c, area, m_partyViews, &held, &heldFrame, &tappedFrame, &tappedPet, m_partyFrames.data());
    for (u32 i = 0; i < 4 && i < m_partyViews.size(); ++i) { // phase 20 (assists): whose frames they are
        m_partyGuids[i] = m_partyMembers[i].guid;
        m_partyGuids[4 + i] = m_partyMembers[i].petGuid;
    }
    if (tappedPet >= 0 && m_partyMembers[static_cast<u32>(tappedPet)].petInSight) {
        play.setTarget(m_partyMembers[static_cast<u32>(tappedPet)].petGuid); // PartyMemberPetFrame_OnClick: TargetUnit
    }
    if (held >= 0 && m_unitMenus) { // phase 13: the right click, PartyMemberFrameNDropDown ("PARTY")
        m_unitMenus->openUnitMenu(session, m_partyMembers[static_cast<u32>(held)].guid, UnitMenuScreens::Frame::Party,
                                  heldFrame);
    }
    if (tapped >= 0) {
        // The left click targets the member (TargetUnit); the member's auras (PartyMemberBuffTooltip, the
        // mouse-over) on a second tap, or out of sight.
        const game::PartyMemberView& m = m_partyMembers[static_cast<u32>(tapped)];
        if (m.inSight && play.target() != m.guid) {
            play.setTarget(m.guid);
        } else if (m_auras) {
            m_auras->showPartyAuras(m.guid, tappedFrame, tappedFrame.w / 120.0f);
        }
    }
    const f32 height = partyFramesHeight(c, m_partyViews); // phase 15 (pets): with the pets' frames
    m_partyRect = {area.x, top, area.w, height}; // the chat keeps clear of them
    return height;
}

u64 SocialScreens::partyUnitAt(f32 x, f32 y, ui::Rect& frame) const {
    // Phase 20 (assists): the pets' small frames first (drawn over their members').
    for (u32 n = 0; n < m_partyFrames.size(); ++n) {
        const u32 i = (n + 4) % 8;
        if (m_partyGuids[i] != 0 && m_partyFrames[i].w > 0.0f && m_partyFrames[i].contains(x, y)) {
            frame = m_partyFrames[i];
            return m_partyGuids[i];
        }
    }
    return 0;
}

// --- Modal windows -----------------------------------------------------------------------------------------------

bool SocialScreens::wantsModal() const {
    return m_modal != Modal::None;
}

void SocialScreens::openWindow(SocialTab tab) {
    m_modal = Modal::Window;
    m_tab = tab;
}

void SocialScreens::closeWindow() {
    m_returnToWindow = false;
    if (m_modal == Modal::Window) {
        m_modal = Modal::None;
    }
}

void SocialScreens::openSheet(GameSession& session, u64 guid, std::string_view name) {
    m_sheetGuid = guid;
    m_sheetName.assign(name);
    if (m_sheetName.empty()) {
        if (const std::string* known = session.playerName(guid)) {
            m_sheetName = *known;
        }
    }
    if (m_sheetName.empty()) {
        return;
    }
    m_returnToWindow = m_modal == Modal::Window;
    m_modal = Modal::Sheet;
}

void SocialScreens::closeModal() {
    m_modal = m_returnToWindow ? Modal::Window : Modal::None;
    m_returnToWindow = false;
}

void SocialScreens::sheetAction(GameSession& session, u32 action) {
    game::Gameplay& play = session.play();
    game::Social& social = play.social();
    const u64 guid = m_sheetGuid != 0 ? m_sheetGuid : guidByName(session, m_sheetName);
    switch (action) {
    case kActWhisper: // the chat, ready to whisper (the window closes)
        m_chatTarget.type = ChatType::Whisper;
        m_chatTarget.target = m_sheetName;
        m_chatTarget.channel = 0;
        m_chatExpanded = true;
        m_returnToWindow = false;
        break;
    case kActTarget: play.setTarget(guid); break;
    case kActInvite: social.inviteToGroup(m_sheetName); break;
    case kActUninvite: social.uninvite(guid); break;
    case kActPromote: social.promoteToLeader(guid); break;
    case kActAddFriend: social.addFriend(m_sheetName); break;
    case kActRemoveFriend: social.removeFriend(guid); break;
    case kActIgnore: social.addIgnore(m_sheetName); break;
    case kActUnignore: social.removeIgnore(guid); break;
    case kActDuel:
        if (play.castSpell(game::kDuelSpell, guid)) {
            social.challengeSent(guid);
        }
        break;
    case kActGuildInvite: social.guildInvite(m_sheetName); break;
    case kActGuildPromote: social.guildPromote(m_sheetName); break;
    case kActGuildDemote: social.guildDemote(m_sheetName); break;
    case kActGuildRemove:
        m_confirm = Confirm::GuildRemove;
        m_confirmName = m_sheetName;
        m_modal = Modal::Confirm;
        return;
    case kActEmote: play.setTarget(guid); m_modal = Modal::Emotes; return;
    case kActTrade: { // phase 8: the trade window opens when the other player accepts
        const std::string_view problem = play.exchange().trade().initiateProblem(guid);
        if (!problem.empty()) {
            message(kErrorColor, false, "%.*s", static_cast<int>(problem.size()), problem.data());
        } else {
            play.exchange().trade().initiate(guid);
        }
        break;
    }
    default: break;
    }
    closeModal();
}

void SocialScreens::drawModal(GameSession& session, DialogContext& c, ui::TextInput& text, u32 nowMs) {
    VD_PROFILE_ZONE("SocialScreens::modal");
    (void)nowMs;
    game::Gameplay& play = session.play();
    game::Social& social = play.social();
    switch (m_modal) {
    case Modal::Confirm: {
        char buffer[192];
        std::string_view question;
        switch (m_confirm) {
        case Confirm::LeaveGuild:
            question = text::format(buffer, "CONFIRM_GUILD_LEAVE",
                                    social.guild().known ? std::string_view(social.guild().name) : text::get("GUILD"));
            break;
        case Confirm::GuildRemove: question = text::format(buffer, "REMOVE_GUILDMEMBER_LABEL", m_confirmName); break;
        default: question = text::get("VD_SOCIAL_CONFIRM"); break;
        }
        const ConfirmResult r = confirmDialog(c, question, text::get("ACCEPT"), text::get("CANCEL"));
        if (r.yes) {
            switch (m_confirm) {
            case Confirm::LeaveGuild: social.leaveGuild(); break;
            case Confirm::GuildRemove: social.guildRemove(m_confirmName); break;
            default: break;
            }
        }
        if (r.yes || r.no) {
            m_confirm = Confirm::None;
            closeModal();
        }
        break;
    }
    case Modal::Emotes: {
        const std::string_view target = targetName(session);
        std::array<EmoteButtonView, std::size(kEmotes)> emotes;
        for (u32 i = 0; i < emotes.size(); ++i) {
            emotes[i] = {text::get(kEmotes[i].key), kEmotes[i].token};
        }
        const i32 picked = emoteMenu(c, emotes, target);
        if (picked >= 0) {
            const game::SocialData::TextEmote* emote =
                m_socialData ? m_socialData->textEmoteByToken(kEmotes[picked].token) : nullptr;
            if (emote) {
                social.textEmote(emote->id, session.target());
            }
            closeModal();
        } else if (picked == -2) {
            closeModal();
        }
        break;
    }
    case Modal::Channels: {
        // Where the input goes: the chat types the player can use, its channels, the last whisper.
        m_choices.clear();
        m_choiceTargets.clear();
        const auto add = [&](std::string_view label, ChatType type, u32 channel, std::string_view target) {
            if (m_choices.size() >= m_choiceTexts.size()) {
                return;
            }
            std::string& textOut = m_choiceTexts[m_choices.size()];
            textOut.assign(label);
            ChatTarget to;
            to.type = type;
            to.channel = channel;
            to.target.assign(target);
            const bool selected = to.type == m_chatTarget.type && to.channel == m_chatTarget.channel &&
                                  (type != ChatType::Whisper || sameName(to.target, m_chatTarget.target));
            m_choices.push_back({textOut, chatColor(type), selected, true});
            m_choiceTargets.push_back(std::move(to));
        };
        const auto addType = [&](ChatType type) { add(text::get(game::chatTypeKey(type)), type, 0, {}); };
        addType(ChatType::Say);
        addType(ChatType::Yell);
        if (social.group().active) {
            addType(ChatType::Party);
            if (social.group().raid) {
                addType(ChatType::Raid);
            }
        }
        if (social.guild().id != 0) {
            addType(ChatType::Guild);
            addType(ChatType::Officer);
        }
        char label[128];
        for (u32 n = 1; n <= game::kMaxChatChannels; ++n) {
            if (const game::ChatChannel* channel = social.channelByNumber(n)) {
                char shown[96]; // phase 21: in the player's language
                const std::string_view name = game::channelDisplayName(channel->name, shown);
                std::snprintf(label, sizeof(label), "%u. %.*s", n, static_cast<int>(name.size()), name.data());
                add(label, ChatType::Channel, n, {});
            }
        }
        if (!m_lastWhisper.empty()) { // "Tell Aeryn": whoever whispered last
            const std::string_view tell = trimLabel(text::format(label, "CHAT_WHISPER_SEND", m_lastWhisper));
            add(tell, ChatType::Whisper, 0, m_lastWhisper);
        }
        // Phase 18 (extras): ChatMenu's Language submenu (LanguageMenu_LoadLanguages: GetLanguageByIndex), when the
        // player speaks more than one: "Language: Dwarvish" picks the edit box's language (LanguageMenu_Click).
        const usize firstLanguage = m_choices.size();
        std::array<u32, 16> languages{};
        const u32 languageCount = play.knownLanguages(languages);
        const u32 speaking = play.chatLanguage() != 0 ? play.chatLanguage() : play.defaultLanguage();
        for (u32 i = 0; languageCount > 1 && i < languageCount && m_choices.size() < m_choiceTexts.size(); ++i) {
            const std::string_view name =
                play.languageData() ? play.languageData()->name(languages[i]) : std::string_view{};
            std::string& textOut = m_choiceTexts[m_choices.size()];
            textOut.assign(text::format(label, "VD_EXTRAS_LANGUAGE_CHOICE", name));
            m_choices.push_back({textOut, ui::kGold, languages[i] == speaking, true});
            ChatTarget keep = m_chatTarget;
            keep.channel = languages[i]; // the language, for the pick below
            m_choiceTargets.push_back(std::move(keep));
        }
        const i32 picked = choiceList(c, "channels", text::get("VD_SOCIAL_CHAT_TO"), m_choices);
        if (picked >= 0 && static_cast<usize>(picked) >= firstLanguage) {
            play.setChatLanguage(m_choiceTargets[static_cast<u32>(picked)].channel); // phase 18 (extras)
            m_modal = Modal::None;
        } else if (picked >= 0) {
            m_chatTarget = m_choiceTargets[static_cast<u32>(picked)];
            m_modal = Modal::None;
        } else if (picked == -2) {
            m_modal = Modal::None;
        }
        break;
    }
    case Modal::Sheet: {
        m_sheetActions.clear();
        m_sheetActionIds.clear();
        const u64 guid = m_sheetGuid;
        const bool self = guid != 0 && guid == session.playerGuid();
        const Entity* e = guid != 0 ? session.entities().find(guid) : nullptr;
        const auto add = [&](u32 id, std::string_view textIn, bool enabled = true) {
            m_sheetActions.push_back({textIn, enabled, false});
            m_sheetActionIds.push_back(id);
        };
        // What it is: level, race and class when in sight; the guild.
        m_sheetDetail.clear();
        if (e && e->type == game::ObjectType::Player) {
            text::append(m_sheetDetail, "PLAYER_LEVEL", e->value(uf::UNIT_FIELD_LEVEL),
                         raceText(e->byteValue(uf::UNIT_FIELD_BYTES_0, 0)),
                         classText(e->byteValue(uf::UNIT_FIELD_BYTES_0, 1)));
            const std::string_view guild = social.guildName(e->value(uf::PLAYER_GUILDID));
            if (!guild.empty()) {
                appendf(m_sheetDetail, " · <%.*s>", static_cast<int>(guild.size()), guild.data());
            }
        }
        if (!self) {
            const bool member = guid != 0 && social.inGroupWith(guid);
            const bool inGuild = social.guild().id != 0;
            add(kActWhisper, text::get("WHISPER"));
            if (e) {
                add(kActTarget, text::get("TARGET"));
            }
            if (!member) {
                add(kActInvite, text::get("GROUP_INVITE"),
                    !social.group().active || social.isLeader() || social.group().ownAssistant);
            } else if (social.isLeader()) {
                add(kActPromote, text::get("PARTY_PROMOTE"));
                add(kActUninvite, text::get("PARTY_UNINVITE"));
            }
            if (guid != 0 && social.isFriend(guid)) {
                add(kActRemoveFriend, text::get("REMOVE_FRIEND"));
            } else {
                add(kActAddFriend, text::get("ADD_FRIEND"));
            }
            if (guid != 0 && social.isIgnored(guid)) {
                add(kActUnignore, text::get("VD_SOCIAL_UNIGNORE"));
            } else {
                add(kActIgnore, text::get("IGNORE_PLAYER"));
            }
            add(kActDuel, text::get("DUEL"),
                e && e->type == game::ObjectType::Player && social.duel().phase == game::DuelPhase::None);
            add(kActEmote, text::get("EMOTE_MESSAGE"), e != nullptr);
            add(kActTrade, text::get("TRADE"), e && e->type == game::ObjectType::Player); // phase 8 (exchange)
            bool guildMember = false;
            for (const game::GuildMember& m : social.guild().members) {
                guildMember = guildMember || sameName(m.name, m_sheetName);
            }
            if (inGuild && !guildMember && social.guild().can(net::kGuildRightInvite)) {
                add(kActGuildInvite, trimLabel(text::get("CHAT_GUILD_INVITE_SEND"))); // "Invite to guild"
            }
            if (guildMember && social.guild().can(net::kGuildRightPromote)) {
                add(kActGuildPromote, text::get("VD_SOCIAL_GUILD_PROMOTE"));
            }
            if (guildMember && social.guild().can(net::kGuildRightDemote)) {
                add(kActGuildDemote, text::get("VD_SOCIAL_GUILD_DEMOTE"));
            }
            if (guildMember && social.guild().can(net::kGuildRightRemove)) {
                add(kActGuildRemove, trimLabel(text::get("CHAT_GUILD_UNINVITE_SEND"))); // "Remove from guild"
            }
        }
        PlayerSheetView view{m_sheetName, m_sheetDetail, m_sheetActions};
        const i32 picked = playerSheet(c, view);
        if (picked >= 0) {
            sheetAction(session, m_sheetActionIds[static_cast<u32>(picked)]);
        } else if (picked == -2) {
            closeModal();
        }
        break;
    }
    case Modal::Window: window(session, c, text); break;
    case Modal::ChatTabMenu: tabMenu(session, c); break;
    case Modal::RaidInfo: {
        // RaidInfoFrame_Update: GetSavedInstanceInfo's name, id and "Resets in" SecondsToTime(reset).
        const game::Travel& travel = play.travel();
        const std::span<const game::SavedInstance> saved = travel.savedInstances();
        const u32 count = std::min<u32>(static_cast<u32>(saved.size()), kRaidInfos);
        for (u32 i = 0; i < count; ++i) {
            const game::SavedInstance& s = saved[i];
            const game::MapInfo* map = travel.areas() ? travel.areas()->map(s.map) : nullptr;
            std::snprintf(m_raidIds[i].data(), m_raidIds[i].size(), "%u", s.instanceId);
            const i32 left = static_cast<i32>(s.resetAtMs - session.lastUpdateMs());
            char when[64];
            const std::string_view time = game::secondsToTime(when, left > 0 ? static_cast<u32>(left) / 1000u : 0u);
            std::snprintf(m_raidResets[i].data(), m_raidResets[i].size(), "%.*s %.*s",
                          static_cast<int>(text::get("RESETS_IN").size()), text::get("RESETS_IN").data(),
                          static_cast<int>(time.size()), time.data());
            m_raidRows[i] = {map ? map->name : std::string_view(m_raidIds[i].data()), m_raidIds[i].data(),
                             m_raidResets[i].data()};
        }
        if (raidInfoPanel(c, {m_raidRows.data(), count})) {
            m_modal = m_returnToWindow ? Modal::Window : Modal::None;
            m_returnToWindow = false;
        }
        break;
    }
    default: break;
    }
}

// --- The social window -------------------------------------------------------------------------------------------

// Friends frame: FriendsFrame's Friends tab with the 1.12's art and rules: FriendsList_Update and IgnoreList_Update
// (online friends first; the first one selected when none is), the rows' OnClick, FriendsFrame_ShowDropdown, and the
// buttons' OnClick (Add Friend and Ignore Player take a cooperative target or ask its name: ADD_FRIEND / ADD_IGNORE).
void SocialScreens::friendsWindow(GameSession& session, DialogContext& c, ui::TextInput& text) {
    game::Gameplay& play = session.play();
    game::Social& social = play.social();
    const game::QueryCache& queries = play.queries();
    const bool ignore = m_tab == SocialTab::Ignored;
    m_rowGuids.clear();
    m_friendRows.clear();
    m_rowOnline.clear();
    const std::string_view unknown = text::get("UNKNOWN"); // GetFriendInfo / GetIgnoreName before the name is known
    u32 used = 0;
    const auto add = [&](u64 guid, bool online) -> u32 {
        const u32 i = used++;
        const std::string* known = queries.findPlayerName(guid);
        m_rowNames[i].assign(known ? std::string_view(*known) : std::string_view{});
        m_rowDetails[i].clear();
        m_rowRights[i].clear();
        m_rowGuids.push_back(guid);
        m_rowOnline.push_back(online ? 1 : 0);
        return i;
    };
    if (!ignore) {
        for (u32 pass = 0; pass < 2; ++pass) {
            for (const game::FriendInfo& f : social.friends()) {
                const bool online = f.status != net::kFriendOffline;
                if ((pass == 0) != online || used >= m_rowNames.size()) {
                    continue;
                }
                const u32 i = add(f.guid, online);
                const std::string_view name = m_rowNames[i].empty() ? unknown : std::string_view(m_rowNames[i]);
                if (online) { // "Name - Area <AFK>" over "Level 12 Rogue"
                    const std::string_view area = m_socialData ? m_socialData->areaName(f.area) : std::string_view{};
                    const std::string_view status = f.status == net::kFriendAfk   ? text::get("CHAT_FLAG_AFK")
                                                    : f.status == net::kFriendDnd ? text::get("CHAT_FLAG_DND")
                                                                                  : std::string_view{};
                    text::append(m_rowDetails[i], "FRIENDS_LIST_TEMPLATE", name, area, status);
                    text::append(m_rowRights[i], "FRIENDS_LEVEL_TEMPLATE", f.level, classText(f.cls));
                } else { // "Name - Offline" in gray over "Unknown"
                    text::append(m_rowDetails[i], "FRIENDS_LIST_OFFLINE_TEMPLATE", name);
                    m_rowRights[i].assign(unknown);
                }
            }
        }
    } else {
        for (const u64 guid : social.ignored()) {
            if (used >= m_rowNames.size()) {
                break;
            }
            const u32 i = add(guid, false);
            m_rowDetails[i].assign(m_rowNames[i].empty() ? unknown : std::string_view(m_rowNames[i]));
        }
    }
    for (u32 i = 0; i < used; ++i) {
        m_friendRows.push_back({m_rowDetails[i], m_rowRights[i]});
    }
    u64& selectedGuid = ignore ? m_selectedIgnore : m_selectedFriend;
    i32 selected = -1;
    for (u32 i = 0; i < m_rowGuids.size(); ++i) {
        if (m_rowGuids[i] == selectedGuid) {
            selected = static_cast<i32>(i);
        }
    }
    if (selected < 0 && !m_rowGuids.empty()) { // SetSelectedFriend(1) / SetSelectedIgnore(1)
        selected = 0;
        selectedGuid = m_rowGuids[0];
    }
    FriendsFrameView view;
    view.ignore = ignore;
    view.rows = m_friendRows;
    view.selected = selected;
    view.connected = selected >= 0 && m_rowOnline[static_cast<u32>(selected)] != 0;
    view.guildEnabled = social.guild().id != 0;
    const FriendsFrameResult r = friendsFrame(c, view);
    const std::string_view selectedName =
        selected >= 0 ? std::string_view(m_rowNames[static_cast<u32>(selected)]) : std::string_view{};

    if (r.scrolled) {
        m_uiSound(UiSound::ChatScroll);
    }
    if (r.closed) {
        if (text.active()) {
            text.end();
        }
        m_modal = Modal::None;
        return;
    }
    if (r.tab >= 0) {
        switchTab(social, static_cast<SocialTab>(r.tab), text);
        return;
    }
    if (r.showIgnore || r.showFriends) { // FriendsFrameToggleTab2 / IgnoreFrameToggleTab1: FriendsFrame.showFriendsList
        m_tab = r.showIgnore ? SocialTab::Ignored : SocialTab::Friends;
        return;
    }
    if (r.row >= 0 && static_cast<u32>(r.row) < m_rowGuids.size()) { // SetSelectedFriend / SetSelectedIgnore
        selectedGuid = m_rowGuids[static_cast<u32>(r.row)];
        m_uiSound(UiSound::CheckBoxOn);
    }
    if (!ignore && r.rowHeld >= 0 && static_cast<u32>(r.rowHeld) < m_rowGuids.size() && m_unitMenus) {
        // The right button: FriendsFrame_ShowDropdown(name, connected), the FRIEND menu of an online friend.
        const u32 i = static_cast<u32>(r.rowHeld);
        if (m_rowOnline[i] != 0 && !m_rowNames[i].empty()) {
            m_unitMenus->openFriendMenu(session, m_rowNames[i], r.rowRect.x, r.rowRect.bottom(), false);
        }
        return;
    }
    if (r.addFriend || r.ignorePlayer) {
        // UnitCanCooperate("player", "target"): AddFriend / AddIgnore(UnitName("target")); else StaticPopup_Show.
        const std::string_view target = cooperativeTarget(session);
        if (!target.empty()) {
            if (r.addFriend) {
                social.addFriend(target);
            } else {
                social.addIgnore(target);
            }
        } else if (m_popups) {
            if (r.addFriend) {
                m_popups->askAddFriend(session);
            } else {
                m_popups->askAddIgnore(session);
            }
        }
    }
    if (selected >= 0) {
        const u64 guid = m_rowGuids[static_cast<u32>(selected)];
        if (r.removeFriend) {
            social.removeFriend(guid); // FriendsFrame_RemoveFriend
        }
        if (r.stopIgnore) {
            social.removeIgnore(guid); // FriendsFrame_UnIgnore
        }
        if (r.sendMessage && !selectedName.empty()) {
            sendTell(selectedName); // FriendsFrame_SendMessage: ChatFrame_OpenChat("/w name ")
        }
        if (r.groupInvite && !selectedName.empty()) {
            social.inviteToGroup(selectedName); // FriendsFrame_GroupInvite: InviteByName
        }
    }
}

void SocialScreens::switchTab(game::Social& social, SocialTab tab, ui::TextInput& text) {
    // PanelTemplates_Tab_OnClick, then FriendsFrame_OnShow: the friends list (not the ignore list), its sound.
    m_tab = tab;
    if (text.active()) {
        text.end();
    }
    if (tab == SocialTab::Guild) {
        social.requestRoster();
    }
    if (tab == SocialTab::Friends) {
        social.refreshFriends();
    }
    m_uiSound(UiSound::GameMenuOpen);
}

std::string_view SocialScreens::cooperativeTarget(GameSession& session) {
    game::Gameplay& play = session.play();
    const game::EntityStore& entities = session.entities();
    const game::Entity* me = entities.find(session.playerGuid());
    const game::Entity* target = play.target() != 0 ? entities.find(play.target()) : nullptr;
    if (!me || !target || me->type != game::ObjectType::Player || target->type != game::ObjectType::Player) {
        return {};
    }
    if (target->guid != me->guid && game::reactionTo(*me, *target, play.reactions()) != game::Reaction::Friendly) {
        return {};
    }
    const std::string* name = session.playerName(target->guid);
    return name ? std::string_view(*name) : std::string_view{};
}

void SocialScreens::window(GameSession& session, DialogContext& c, ui::TextInput& text) {
    game::Social& social = session.play().social();
    // Retouches (friends frame): every tab is FriendsFrame with its 1.12 art. InGuildCheck: out of a guild its tab is
    // disabled and the window goes back to the first one.
    if (m_tab == SocialTab::Guild && social.guild().id == 0) {
        m_tab = SocialTab::Friends;
    }
    if (m_tab != SocialTab::Who) {
        m_whoColumnMenu = false;
    }
    if (m_tab != SocialTab::Group) {
        m_raid.hidden();
    }
    switch (m_tab) {
    case SocialTab::Who: whoWindow(session, c, text); break;
    case SocialTab::Guild: guildWindow(session, c, text); break;
    case SocialTab::Group: raidWindow(session, c, text); break;
    default: friendsWindow(session, c, text); break;
    }
}

void SocialScreens::closeSocialWindow(ui::TextInput& text) {
    if (text.active()) {
        text.end();
    }
    m_modal = Modal::None;
}

void SocialScreens::sendWho(GameSession& session, std::string_view query) {
    // SendWho: the text read as WoW.exe reads it (game/WhoQuery), then CMSG_WHO.
    game::WhoQueryNames names;
    if (m_socialData) {
        names.races = m_socialData->raceNames();
        names.classes = m_socialData->classNames();
        names.zones = m_socialData->topZones();
        names.zoneNames = m_socialData->topZoneNames();
    }
    game::parseWhoQuery(query, names, m_whoQuery);
    std::array<std::string_view, 4> terms{};
    for (u32 i = 0; i < m_whoQuery.termCount; ++i) {
        terms[i] = m_whoQuery.terms[i];
    }
    net::WhoRequest request;
    request.minLevel = m_whoQuery.minLevel;
    request.maxLevel = m_whoQuery.maxLevel;
    request.name = m_whoQuery.name;
    request.guild = m_whoQuery.guild;
    request.raceMask = m_whoQuery.raceMask;
    request.classMask = m_whoQuery.classMask;
    request.zones = {m_whoQuery.zones.data(), m_whoQuery.zoneCount};
    request.terms = {terms.data(), m_whoQuery.termCount};
    session.play().social().who(request);
    m_whoSelected.clear(); // WhoFrameWhoButton: WhoFrame.selectedWho = nil
}

// Retouches (friends frame): the Who tab as WhoFrame (WhoList_Update, the buttons' OnClick, WhoFrameEditBox).
void SocialScreens::whoWindow(GameSession& session, DialogContext& c, ui::TextInput& text) {
    constexpr ui::WidgetId kWhoField = ui::widgetId("social.who");
    game::Social& social = session.play().social();
    const bool editing = text.active() && text.field() == kWhoField;
    if (editing) {
        m_whoInput.assign(text.text());
    }
    const game::WhoResults& who = social.who();
    // Phase 18 (p3): the columns (the name, WhoFrameDropDown's column, the level, the class) in SortWho's order, sorted
    // again with every answer and every header tapped.
    if (who.version != m_whoSortedVersion || m_whoSort.version() != m_whoSortedKeys) {
        m_whoSortedVersion = who.version;
        m_whoSortedKeys = m_whoSort.version();
        game::WhoSortNames names;
        names.user = this;
        names.zone = [](const void* user, u32 id) {
            const SocialScreens* self = static_cast<const SocialScreens*>(user);
            return self->m_socialData ? self->m_socialData->areaName(id) : std::string_view{};
        };
        names.cls = [](const void* user, u32 id) { return static_cast<const SocialScreens*>(user)->classText(id); };
        names.race = [](const void* user, u32 id) { return static_cast<const SocialScreens*>(user)->raceText(id); };
        game::sortWho(who.entries, m_whoSort, names, m_whoOrder);
    }
    m_whoRows.clear();
    m_rowGuids.clear();
    i32 selected = -1;
    u32 used = 0;
    for (const u16 index : m_whoOrder) {
        if (used >= m_rowNames.size() || index >= who.entries.size()) {
            continue;
        }
        const game::WhoEntry& w = who.entries[index];
        const u32 i = used++;
        m_rowNames[i].assign(w.name);
        m_rowDetails[i].clear();
        appendf(m_rowDetails[i], "%u", w.level);
        const std::string_view zone = m_socialData ? m_socialData->areaName(w.zone) : std::string_view{};
        WhoRowView& r = m_whoRows.emplace_back();
        r.name = m_rowNames[i];
        r.variable = m_whoColumn == 1 ? std::string_view(w.guild) : m_whoColumn == 2 ? raceText(w.race) : zone;
        r.level = m_rowDetails[i];
        r.cls = classText(w.cls);
        if (!m_whoSelected.empty() && m_rowNames[i] == m_whoSelected) {
            selected = static_cast<i32>(i);
        }
    }
    static constexpr const char* kColumnKeys[] = {"ZONE", "GUILD", "RACE"};
    m_whoHeaders[0] = {text::get("NAME"), 83.0f};
    m_whoHeaders[1] = {text::get(kColumnKeys[std::min<u32>(m_whoColumn, 2)]), 105.0f, false, false, true};
    m_whoHeaders[2] = {text::get("LEVEL_ABBR"), 32.0f};
    m_whoHeaders[3] = {text::get("CLASS"), 92.0f};
    // WhoFrameTotals: "3 People Found" and, when the server lists fewer than it found, "(49 displayed)".
    m_subtitle.clear();
    const u32 listed = static_cast<u32>(who.entries.size());
    const u32 found = std::max(who.online, listed);
    text::append(m_subtitle, found == 1 ? "WHO_FRAME_TOTAL_TEMPLATE" : "WHO_FRAME_TOTAL_TEMPLATE_P1", found);
    if (found > listed) {
        m_subtitle.append("  ");
        text::append(m_subtitle, "WHO_FRAME_SHOWN_TEMPLATE", listed);
    }
    WhoFrameView view;
    view.rows = m_whoRows;
    view.selected = selected;
    view.headers = m_whoHeaders;
    view.totals = m_subtitle;
    view.input = m_whoInput;
    view.editing = editing;
    view.guildEnabled = social.guild().id != 0;
    const WhoFrameResult r = whoFrame(c, view);
    if (r.scrolled) {
        m_uiSound(UiSound::ChatScroll);
    }
    if (m_whoColumnMenu) { // WhoFrameDropDown's list: which column it shows (WhoFrameDropDownButton_OnClick)
        for (u32 i = 0; i < 3; ++i) {
            m_whoColumnChoices[i] = {text::get(kColumnKeys[i]), ui::kWhite, i == m_whoColumn};
        }
        const i32 picked = choiceList(c, "social.whocolumn", text::get("WHO"), m_whoColumnChoices);
        if (picked >= 0) {
            m_whoColumn = static_cast<u32>(picked); // shown, not sorted
        }
        if (picked != -1) {
            m_whoColumnMenu = false;
        }
        return;
    }
    if (r.closed) {
        closeSocialWindow(text);
        return;
    }
    if (r.tab >= 0) {
        switchTab(social, static_cast<SocialTab>(r.tab), text);
        return;
    }
    if (r.header >= 0) { // WhoFrameColumnHeaderTemplate's OnClick: SortWho, igMainMenuOptionCheckBoxOn
        static constexpr game::WhoSort kColumnSorts[] = {game::WhoSort::Zone, game::WhoSort::Guild, game::WhoSort::Race};
        m_whoSort.sort(r.header == 0   ? game::WhoSort::Name
                       : r.header == 1 ? kColumnSorts[std::min<u32>(m_whoColumn, 2)]
                       : r.header == 2 ? game::WhoSort::Level
                                       : game::WhoSort::Class);
        m_uiSound(UiSound::CheckBoxOn);
    }
    if (r.headerMenu == 1) {
        m_whoColumnMenu = true;
    }
    if (r.tappedEdit) {
        if (editing) {
            text.end();
        } else {
            text.begin(kWhoField, m_whoInput, 255, ui::TextKind::Plain);
        }
    }
    if ((editing && text.submitted()) || r.refresh) { // WhoFrameEditBox_OnEnterPressed: SendWho, ClearFocus
        if (text.active()) {
            text.end();
        }
        sendWho(session, m_whoInput);
    }
    if (r.row >= 0 && static_cast<usize>(r.row) < m_whoRows.size()) { // FriendsFrameWhoButton_OnClick
        m_whoSelected.assign(m_whoRows[static_cast<u32>(r.row)].name);
        m_uiSound(UiSound::CheckBoxOn);
    }
    if (r.rowHeld >= 0 && static_cast<usize>(r.rowHeld) < m_whoRows.size() && m_unitMenus) {
        // The right button: FriendsFrame_ShowDropdown(name, 1).
        m_unitMenus->openFriendMenu(session, m_whoRows[static_cast<u32>(r.rowHeld)].name, r.rowRect.x,
                                    r.rowRect.bottom(), false);
    }
    if (selected >= 0) {
        if (r.addFriend) {
            social.addFriend(m_whoSelected); // AddFriend(WhoFrame.selectedName)
        }
        if (r.groupInvite) {
            social.inviteToGroup(m_whoSelected); // InviteByName(WhoFrame.selectedName)
        }
    }
}

// Retouches (friends frame): the Guild tab as GuildFrame (app/GuildScreens draws the page and its side frames).
void SocialScreens::guildWindow(GameSession& session, DialogContext& c, ui::TextInput& text) {
    game::Social& social = session.play().social();
    const game::GuildState& guild = social.guild();
    // FriendsFrameTitleText: the guild's name (GetGuildInfo("player")).
    const FriendsFrameShell f =
        beginFriendsFrame(c, FriendsPage::Guild, guild.known ? std::string_view(guild.name) : std::string_view{});
    if (m_guild) {
        m_guild->drawTab(session, c, text, f);
    }
    const i32 tab = endFriendsFrame(c, f, SocialTab::Guild, guild.id != 0);
    if (m_guild) {
        m_guild->drawSideFrame(session, c, text, f);
    }
    if (f.closed) {
        closeSocialWindow(text);
    } else if (tab >= 0) {
        switchTab(social, static_cast<SocialTab>(tab), text);
    }
}

// Retouches (friends frame): the Raid tab as RaidFrame with Blizzard_RaidUI's groups (app/RaidScreens draws the page).
void SocialScreens::raidWindow(GameSession& session, DialogContext& c, ui::TextInput& text) {
    game::Social& social = session.play().social();
    const FriendsFrameShell f = beginFriendsFrame(c, FriendsPage::Raid, text::get("RAID"));
    const RaidScreens::Action action = m_raid.drawTab(session, c, f, {m_unitMenus, m_popups, m_raidPullouts});
    const i32 tab = endFriendsFrame(c, f, SocialTab::Group, social.guild().id != 0);
    if (f.closed) {
        closeSocialWindow(text);
    } else if (tab >= 0) {
        switchTab(social, static_cast<SocialTab>(tab), text);
    } else if (action == RaidScreens::Action::RaidInfo) {
        m_modal = Modal::RaidInfo; // RaidFrameRaidInfoButton: RaidInfoFrame
        m_returnToWindow = true;
    }
}

} // namespace vd
