#pragma once

#include "core/Types.h"
#include "net/GameMessages.h"

#include <string_view>

namespace vd::game {

// What a line typed in the chat asks for (phase 8): the useful subset of the original ChatFrame's slash commands,
// with their English names and Spanish aliases. Plain text goes to the chat's current channel; a leading '.' is a
// server command (GM accounts) and goes out as said.
enum class ChatCommandKind : u8 {
    None,         // nothing to do (empty line)
    Message,      // text in `type` (Whisper: `target` is the name; Channel: `channel` is the number)
    SetChannel,   // a channel command without text: the chat's channel becomes `type` / `channel` / `target`
    Reply,        // /r: whisper `text` to whoever whispered last
    TextEmote,    // /wave, /dance...: `target` is the token (EmotesText name)
    Join,         // /join name [password]: `target`, `text`
    Leave,        // /leave name-or-number: `target` (`channel` when a number)
    ChannelList,  // /chatlist name-or-number
    Invite,       // /invite [name] (empty: the target)
    Uninvite,     // /kick [name]
    LeaveGroup,   // /leaveparty
    Promote,      // /promote [name]
    Afk,          // /afk [message]
    Dnd,          // /dnd [message]
    Who,          // /who [text]
    AddFriend,    // /friend [name]
    RemoveFriend, // /removefriend name
    Ignore,       // /ignore [name]
    Unignore,     // /unignore name
    GuildInvite,  // /ginvite name
    GuildRemove,  // /gremove name
    GuildPromote, // /gpromote name
    GuildDemote,  // /gdemote name
    GuildLeave,   // /gquit
    GuildMotd,    // /gmotd text
    GuildRoster,  // /groster: the guild tab
    Duel,         // /duel [name]
    Forfeit,      // /forfeit
    Roll,         // /roll [min] [max]: `minimum`, `maximum`
    Help,         // /help
    Unknown,      // /xyz that is no command nor emote: `target` is the word
    // Parity (popups): SLASH_LOGOUT and SLASH_QUIT (Logout(), Quit(): the CAMP / QUIT countdown).
    Logout, // /logout, /camp
    Quit,   // /quit, /exit
    // Phase 12: SLASH_RAID_INFO (the saved raids: RaidInfoFrame, or NO_RAID_INSTANCES_SAVED in the chat).
    RaidInfo, // /raidinfo
    // Phase 13 (social): the rest of ChatFrame.lua's SlashCmdList. `target` is the name typed (empty: the target,
    // as GetSlashCmdTarget falls back to it), the channel commands' `target` the channel (a name or a number) and
    // `text` what follows it (a player, a password).
    Follow,             // /follow, /f, /fol [name]: FollowByName, or FollowUnit("target")
    Assist,             // /assist, /a [name]: AssistByName, or AssistUnit("target")
    TargetByName,       // /target, /tar name: the nearest unit so named (TargetByName)
    Inspect,            // /inspect, /ins: InspectUnit("target")
    Trade,              // /trade, /tr: InitiateTrade("target")
    Pvp,                // /pvp: TogglePVP
    Played,             // /played: RequestTimePlayed
    Time,               // /time: the game's time (GameTime_GetTime)
    Cast,               // /cast, /spell name[(Rank n)]: `text` (CastSpellByName)
    GuildLeader,        // /gleader, /guildleader name
    GuildDisband,       // /gdisband, /guilddisband: CONFIRM_GUILD_DISBAND (the guild master only)
    ReadyCheck,         // /readycheck (a raid's leader)
    LootFreeForAll,     // /ffa
    LootRoundRobin,     // /roundrobin
    LootMaster,         // /master name
    ChatHelp,           // /chat, /chathelp: CHAT_HELP_TEXT_LINE1..
    ChannelPassword,    // /password, /pass channel [password]
    ChannelOwner,       // /owner channel [player]: shows the owner without a player
    ChannelModerator,   // /mod, /moderator channel player
    ChannelUnmoderator, // /unmod, /unmoderator channel player
    ChannelMute,        // /mute, /squelch, /unvoice channel player
    ChannelUnmute,      // /unmute, /unsquelch, /voice channel player
    ChannelInvite,      // /cinvite, /chatinvite channel player
    ChannelKick,        // /ckick channel player
    ChannelBan,         // /ban channel player
    ChannelUnban,       // /unban channel player
    ChannelAnnounce,    // /announce, /ann channel
    ChannelModerate,    // /moderate channel
    Script,             // /script, /run: Lua, which this client has not
    GuildInfo,          // /ginfo, /guildinfo: GuildInfo (the server's answer prints two lines)
    // Phase 15 (options): SLASH_MACRO and SLASH_MACROHELP.
    Macro,     // /macro, /m: ShowMacroFrame (1.12's RunMacro(msg) is commented out: always the window)
    MacroHelp, // /macrohelp: MACRO_HELP_TEXT_LINE1..
};

struct ChatCommand {
    ChatCommandKind kind = ChatCommandKind::None;
    net::ChatType type = net::ChatType::Say;
    std::string_view target; // a player, a channel's name or an emote token
    std::string_view text;
    u32 channel = 0; // 1..10
    u32 minimum = 1, maximum = 100;
};

// Parses one line of the chat input. `current` is the chat's channel (where plain text goes), with `currentTarget`
// (whisper name) and `currentChannel` (number) when it is a whisper or a channel. Views point into `input`.
ChatCommand parseChatInput(std::string_view input, net::ChatType current, std::string_view currentTarget = {},
                           u32 currentChannel = 0);

} // namespace vd::game
