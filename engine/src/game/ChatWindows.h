#pragma once

#include "core/Types.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vd::game {

// Phase 18 (p3): the chat windows as WoW.exe 1.12.1 keeps them (ChatFrame.cpp, read-only disassembly: the defaults
// 0x4984C6, the reader of chat-cache.txt 0x498A60, its writer 0x499A80) and FloatingChatFrame.lua changes them
// (FCF_OpenNewWindow, FCF_Close, FCF_SetWindowName, FCF_ToggleLock, FCF_SetChatWindowFontSize, FCF_SetWindowColor /
// Alpha, ChatFrame_AddMessageGroup / RemoveMessageGroup, ChatFrame_AddChannel / RemoveChannel). One file per character
// (WTF\Account\<ACCOUNT>\<realm>\<character>\chat-cache.txt), the account data type 7 kept on the server.

constexpr u32 kChatWindowSlots = 10; // WoW.exe's table (0x5F0 / 0x98): the file always lists ten
constexpr u32 kChatWindows = 7;      // NUM_CHAT_WINDOWS: ChatFrame1..7
constexpr u32 kChatGroups = 68;      // the message groups of WoW.exe's table 0x805FB0
constexpr u32 kChatCacheVersion = 2; // VERSION and ADDEDVERSION it writes

// The message groups by WoW.exe's index (the order of its table, the names the file uses).
enum ChatGroup : u8 {
    kGroupSystem = 0,
    kGroupSay = 1,
    kGroupYell = 2,
    kGroupWhisper = 3,
    kGroupParty = 4,
    kGroupGuild = 5,
    kGroupCreature = 6,
    kGroupChannel = 7, // the channels' join / leave / notices (their messages go by each window's channel list)
    kGroupSkill = 8,
    kGroupLoot = 9,
    kGroupCombatMiscInfo = 10,
    kGroupCombatXpGain = 29,
    kGroupCombatHonorGain = 65,
    kGroupCombatFactionChange = 66,
    kGroupMoney = 67,
};

struct ChatGroupInfo {
    const char* name;  // its name in chat-cache.txt and ChatTypeGroup
    bool byDefault;    // on in a new character's windows: ChatFrame1 for 0..9, ChatFrame2 (Combat Log) after
    u8 addedVersion;   // the ADDEDVERSION that brought it (an older file gets the default ones added)
    bool combat;       // ours: a line of the combat log (app/CombatScreens' store) rather than of the chat
    bool others;       // ours: about other players or creatures (the combat log's "everyone" lines)
};
const ChatGroupInfo& chatGroupInfo(u32 group);
i32 chatGroupIndex(std::string_view name); // -1: unknown
// The label a filter menu shows: ChatTypeGroup's own name for the groups of several message types (SAY, WHISPER...),
// else its one CHAT_MSG_<name> (FCFDropDown_LoadChatTypes). A core/Text key.
const char* chatGroupLabelKey(u32 group);

// The filter menus of FCFOptionsDropDown (ChatFrame.lua's *MenuChatTypeGroups), as group indexes.
enum class ChatMenu : u8 { Channels, Combat, Spell, SpellOther, Periodic, Other, Count };
std::span<const u8> chatMenuGroups(ChatMenu menu);
const char* chatMenuLabelKey(ChatMenu menu); // CHANNELS, COMBAT_MESSAGES, SPELL_MESSAGES...

struct ChatWindowConfig {
    std::string name;  // empty: GENERAL, COMBAT_LOG or CHAT_NAME_TEMPLATE (FCF_SetWindowName's defaults)
    u32 fontSize = 0;  // SIZE: 0 the font's own (14), else one of CHAT_FONT_HEIGHTS
    u8 r = 0, g = 0, b = 0, a = 0; // COLOR: the background (a: 0..255)
    bool locked = false;
    u32 docked = 0; // DOCKED: its place in the dock (1 first), 0 floating
    bool shown = false;
    std::array<bool, kChatGroups> groups{};
    std::vector<std::string> channels; // ChatFrame_AddChannel: channel names without their zone
    u32 zoneChannels = 0;              // the zone channels' bits it listens to (written, kept)

    bool hasGroup(u32 group) const { return group < kChatGroups && groups[group]; }
    bool hasChannel(std::string_view channel) const;
    bool anyCombat() const;       // a combat group on: the combat log's lines show in it
    bool combatOthers() const;    // ...about others too (else only the player's and its pet's)
};

class ChatWindows {
public:
    ChatWindows() { setDefaults(); }
    // A character without chat-cache.txt (WoW.exe 0x4984C6): ChatFrame1 shown and docked first with SYSTEM..LOOT,
    // ChatFrame2 shown and docked second with the combat groups (and MONEY, COMBAT_FACTION_CHANGE), no others.
    void setDefaults();
    // chat-cache.txt as WoW.exe reads it: false when nothing could be read (the defaults stay). A file older than
    // ADDEDVERSION 2 gets the groups added since switched on in ChatFrame1 (groups 0..9) or ChatFrame2.
    bool parse(std::string_view text);
    // ...and writes it.
    std::string format() const;

    ChatWindowConfig& window(u32 index) { return m_windows[index < kChatWindowSlots ? index : 0]; }
    const ChatWindowConfig& window(u32 index) const { return m_windows[index < kChatWindowSlots ? index : 0]; }
    // The docked windows in dock order (FCF_DockUpdate: DOCKED_CHAT_FRAMES), at most kChatWindows; returns how many.
    u32 docked(std::span<u8> out) const;
    // FCF_GetNumActiveChatFrames: shown or docked.
    u32 active() const;

    // FCF_OpenNewWindow: the first window neither shown nor docked (or the last), named (empty: CHAT_NAME_TEMPLATE),
    // black at DEFAULT_CHATFRAME_ALPHA, unlocked, listening to SAY, YELL, GUILD, WHISPER, PARTY and CHANNEL, shown and
    // docked last. Returns its index, -1 when every one is in use.
    i32 openNew(std::string_view name);
    // FCF_Close: hidden and out of the dock (ChatFrame1 cannot be closed).
    void close(u32 index);
    void setName(u32 index, std::string_view name) { window(index).name.assign(name.substr(0, 31)); }
    void toggleGroup(u32 index, u32 group);
    void toggleChannel(u32 index, std::string_view channel);

    // The kept parts of the file this client does not use (written back as read).
    std::vector<std::string>& joinedChannels() { return m_channels; }
    bool changed() const { return m_changed; }
    void markChanged() { m_changed = true; }
    void clearChanged() { m_changed = false; }

private:
    void redock(); // the dock's places 1..n again, in order

    std::array<ChatWindowConfig, kChatWindowSlots> m_windows{};
    std::string m_guildRecruitment = "AUTO"; // OPTION_GUILD_RECRUITMENT_CHANNEL
    std::vector<std::string> m_channels;     // CHANNELS: the channels joined (not the zone ones)
    u32 m_zoneChannels = 0;
    std::vector<std::string> m_colors; // COLORS: "<TYPE> r g b" lines, kept as they are
    bool m_changed = false;
};

} // namespace vd::game
