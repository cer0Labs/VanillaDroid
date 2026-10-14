#pragma once

#include "app/CharCreateScreens.h"
#include "app/ChatConfigScreens.h" // phase 18 (p3)
#include "app/ChatFrame.h"
#include "app/Dialogs.h"
#include "app/RaidScreens.h"
#include "app/SocialWindows.h"
#include "core/Text.h"
#include "core/Types.h"
#include "game/Appearance.h"
#include "game/CharCreate.h"
#include "game/Chat.h"
#include "game/ChatCommands.h"
#include "game/ChatWindows.h" // phase 18 (p3)
#include "game/ListSort.h"    // phase 18 (p3)
#include "game/Social.h"
#include "game/WhoQuery.h" // retouches (friends frame)
#include "ui/TextInput.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vd {

class AuraScreens;
class ChatLinkInsert;
class CombatScreens;
class GameSession;
class GuildScreens;
class PopupScreens;
class Portraits;
class RaidPullouts;
class UiResources;
class UnitMenuScreens;
namespace game {
class GameData;
class SocialData;
} // namespace game

// The social side of the touch UI (phase 8): what the windows of app/SocialWindows show, read from the session's
// game::Social (and Gameplay's chat), and what the player's taps and typed commands do there. GameScreens owns it
// and calls it from the login screen (character creation and deletion) and from the game (chat, party frames,
// invitations, the social window, a player's sheet, emotes). Its texts are keys of core/Text (game/SocialText: the
// original client's strings, VD_SOCIAL_* in locale/*/social.lang). Main thread; storage reserved once (no allocation
// per frame once warmed up).
class SocialScreens {
public:
    // `socialData` / `create` may be null (no db.vdpk): zone channels, emotes and creation are then unavailable.
    void init(const game::GameData* data, game::SocialData* socialData, const game::CharCreateData* create,
              UiResources* resources);

    // --- Before the world ---
    // Phase 18 (create): the creation screen (app/CharCreateScreens: GlueXML's CharacterCreate), started by the
    // character list's Create New Character.
    void startCreation() { m_charCreate.start(); }
    bool creating() const { return m_charCreate.active(); }
    // The creation screen instead of the character list (call while creating()): it fills `scene` (the race's glue
    // scene and the character being made) and plays its sounds through `sound`.
    void drawCreation(GameSession& session, DialogContext& c, ui::TextInput& text, GlueSceneRequest& scene,
                      const UiSoundSink& sound) {
        m_charCreate.draw(session, c, text, scene, sound);
    }
    // "Delete" on the list: a confirmation. True while it is shown (draw nothing else modal then).
    void askDelete(u64 guid, std::string_view name);
    bool drawDeleteConfirm(GameSession& session, DialogContext& c);
    // The character being created (nullptr when not creating), and the way it faces (radians, added to "facing the
    // camera"): without the glue scenes the game shows it in front of the world's camera.
    const game::CharacterLook* previewLook() const { return m_charCreate.look(); }
    f32 previewTurn() const { return m_charCreate.facingRadians(); }

    // --- In the world ---
    // The area the player stands in (world/Terrain::areaAt): the zone channels follow it.
    void setArea(u32 areaId) { m_areaId = areaId; }
    // Once a frame before drawing: the chat's new lines, the social events (messages, invitations).
    void update(GameSession& session, u32 nowMs);
    // Parity: the chat (not modal) as 1.12's docked chat frames (app/ChatFrame): ChatFrame1 at the bottom left,
    // left of `actionBar` (the HUD's action buttons) and clear of the party frames, its General, Combat Log (from
    // setCombatLog), Whispers, Party and Guild tabs. Also offline with VD_HUD_DEMO (sample lines, development).
    void chat(GameSession& session, DialogContext& c, ui::TextInput& text, const ui::Rect& actionBar, u32 nowMs);
    // Where the Combat Log tab (ChatFrame2) gets its lines and filter (GameScreens' CombatScreens).
    void setCombatLog(CombatScreens* combatLog) { m_combatLog = combatLog; }
    // Everything the chat drew this frame (other HUD text keeps clear of it), and whether it is open.
    const ui::Rect& chatRect() const { return m_chatRect; }
    bool chatExpanded() const { return m_chatExpanded; }
    const ui::Rect& partyRect() const { return m_partyRect; } // the party frames this frame (empty: none)
    // Phase 18 (extras): the hardware keys. OPENCHAT (Enter): the edit box opens on its chat type (ChatFrame_OpenChat);
    // Escape closes the open chat (ChatEdit_OnEscapePressed) or a step drawModal shows (a sheet, a menu, a question:
    // CloseMenus); true when one was. The creation screen's keys (CharacterCreate_OnKeyDown).
    void openChat() {
        m_focusInput = true;
        m_returnToWindow = false;
    }
    bool collapseChat() { return std::exchange(m_chatExpanded, false); }
    bool escapeModal() {
        if (!wantsPopup()) {
            return false;
        }
        closeModal();
        return true;
    }
    void creationKey(GlueKey key) { m_charCreate.key(key); }
    // Party frames under the player frame, from `top` (not modal). Returns the height they took.
    f32 partyFrames(GameSession& session, DialogContext& c, f32 top);
    // Phase 20 (assists, docs/MOBILE_CONTROLS.md §5.3 / §8): a healer's touch template brings the party frames into
    // `dock` (their bottom right at its bottom right, `left`: their bottom left at its bottom left: beside the touch
    // cluster); empty: under the player frame.
    void setPartyDock(const ui::Rect& dock, bool left) {
        m_partyDock = dock;
        m_partyDockLeft = left;
    }
    // ...and the member (or member's pet) whose frame is under (x, y) as last drawn, 0 none; `frame` its rectangle.
    u64 partyUnitAt(f32 x, f32 y, ui::Rect& frame) const;
    // Parity: where the party frames get their members' portraits (null: the race pictures).
    void setPortraits(Portraits* portraits) { m_portraits = portraits; }
    // Parity (auras): the party frames' debuffs and their aura list (app/AuraScreens; null: none).
    void setAuras(AuraScreens* auras) { m_auras = auras; }
    // Phase 15 (pets): UIOptionsFrame's "Show Party Members' Pets" (SHOW_PARTY_PETS, default "1"): the party frames show
    // the members' pets (PartyMemberPetFrame).
    void setShowPartyPets(bool show) { m_showPartyPets = show; }
    // Phase 15 (options): GUILDMEMBER_ALERT (guildMemberNotify): the guild's members signing on and off in the chat.
    void setGuildMemberAlert(bool on) { m_guildMemberAlert = on; }
    bool showPartyPets() const { return m_showPartyPets; }
    // Phase 13 (social): the players' menus and the C functions of the slash commands (FollowUnit, TargetByName,
    // InspectUnit...: app/UnitMenuScreens), and the popups some commands ask (CONFIRM_GUILD_DISBAND).
    void setUnitMenus(UnitMenuScreens* menus) { m_unitMenus = menus; }
    void setPopups(PopupScreens* popups) { m_popups = popups; }
    // Phase 15 (guild): who draws the guild tab (GuildFrame and its side frames: app/GuildScreens; null: the list), and
    // the social tables it names zones and classes with (null without them).
    void setGuild(GuildScreens* guild) { m_guild = guild; }
    // Phase 16 (raids): the pullout frames a group label or class button dragged out of the raid tab opens.
    void setRaidPullouts(RaidPullouts* pullouts) { m_raidPullouts = pullouts; }
    const game::SocialData* socialData() const { return m_socialData; }
    // ChatFrame_SendTell: the chat opens to whisper `name` ("Tell Aeryn: "), the keyboard with it.
    void sendTell(std::string_view name);
    // Phase 15 (options): EXECUTE_CHAT_LINE, a macro's line as if typed in the chat and sent (ChatEdit_SendText): a
    // command, an emote, or text said in the chat's current channel, which the line does not change (the edit box's
    // sticky type is only set by Enter). And /macro asked for the macro window (read once).
    void runLine(GameSession& session, std::string_view line);
    bool takeMacroWanted() { return std::exchange(m_macroWanted, false); }
    // Phase 15 (options): a line was said since the last call (CLEAR_AFK clears AFK upon talking).
    bool takeSpoke() { return std::exchange(m_spoke, false); }
    // Phase 15 (options): HIDE_PARTY_INTERFACE (no party frames in a raid) and SHOW_PARTY_BACKGROUND.
    void setPartyOptions(bool hideInRaid, bool background) {
        m_hidePartyInRaid = hideInRaid;
        m_partyBackground = background;
    }
    // One modal window at a time: invitations, the social window, a player's sheet, the emote menu, the channel
    // picker. True when one is up (wantsModal()) and was drawn.
    bool wantsModal() const;
    bool windowOpen() const { return m_modal == Modal::Window; } // the social window itself (phase 9: its sounds)
    void drawModal(GameSession& session, DialogContext& c, ui::TextInput& text, u32 nowMs);
    void openWindow(SocialTab tab = SocialTab::Friends);
    // Parity pass (panels): the social window (FriendsFrame) is a panel: open while it or what it opened over itself
    // (a player's sheet, Raid Info) is up; drawn in the place of `c` by drawWindow. drawModal draws the rest
    // (wantsPopup: invitations, a sheet, the emote menu, the channel picker, questions).
    bool windowWanted() const { return m_modal == Modal::Window || m_returnToWindow; }
    bool wantsPopup() const { return m_modal != Modal::None && m_modal != Modal::Window; }
    void drawWindow(GameSession& session, DialogContext& c, ui::TextInput& text) { window(session, c, text); }
    void closeWindow();
    // The actions on a player (a tap on a player's target frame, a party frame, a name in the chat).
    void openSheet(GameSession& session, u64 guid, std::string_view name);
    // Messages for the middle of the screen (GameScreens shows them with its own).
    struct Message {
        std::array<char, 128> text{};
        u32 color = 0;
        bool big = false;
    };
    bool popMessage(Message& out);
    // Phase 8: a line of another module for the chat's system lines (loot rolls, mail, auctions).
    void systemNotice(u32 color, std::string_view text);
    // Phase 18 (p3): ...of a message group (game::ChatGroup: LOOT, SKILL, MONEY, COMBAT_XP_GAIN...): the windows that
    // listen to it show it.
    void systemNotice(u32 color, std::string_view text, u8 group);
    // Phase 18 (p3): the chat windows (game::ChatWindows: chat-cache.txt), their menus (app/ChatConfigScreens), and
    // NAME_CHAT's answer (a window renamed, or a new one).
    game::ChatWindows& chatWindows() { return m_chatWindows; }
    ChatConfigScreens& chatConfig() { return m_chatConfig; }
    // Phase 18 (p3): the interface sounds (the who list's column headers; the chat menus' check marks).
    void setUiSoundSink(UiSoundSink sink) {
        m_uiSound = sink;
        m_chatConfig.setUiSoundSink(sink);
    }
    void nameChatWindow(u32 chatFrame, std::string_view name);
    // A new character in the world (or none): everything about the last one is forgotten.
    void reset();

private:
    enum class Modal : u8 {
        None,
        Window,
        Sheet,
        Emotes,
        Channels,
        Confirm, // parity (popups): invitations and challenges are StaticPopups now (app/PopupScreens)
        ChatTabMenu, // parity: a chat tab's menu (FCFOptionsDropDown)
        RaidInfo,    // phase 12: RaidInfoFrame (the group tab's Raid Info, /raidinfo)
    };
    enum class Confirm : u8 { None, LeaveGuild, RemoveFriend, Unignore, GuildRemove };
    // Chat categories: what the tabs show.
    enum Category : u32 {
        kCatSay = 1u << 0,
        kCatWhisper = 1u << 1,
        kCatParty = 1u << 2,
        kCatGuild = 1u << 3,
        kCatChannel = 1u << 4,
        kCatSystem = 1u << 5,
        kCatEmote = 1u << 6,
        kCatAll = 0xFFFFFFFFu,
    };
    // Phase 18 (p3): a channel's own messages (CHAT_MSG_CHANNEL) go by each window's channel list, not by a group.
    static constexpr u8 kChannelMessage = 0xFF;
    struct ChatEntry {
        std::string text;
        u32 color = 0;
        u32 category = 0;
        u64 sender = 0; // a player to act on (0: none)
        std::string senderName;
        bool nameMissing = false; // formatted before the sender's name arrived: again when it does
        game::ChatLine raw;       // the line, to format it again
        u32 timeMs = 0;           // when it came in (its fading)
        u32 key = 0;              // the same line across frames (app/ChatFrame's widgets)
        std::array<ChatFrameLink, 4> links{}; // its |H..|h links (game/ChatLinks)
        u32 linkCount = 0;
        u8 group = 0;        // phase 18 (p3): its message group (game::ChatGroup), or kChannelMessage
        std::string channel; // phase 18 (p3): kChannelMessage: the channel without its zone
    };
    struct ChatTarget {
        net::ChatType type = net::ChatType::Say;
        std::string target; // whisper name
        u32 channel = 0;    // number
    };

    // Chat.
    void readChat(GameSession& session);
    void formatLine(GameSession& session, ChatEntry& entry);
    ChatEntry& addLine(u32 category, u32 color, u8 group = 0, std::string_view channel = {});
    bool shows(const game::ChatWindowConfig& window, const ChatEntry& e) const; // phase 18 (p3)
    void indexLinks(ChatEntry& entry);
    // Parity: the chat frame's parts.
    void chatTabs(u32 nowMs, u32& selected);
    void chatLines(u32 nowMs);
    std::string_view editHeader(GameSession& session, std::array<char, 96>& buffer);
    void touchChat(u32 nowMs);
    void flashTabs(const ChatEntry& e); // phase 18 (p3): the windows that show it
    void openLink(GameSession& session, std::string_view line, u32 at, const ui::Rect& where);
    void tabMenu(GameSession& session, DialogContext& c);
    void demoChat(GameSession& session, u32 nowMs);
    // A line of the chat's system category and a message for the middle of the screen: the text of `key`
    // (core/Text) with its arguments, or a text already built.
    template <typename... Args> void systemLine(u32 color, text::Key key, const Args&... args);
    void systemText(u32 color, std::string_view text);
    template <typename... Args> void message(u32 color, bool big, text::Key key, const Args&... args);
    void messageText(u32 color, bool big, std::string_view text);
    void readSocialEvents(GameSession& session, u32 nowMs);
    void execute(GameSession& session, const game::ChatCommand& cmd);
    // Phase 13 (social): the slash commands of the rest of SlashCmdList.
    void executeMore(GameSession& session, const game::ChatCommand& cmd);
    void castByName(GameSession& session, std::string_view text); // CastSpellByName("Fireball(Rank 3)")
    std::string_view channelName(GameSession& session, const game::ChatCommand& cmd); // a number's name
    void linkTapped(GameSession& session, std::string_view line, u32 at, const ui::Rect& where, bool held);
    void sendTo(GameSession& session, const ChatTarget& to, std::string_view text);
    void help();
    // Players.
    std::string_view raceText(u32 race) const;  // ChrRaces' name (empty without the tables)
    std::string_view classText(u32 cls) const; // ChrClasses' name
    std::string_view targetName(GameSession& session);
    u64 playerInSight(GameSession& session, std::string_view name);
    u64 guidByName(GameSession& session, std::string_view name);
    void sheetAction(GameSession& session, u32 action);
    void closeModal(); // back to the social window when it opened the dialog
    // The social window. Retouches (friends frame): FriendsFrame with each tab's 1.12 art: the Friends tab
    // (FriendsListFrame and IgnoreListFrame), Who (WhoFrame), Guild (GuildFrame: app/GuildScreens) and Raid
    // (RaidFrame: app/RaidScreens); a tab chosen (FriendsFrameTabTemplate's OnClick); SendWho; and
    // UnitCanCooperate("player", "target")'s name (empty: none).
    void window(GameSession& session, DialogContext& c, ui::TextInput& text);
    void friendsWindow(GameSession& session, DialogContext& c, ui::TextInput& text);
    void whoWindow(GameSession& session, DialogContext& c, ui::TextInput& text);
    void guildWindow(GameSession& session, DialogContext& c, ui::TextInput& text);
    void raidWindow(GameSession& session, DialogContext& c, ui::TextInput& text);
    void closeSocialWindow(ui::TextInput& text);
    void sendWho(GameSession& session, std::string_view query);
    void switchTab(game::Social& social, SocialTab tab, ui::TextInput& text);
    std::string_view cooperativeTarget(GameSession& session);

    const game::GameData* m_data = nullptr;
    game::SocialData* m_socialData = nullptr;
    UiResources* m_resources = nullptr;
    bool m_trace = false;

    // Character creation (phase 18: app/CharCreateScreens) and deletion.
    CharCreateScreens m_charCreate;
    u32 m_resultVersion = 0;
    std::string m_createStatus;
    u64 m_deleteGuid = 0;
    std::string m_deleteName;
    bool m_waitingDelete = false;

    // Chat.
    static constexpr u32 kChatLines = 160;
    std::vector<ChatEntry> m_chatLines; // ring
    u32 m_chatNext = 0;
    u32 m_chatCount = 0;
    u32 m_chatCursor = 0;
    u32 m_socialCursor = 0;
    u32 m_queryVersion = 0;
    // Parity: the docked chat frames (app/ChatFrame). Phase 18 (p3): the chat windows of chat-cache.txt.
    static constexpr u32 kDocks = game::kChatWindows; // ChatFrame1..7
    game::ChatWindows m_chatWindows;
    ChatConfigScreens m_chatConfig;
    std::array<std::array<char, 48>, kDocks> m_tabNames{}; // the default names (CHAT_NAME_TEMPLATE)
    std::vector<ChatFrameLine> m_chatViews;
    std::vector<u32> m_chatViewEntries; // the ring entry behind each view line (kNoEntry: the combat log's)
    std::array<ChatFrameTab, kDocks> m_tabViews{};
    std::array<u8, kDocks> m_tabDocks{}; // the dock behind each tab shown
    u32 m_tabCount = 0;
    std::array<u32, kDocks> m_flashSince{}; // FCF_FlashTab: when a tab's flash started (0: none)
    u8 m_dock = 0;                          // the selected frame
    u8 m_menuDock = 0;                      // the tab whose menu is open
    u32 m_lineKey = 0;                      // the last key given to a line
    u32 m_revealMs = 0;                     // the last time a tap brought every line back
    u32 m_activeUntilMs = 0;                // the background shows until then (mouse-over)
    u32 m_lastChatMs = 0;
    u32 m_visibleMs = 0;                    // a line's time before it fades (VD_CHAT_VISIBLE_MS in development)
    u32 m_nowMs = 0;
    f32 m_backgroundAlpha = 0.0f;
    ChatFrameState m_chatState;
    CombatScreens* m_combatLog = nullptr;
    ui::Rect m_partyRect; // where the party frames were drawn this frame (the chat keeps clear of them)
    bool m_chatDemo = false; // VD_HUD_DEMO: sample lines without a server
    bool m_chatDemoFilled = false;
    bool m_chatExpanded = false;
    ui::Rect m_chatRect;
    std::string m_chatInput;
    ChatTarget m_chatTarget;   // the edit box's chat type (chatType)
    ChatTarget m_stickyTarget; // phase 16: the one it goes back to after a line (stickyType: SAY, PARTY, RAID, GUILD)
    // ChatFrame_OpenChat: a sticky party or raid type with no party or raid any more speaks in SAY this time.
    void openChatType(GameSession& session);
    std::string m_lastWhisper; // who whispered last (/r)
    u64 m_cursorsPlayer = 0;
    u32 m_areaId = 0;

    // Windows.
    Modal m_modal = Modal::None;
    bool m_returnToWindow = false;
    Confirm m_confirm = Confirm::None;
    std::string m_confirmName;
    u64 m_confirmGuid = 0;
    SocialTab m_tab = SocialTab::Friends;
    std::vector<u64> m_rowGuids;
    std::vector<std::string> m_rowNames;   // names behind the rows
    std::vector<std::string> m_rowDetails; // reused strings of the frame
    std::vector<std::string> m_rowRights;
    // Retouches (friends frame): its rows, whether each friend is online, and the selected friend and ignored player
    // (FriendsFrame.selectedFriend / selectedIgnore, by GUID: the online ones come first and move).
    std::vector<FriendRowView> m_friendRows;
    std::vector<u8> m_rowOnline;
    u64 m_selectedFriend = 0, m_selectedIgnore = 0;
    std::string m_subtitle; // retouches (friends frame): WhoFrameTotals
    // Retouches (friends frame): WhoFrameEditBox, the rows of WhoFrame, WhoFrame.selectedName and the last query read.
    std::string m_whoInput;
    std::vector<WhoRowView> m_whoRows;
    std::string m_whoSelected;
    game::WhoQuery m_whoQuery;
    // Phase 18 (p3): the who list's sort (SortWho's keys for the whole session, as WoW.exe keeps them, and the order of
    // the last answer) and WhoFrameDropDown's column (WHOFRAME_DROPDOWN_LIST: 0 zone, 1 guild, 2 race).
    game::WhoSortKeys m_whoSort = game::whoSortDefaults();
    std::vector<u16> m_whoOrder;
    u32 m_whoSortedVersion = ~0u, m_whoSortedKeys = ~0u;
    u32 m_whoColumn = 0;
    bool m_whoColumnMenu = false;
    std::array<SortHeaderView, 4> m_whoHeaders{};
    std::array<ChoiceView, 3> m_whoColumnChoices{};
    UiSoundSink m_uiSound;
    // A player's sheet.
    u64 m_sheetGuid = 0;
    std::string m_sheetName;
    std::string m_sheetDetail;
    std::vector<SheetActionView> m_sheetActions;
    std::vector<u32> m_sheetActionIds;
    std::vector<ChoiceView> m_choices;
    // Phase 12: the raid information's rows (MAX_RAID_INFOS = 10), their ids and reset times.
    static constexpr u32 kRaidInfos = 10;
    std::array<RaidInfoRowView, kRaidInfos> m_raidRows{};
    std::array<std::array<char, 16>, kRaidInfos> m_raidIds{};
    std::array<std::array<char, 96>, kRaidInfos> m_raidResets{};
    std::vector<ChatTarget> m_choiceTargets;
    std::vector<std::string> m_choiceTexts;
    std::vector<PartyFrameView> m_partyViews;
    std::array<game::PartyMemberView, 4> m_partyMembers{};
    std::array<ui::Rect, 8> m_partyFrames{}; // phase 20 (assists): the members' frames, then their pets'
    std::array<u64, 8> m_partyGuids{};
    ui::Rect m_partyDock;
    bool m_partyDockLeft = false;
    Portraits* m_portraits = nullptr;
    AuraScreens* m_auras = nullptr;
    bool m_showPartyPets = true; // phase 15 (pets): SHOW_PARTY_PETS
    bool m_guildMemberAlert = false; // phase 15 (options): GUILDMEMBER_ALERT
    UnitMenuScreens* m_unitMenus = nullptr; // phase 13 (social)
    PopupScreens* m_popups = nullptr;
    GuildScreens* m_guild = nullptr; // phase 15 (guild)
    RaidPullouts* m_raidPullouts = nullptr; // phase 16 (raids)
    RaidScreens m_raid;     // phase 16 (raidframe): the raid tab (the social window's Group tab)
    bool m_raidSeen = false; // phase 16 (raidframe): in a raid when the events were last read
    ChatLinkInsert* m_chatLink = nullptr;    // the chat's edit box (DialogContext::chatLink), from chat()
    bool m_focusInput = false; // sendTell: the edit box opens on the next chat()
    bool m_guildInfoWanted = false; // /ginfo asked: the next SMSG_GUILD_INFO prints its lines
    bool m_macroWanted = false;     // phase 15 (options): /macro
    bool m_spoke = false;           // phase 15 (options): a chat line sent
    bool m_hidePartyInRaid = false;
    bool m_partyBackground = false;
    std::string m_outgoing;    // what a typed line goes out as (game::escapeChatInput)

    std::array<Message, 8> m_messages{};
    u32 m_messageFirst = 0, m_messageCount = 0;
    u32 m_duelShownSecond = 0; // the countdown's last number shown
    bool m_lastAfk = false, m_lastDnd = false;
    std::string m_afkMessage, m_dndMessage; // what /afk and /dnd said (MARKED_AFK_MESSAGE, MARKED_DND)
    i32 m_lootMethodSeen = -1, m_lootThresholdSeen = -1; // the group's loot rules last told (-1: none yet)
};

} // namespace vd
