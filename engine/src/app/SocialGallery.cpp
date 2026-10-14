// UI gallery pages of the social windows (phase 8): character creation, party frames, the chat with tabs, emotes,
// a player's sheet, the social window and the channel picker, with sample data (development: VD_UI_GALLERY=create,
// party, socialchat, emotes, sheet, social, channels). The windows' own words come from their keys as in the game
// (VD_LOCALE=esES shows them in Spanish); names, zones and what players say are English sample data.
#include "app/ChatFrame.h"
#include "app/GuildWindows.h"
#include "app/RaidWindows.h"
#include "app/SocialWindows.h"
#include "app/UiGallery.h"

#include "core/Hash.h"
#include "core/Log.h"
#include "core/Text.h"
#include "game/ChatLinks.h"
#include "ui/ChatDock.h"
#include "ui/Widgets.h"

#include <array>
#include <cstdio>
#include <string>

namespace vd {
namespace {

using ui::rgba;

void logResult(const char* what) {
    VD_LOG_INFO("UI gallery: %s", what);
}

// What the sample pages keep between frames (one gallery at a time).
struct SocialGalleryState {
    u32 tab = 0;
    bool expanded = true;
    SocialTab socialTab = SocialTab::Friends;
    i32 selected = -1;
    u32 channel = 0;
    std::array<std::string, 24> texts; // built from the keys each frame (storage reused)
    ChatFrameState chat;
    std::array<std::array<ChatFrameLink, 4>, 16> links{};
    u64 flashStart = 0;
};
SocialGalleryState& state() {
    static SocialGalleryState s;
    return s;
}

// texts[i] rebuilt: `head` (sample data), the key's text with its arguments, then `tail`. Views into it stay valid
// until it is rebuilt next frame.
template <typename... Args>
std::string_view keyed(u32 i, std::string_view head, text::Key key, std::string_view tail, const Args&... args) {
    std::string& out = state().texts[i];
    out.assign(head);
    text::append(out, key, args...);
    out.append(tail);
    return out;
}

// A client string that leads into something ("Invite to guild: ") as a label.
std::string_view trimLabel(std::string_view s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == ':')) {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

void UiGallery::socialPage(DialogContext& c, u32 page) {
    SocialGalleryState& s = state();
    char text[96];
    switch (page) {
    case 0: // create: phase 18 (create), app/CharCreateGallery (its other pages by name)
        m_charCreate.select("create");
        m_charCreate.draw(c, *m_text);
        break;
    case 1: { // party frames and the collapsed chat
        static const PartyFrameView kParty[] = {
            {"Aeryn", 12, 0.8f, 0.6f, 0, 4, 1, true, false, false, false, false, true, true, true},
            {"Brakk", 13, 0.35f, 0.9f, 1, 3, 0, true, false, false, true, false, false, true, false},
            {"Tolvir", 11, 0.0f, 0.0f, 0, 7, 0, true, true, true, false, false, false, false, false},
            {"Miriel", 12, 0.0f, 0.0f, 3, 1, 1, false, false, false, false, false, false, false, false},
        };
        ui::Gui g{c.ui, c.draw};
        const ui::Rect& safe = c.safeArea;
        const i32 tapped = partyFrames(c, {safe.x + g.px(4.0f), safe.y + g.unit(100.0f), 0.0f, 0.0f}, kParty);
        if (tapped >= 0) {
            std::snprintf(text, sizeof(text), "party tapped %d", tapped);
            logResult(text);
        }
        break;
    }
    case 2: { // the chat (app/ChatFrame): the lines as game/SocialText builds them, players and items linked
        char item[128];
        keyed(9, "[2. Trade] ", "CHAT_CHANNEL_GET", "WTS ", "|Hplayer:Galen|h[Galen]|h");
        s.texts[9].append(game::formatItemLink(item, 2132, "|cff1eff00", "Militia Shortsword"));
        s.texts[9].append(" 1g");
        ChatFrameLine lines[] = {
            {keyed(0, {}, "CHAT_YOU_JOINED_NOTICE", {}, "1. General - Elwynn Forest"), rgba(255, 192, 192)},
            {keyed(1, "[1. General] ", "CHAT_CHANNEL_GET", "anyone for Hogger? need 2", "|Hplayer:Tolvir|h[Tolvir]|h"),
             rgba(255, 192, 192)},
            {keyed(2, {}, "CHAT_SAY_GET", "Hello everyone", "|Hplayer:Arthel|h[Arthel]|h"), ui::kWhite},
            {keyed(3, {}, "CHAT_WHISPER_GET", "coming to the mine?", "|Hplayer:Aeryn|h[Aeryn]|h"), rgba(255, 128, 255)},
            {keyed(4, {}, "CHAT_WHISPER_INFORM_GET", "be there in a minute", "|Hplayer:Aeryn|h[Aeryn]|h"),
             rgba(255, 128, 255)},
            {"Aeryn waves at you.", rgba(255, 128, 64)},
            {keyed(5, {}, "VD_SOCIAL_GROUP_JOINED", {}), rgba(255, 255, 0)},
            {keyed(6, {}, "CHAT_PARTY_GET", "the kobolds at the entrance are mine", "|Hplayer:Aeryn|h[Aeryn]|h"),
             rgba(170, 170, 255)},
            {keyed(7, {}, "CHAT_GUILD_GET", "anyone got Linen Cloth?", "|Hplayer:Brakk|h[Brakk]|h"), rgba(64, 255, 64)},
            {keyed(8, {}, "ERR_FRIEND_ONLINE_SS", {}, "Brakk", "Brakk"), rgba(64, 255, 64)},
            {s.texts[9], rgba(255, 192, 192)},
            {keyed(10, {}, "RANDOM_ROLL_RESULT", {}, "Arthel", 57, 1, 100), rgba(255, 255, 0)},
        };
        for (u32 i = 0; i < std::size(lines); ++i) {
            u32 n = 0;
            game::ChatLink found;
            for (usize from = 0; n < 4 && game::nextChatLink(lines[i].text, from, found); from = found.end) {
                s.links[i][n++] = {static_cast<u16>(found.begin), static_cast<u16>(found.end)};
            }
            lines[i].key = i + 1;
            lines[i].links = {s.links[i].data(), n};
        }
        const u64 now = c.ui.nowMs();
        if (s.flashStart == 0) {
            s.flashStart = now; // a whisper came while General was shown: its tab flashes (FCF_FlashTab)
        }
        const ChatFrameTab tabs[] = {{text::get("GENERAL")},
                                     {text::get("COMBAT_LOG")},
                                     {text::get("CHAT_MSG_WHISPER_INFORM"),
                                      s.tab == 2 ? 0.0f : ui::flashAlpha(now - s.flashStart)},
                                     {text::get("PARTY")},
                                     {text::get("GUILD")}};
        ChatFrameView v;
        v.lines = lines;
        v.tabs = tabs;
        v.selected = s.tab;
        v.expanded = s.expanded;
        v.backgroundAlpha = 1.0f;
        v.header = keyed(11, {}, "CHAT_CHANNEL_SEND", {}, 2, "Trade");
        v.headerColor = rgba(255, 192, 192);
        // Phase 13: a bag item linked into the edit box (the desktop's Shift + click): drawn as the link shows.
        char typed[256], linked[160];
        std::snprintf(typed, sizeof(typed), "WTS %s 1g",
                      game::formatItemLink(linked, 15210, "|cff1eff00", "Raider Shortsword", 0, 1127, "of the Monkey")
                          .data());
        v.input = typed;
        v.editing = true;
        v.hint = text::get("VD_SOCIAL_CHAT_HINT");
        const ChatFrameResult r = chatFrame(c, chatFrameRect(c, s.expanded, {}, {}, false), v, s.chat);
        if (r.tab >= 0) {
            s.tab = static_cast<u32>(r.tab);
            logResult("chat tab");
        }
        if (r.tabMenu >= 0) {
            logResult("chat tab menu");
        }
        if (r.linkLine >= 0) {
            std::snprintf(text, sizeof(text), "chat link at %u of line %d", r.linkBegin, r.linkLine);
            logResult(text);
        } else if (r.line >= 0) {
            std::snprintf(text, sizeof(text), "chat line %d", r.line);
            logResult(text);
        }
        if (r.toggle) {
            s.expanded = !s.expanded;
        }
        if (r.pickChannel) {
            logResult("chat channel picker");
        }
        if (r.emotes) {
            logResult("chat emotes");
        }
        break;
    }
    case 3: { // emotes
        struct Sample {
            const char* key;
            std::string_view token;
        };
        static constexpr Sample kEmotes[] = {
            {"VD_SOCIAL_EMOTE_WAVE", "wave"},   {"VD_SOCIAL_EMOTE_HELLO", "hello"},
            {"VD_SOCIAL_EMOTE_BOW", "bow"},     {"VD_SOCIAL_EMOTE_DANCE", "dance"},
            {"VD_SOCIAL_EMOTE_CHEER", "cheer"}, {"VD_SOCIAL_EMOTE_LAUGH", "laugh"},
            {"VD_SOCIAL_EMOTE_CLAP", "clap"},   {"VD_SOCIAL_EMOTE_THANK", "thank"},
            {"VD_SOCIAL_EMOTE_NOD", "nod"},     {"VD_SOCIAL_EMOTE_NO", "no"},
            {"VD_SOCIAL_EMOTE_CRY", "cry"},     {"VD_SOCIAL_EMOTE_SIT", "sit"},
            {"VD_SOCIAL_EMOTE_SLEEP", "sleep"}, {"VD_SOCIAL_EMOTE_HEALME", "healme"},
            {"VD_SOCIAL_EMOTE_OOM", "oom"},
        };
        std::array<EmoteButtonView, std::size(kEmotes)> emotes;
        for (u32 i = 0; i < emotes.size(); ++i) {
            emotes[i] = {text::get(kEmotes[i].key), kEmotes[i].token};
        }
        const i32 picked = emoteMenu(c, emotes, "Aeryn");
        if (picked >= 0) {
            std::snprintf(text, sizeof(text), "emote %s", std::string(kEmotes[picked].token).c_str());
            logResult(text);
        }
        break;
    }
    case 4: { // a player's sheet
        const SheetActionView actions[] = {
            {text::get("WHISPER")},       {text::get("TARGET")},
            {text::get("GROUP_INVITE")},  {text::get("ADD_FRIEND")},
            {text::get("IGNORE_PLAYER")}, {text::get("DUEL")},
            {text::get("EMOTE_MESSAGE")}, {trimLabel(text::get("CHAT_GUILD_INVITE_SEND"))},
        };
        const PlayerSheetView v{"Aeryn", keyed(0, {}, "PLAYER_LEVEL", " · <The Wanderers>", 12, "Night Elf", "Rogue"),
                                actions};
        const i32 picked = playerSheet(c, v);
        if (picked >= 0) {
            std::snprintf(text, sizeof(text), "sheet action %d", picked);
            logResult(text);
        }
        break;
    }
    case 5: { // the social window, every tab
        char ago[48], offlineFor[96];
        const std::string_view offlineDays =
            text::format(offlineFor, "VD_SOCIAL_OFFLINE_FOR", text::format(ago, "LASTONLINE_DAYS_P1", 3));
        if (s.socialTab == SocialTab::Friends || s.socialTab == SocialTab::Ignored) {
            // Retouches (friends frame): FriendsListFrame and IgnoreListFrame with the 1.12's art.
            const std::string_view unknown = text::get("UNKNOWN");
            const FriendRowView friends[] = {
                {keyed(14, {}, "FRIENDS_LIST_TEMPLATE", {}, "Aeryn", "Elwynn Forest", ""),
                 keyed(15, {}, "FRIENDS_LEVEL_TEMPLATE", {}, 12, "Rogue")},
                {keyed(16, {}, "FRIENDS_LIST_TEMPLATE", {}, "Brakk", "Stormwind City", text::get("CHAT_FLAG_AFK")),
                 keyed(17, {}, "FRIENDS_LEVEL_TEMPLATE", {}, 13, "Warrior")},
                {keyed(18, {}, "FRIENDS_LIST_TEMPLATE", {}, "Miriel", "Westfall", ""),
                 keyed(19, {}, "FRIENDS_LEVEL_TEMPLATE", {}, 18, "Priest")},
                {keyed(20, {}, "FRIENDS_LIST_OFFLINE_TEMPLATE", {}, "Tolvir"), unknown},
            };
            const FriendRowView ignored[] = {{"Spammer", {}}, {"Goldseller", {}}};
            FriendsFrameView v;
            v.ignore = s.socialTab == SocialTab::Ignored;
            v.rows = v.ignore ? std::span<const FriendRowView>(ignored) : std::span<const FriendRowView>(friends);
            v.selected = std::max(s.selected, 0);
            v.connected = v.ignore || v.selected < 3;
            v.guildEnabled = false; // a player out of a guild
            const FriendsFrameResult r = friendsFrame(c, v);
            if (r.tab >= 0) {
                s.socialTab = static_cast<SocialTab>(r.tab);
                s.selected = -1;
                logResult("social tab");
            }
            if (r.showIgnore || r.showFriends) {
                s.socialTab = r.showIgnore ? SocialTab::Ignored : SocialTab::Friends;
                s.selected = -1;
            }
            if (r.row >= 0) {
                s.selected = r.row;
            }
            if (r.addFriend || r.removeFriend || r.sendMessage || r.groupInvite || r.ignorePlayer || r.stopIgnore) {
                logResult("friends frame button");
            }
            if (r.closed) {
                logResult("friends frame closed");
            }
            break;
        }
        // Retouches (friends frame): the other tabs on FriendsFrame's art too (WhoFrame, GuildFrame, RaidFrame).
        const auto tabTapped = [&](i32 tab) {
            if (tab >= 0) {
                s.socialTab = static_cast<SocialTab>(tab);
                s.selected = -1;
                logResult("social tab");
            }
        };
        if (s.socialTab == SocialTab::Who) {
            const WhoRowView who[] = {
                {"Aeryn", "Elwynn Forest", "12", "Rogue"},
                {"Galen", "Stormwind City", "15", "Mage"},
                {"Miriel", "Westfall", "18", "Priest"},
            };
            const SortHeaderView headers[] = {
                {text::get("NAME")}, {text::get("ZONE"), 1.0f, false, false, true}, {text::get("LEVEL_ABBR")},
                {text::get("CLASS")}};
            WhoFrameView v;
            v.rows = who;
            v.headers = headers;
            v.selected = s.selected;
            v.totals = keyed(11, {}, "WHO_FRAME_TOTAL_TEMPLATE_P1", {}, 3);
            v.input = "z-\"Elwynn Forest\" 10-20";
            v.guildEnabled = true;
            const WhoFrameResult r = whoFrame(c, v);
            tabTapped(r.tab);
            if (r.row >= 0) {
                s.selected = r.row;
            }
            if (r.refresh || r.addFriend || r.groupInvite) {
                logResult("who frame button");
            }
            if (r.closed) {
                logResult("social closed");
            }
            break;
        }
        if (s.socialTab == SocialTab::Guild) {
            const GuildRowView guild[] = {
                {{"Brakk", "Stormwind City", "13", "Warrior"}},
                {{"Aeryn", "Elwynn Forest", "12", "Rogue"}},
                {{"Tolvir", offlineDays, "9", "Hunter"}, false},
            };
            const FriendsFrameShell f = beginFriendsFrame(c, FriendsPage::Guild, "The Wanderers");
            GuildTabView v;
            v.totals = keyed(11, {}, "GUILD_TOTAL_P1", {}, 3);
            v.online = keyed(12, {}, "GUILD_TOTALONLINE", {}, 2);
            v.headers = {text::get("NAME"), text::get("ZONE"), text::get("LEVEL_ABBR"), text::get("CLASS")};
            v.rows = guild;
            v.selected = s.selected;
            v.toggleLabel = text::get("GUILD_STATUS");
            v.motd = "Deadmines run on Friday at 9 pm. Bring potions.";
            v.motdEditable = true;
            v.addEnabled = true;
            const GuildTabResult r = guildTab(c, f.l, f.s, v);
            if (r.row >= 0) {
                s.selected = r.row;
            }
            tabTapped(endFriendsFrame(c, f, SocialTab::Guild, true));
            if (f.closed) {
                logResult("social closed");
            }
            break;
        }
        // Phase 16 (raidframe): a party's leader out of a raid (raid: in one).
        static RaidTabDrag drag;
        const FriendsFrameShell f = beginFriendsFrame(c, FriendsPage::Raid, text::get("RAID"));
        RaidTabView raid;
        raid.convertEnabled = true;
        raid.raidInfoEnabled = true;
        const RaidTabResult rr = raidTab(c, f.l, f.s, f.panel, raid, drag);
        if (rr.convert || rr.raidInfo) {
            logResult(rr.convert ? "social raid tab: convert" : "social raid tab: raid info");
        }
        tabTapped(endFriendsFrame(c, f, SocialTab::Group, true));
        if (f.closed) {
            logResult("social closed");
        }
        break;
    }
    case 6: { // the channel picker
        const ChoiceView choices[] = {
            {text::get("CHAT_MSG_SAY"), ui::kWhite},
            {text::get("CHAT_MSG_YELL"), rgba(255, 64, 64)},
            {text::get("CHAT_MSG_PARTY"), rgba(170, 170, 255)},
            {text::get("CHAT_MSG_GUILD"), rgba(64, 255, 64)},
            {text::get("CHAT_MSG_OFFICER"), rgba(64, 192, 64)},
            {"1. General - Elwynn Forest", rgba(255, 192, 192), true},
            {"3. LocalDefense - Elwynn Forest", rgba(255, 192, 192)},
            {"4. LookingForGroup", rgba(255, 192, 192)},
            {trimLabel(keyed(0, {}, "CHAT_WHISPER_SEND", {}, "Aeryn")), rgba(255, 128, 255)},
        };
        const i32 picked = choiceList(c, "channels", text::get("VD_SOCIAL_CHAT_TO"), choices);
        if (picked >= 0) {
            std::snprintf(text, sizeof(text), "channel %d", picked);
            logResult(text);
        }
        break;
    }
    default: break;
    }
}

} // namespace vd
