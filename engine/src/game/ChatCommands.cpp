#include "game/ChatCommands.h"

#include "core/Text.h"

#include <climits>
#include <cstdio>
#include <span>

namespace vd::game {
namespace {

using K = ChatCommandKind;
using net::ChatType;

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

// The first word and the rest (trimmed).
std::string_view firstWord(std::string_view s, std::string_view& rest) {
    s = trim(s);
    const usize space = s.find(' ');
    if (space == std::string_view::npos) {
        rest = {};
        return s;
    }
    rest = trim(s.substr(space + 1));
    return s.substr(0, space);
}

bool same(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        char c = a[i];
        c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        if (c != b[i]) {
            return false;
        }
    }
    return true;
}

bool parseNumber(std::string_view s, u32& out) {
    if (s.empty() || s.size() > 9) {
        return false;
    }
    u32 v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + static_cast<u32>(c - '0');
    }
    out = v;
    return true;
}

struct Command {
    std::string_view names[6];
    K kind;
    ChatType type = ChatType::Say;
};

// Commands that send to a chat type (and set the channel when used without text).
constexpr Command kChannels[] = {
    {{"s", "say", "decir", "d"}, K::Message, ChatType::Say},
    {{"y", "yell", "sh", "shout", "gritar"}, K::Message, ChatType::Yell},
    {{"p", "party", "grupo"}, K::Message, ChatType::Party},
    {{"g", "guild", "gc", "gu", "hermandad", "h"}, K::Message, ChatType::Guild},
    {{"o", "officer", "osay", "oficial"}, K::Message, ChatType::Officer},
    {{"ra", "raid", "rsay", "banda"}, K::Message, ChatType::Raid},
    {{"e", "em", "me", "emote"}, K::Message, ChatType::Emote},
    {{"rw", "avisobanda"}, K::Message, ChatType::RaidWarning},               // phase 13: SLASH_RAID_WARNING
    {{"bg", "battleground", "cb"}, K::Message, ChatType::Battleground}, // phase 13: SLASH_BATTLEGROUND
};

// Commands with arguments of their own.
constexpr Command kCommands[] = {
    {{"r", "reply", "responder"}, K::Reply},
    {{"join", "chan", "channel", "unirse", "unirme"}, K::Join},
    {{"leave", "chatleave", "chatexit", "salir"}, K::Leave},
    {{"chatlist", "chatwho", "chatinfo", "canal"}, K::ChannelList},
    {{"invite", "inv", "i", "invitar"}, K::Invite},
    {{"uninvite", "kick", "u", "un", "expulsar"}, K::Uninvite},
    {{"leaveparty", "leavegroup", "abandonar", "dejargrupo"}, K::LeaveGroup},
    {{"promote", "pr", "ascender", "lider"}, K::Promote},
    {{"afk", "away", "ausente"}, K::Afk},
    {{"dnd", "busy", "ocupado"}, K::Dnd},
    {{"who", "quien"}, K::Who},
    {{"friend", "amigo", "addfriend"}, K::AddFriend},
    {{"removefriend", "remfriend", "unfriend", "quitaramigo"}, K::RemoveFriend},
    {{"ignore", "ignorar"}, K::Ignore},
    {{"unignore", "designorar"}, K::Unignore},
    {{"ginvite", "guildinvite", "hinvitar"}, K::GuildInvite},
    {{"gremove", "guildremove", "gkick", "hexpulsar"}, K::GuildRemove},
    {{"gpromote", "guildpromote", "hascender"}, K::GuildPromote},
    {{"gdemote", "guilddemote", "hdegradar"}, K::GuildDemote},
    {{"gquit", "guildquit", "gleave", "hsalir"}, K::GuildLeave},
    {{"gmotd", "guildmotd", "hmotd"}, K::GuildMotd},
    {{"groster", "guildroster", "hlista"}, K::GuildRoster},
    {{"duel", "duelo", "retar"}, K::Duel},
    {{"forfeit", "yield", "concede", "rendirse"}, K::Forfeit},
    {{"roll", "random", "rnd", "rand", "dados"}, K::Roll},
    {{"help", "ayuda", "?", "h?"}, K::Help},
    {{"logout", "camp", "desconectar"}, K::Logout}, // parity (popups)
    {{"quit", "exit", "cerrarjuego"}, K::Quit},
    {{"raidinfo", "infobanda"}, K::RaidInfo}, // phase 12
    // Phase 13 (social): SLASH_* of GlobalStrings.lua, then the Spanish aliases.
    {{"follow", "f", "fol", "seguir"}, K::Follow},
    {{"assist", "a", "asistir"}, K::Assist},
    {{"target", "tar", "objetivo"}, K::TargetByName},
    {{"inspect", "ins", "inspeccionar"}, K::Inspect},
    {{"trade", "tr", "comerciar"}, K::Trade},
    {{"pvp", "jcj"}, K::Pvp},
    {{"played", "jugado"}, K::Played},
    {{"time", "hora"}, K::Time},
    {{"cast", "spell", "lanzar"}, K::Cast},
    {{"gleader", "guildleader", "hlider"}, K::GuildLeader},
    {{"gdisband", "guilddisband", "hdisolver"}, K::GuildDisband},
    {{"readycheck", "listos"}, K::ReadyCheck},
    {{"ffa", "librecomercio"}, K::LootFreeForAll},
    {{"roundrobin", "porturnos"}, K::LootRoundRobin},
    {{"master", "maestro"}, K::LootMaster},
    {{"chat", "chathelp", "chatayuda"}, K::ChatHelp},
    {{"password", "pass", "contrasena"}, K::ChannelPassword},
    {{"owner", "propietario"}, K::ChannelOwner},
    {{"mod", "moderator", "moderador"}, K::ChannelModerator},
    {{"unmod", "unmoderator", "quitarmoderador"}, K::ChannelUnmoderator},
    {{"mute", "squelch", "unvoice", "silenciar"}, K::ChannelMute},
    {{"unmute", "unsquelch", "voice", "quitarsilencio"}, K::ChannelUnmute},
    {{"cinvite", "chatinvite", "cinvitar"}, K::ChannelInvite},
    {{"ckick", "cexpulsar"}, K::ChannelKick},
    {{"ban", "vetar"}, K::ChannelBan},
    {{"unban", "quitarveto"}, K::ChannelUnban},
    {{"announce", "ann", "anunciar"}, K::ChannelAnnounce},
    {{"moderate", "moderar"}, K::ChannelModerate},
    {{"script", "run"}, K::Script},
    {{"ginfo", "guildinfo", "hinfo"}, K::GuildInfo},
    {{"macro", "m", "macros"}, K::Macro}, // phase 15 (options)
    {{"macrohelp", "ayudamacros"}, K::MacroHelp},
};

const Command* findCommand(std::span<const Command> table, std::string_view word) {
    for (const Command& c : table) {
        for (const std::string_view name : c.names) {
            if (!name.empty() && same(word, name)) {
                return &c;
            }
        }
    }
    return nullptr;
}

// Phase 21 (languages): the GlobalStrings.lua names of a command (SLASH_SAY1 = "/s", SLASH_SAY4 = "/decir"...), those
// of the client's language as ChatFrame.lua reads them.
const char* slashFamily(const Command& c) {
    if (c.kind == K::Message) {
        switch (c.type) {
        case ChatType::Say: return "SLASH_SAY";
        case ChatType::Yell: return "SLASH_YELL";
        case ChatType::Party: return "SLASH_PARTY";
        case ChatType::Guild: return "SLASH_GUILD";
        case ChatType::Officer: return "SLASH_OFFICER";
        case ChatType::Raid: return "SLASH_RAID";
        case ChatType::Emote: return "SLASH_EMOTE";
        case ChatType::RaidWarning: return "SLASH_RAID_WARNING";
        case ChatType::Battleground: return "SLASH_BATTLEGROUND";
        default: return nullptr;
        }
    }
    switch (c.kind) {
    case K::Reply: return "SLASH_REPLY";
    case K::Join: return "SLASH_JOIN";
    case K::Leave: return "SLASH_LEAVE";
    case K::ChannelList: return "SLASH_LIST_CHANNEL";
    case K::Invite: return "SLASH_INVITE";
    case K::Uninvite: return "SLASH_UNINVITE";
    case K::Promote: return "SLASH_PROMOTE";
    case K::Afk: return "SLASH_CHAT_AFK";
    case K::Dnd: return "SLASH_CHAT_DND";
    case K::Who: return "SLASH_WHO";
    case K::AddFriend: return "SLASH_FRIENDS";
    case K::RemoveFriend: return "SLASH_REMOVEFRIEND";
    case K::Ignore: return "SLASH_IGNORE";
    case K::Unignore: return "SLASH_UNIGNORE";
    case K::GuildInvite: return "SLASH_GUILD_INVITE";
    case K::GuildRemove: return "SLASH_GUILD_UNINVITE";
    case K::GuildPromote: return "SLASH_GUILD_PROMOTE";
    case K::GuildDemote: return "SLASH_GUILD_DEMOTE";
    case K::GuildLeave: return "SLASH_GUILD_LEAVE";
    case K::GuildMotd: return "SLASH_GUILD_MOTD";
    case K::GuildRoster: return "SLASH_GUILD_ROSTER";
    case K::Duel: return "SLASH_DUEL";
    case K::Forfeit: return "SLASH_DUEL_CANCEL";
    case K::Roll: return "SLASH_RANDOM";
    case K::Help: return "SLASH_HELP";
    case K::Logout: return "SLASH_LOGOUT";
    case K::Quit: return "SLASH_QUIT";
    case K::RaidInfo: return "SLASH_RAID_INFO";
    case K::Follow: return "SLASH_FOLLOW";
    case K::Assist: return "SLASH_ASSIST";
    case K::TargetByName: return "SLASH_TARGET";
    case K::Inspect: return "SLASH_INSPECT";
    case K::Trade: return "SLASH_TRADE";
    case K::Pvp: return "SLASH_PVP";
    case K::Played: return "SLASH_PLAYED";
    case K::Time: return "SLASH_TIME";
    case K::Cast: return "SLASH_CAST";
    case K::GuildLeader: return "SLASH_GUILD_LEADER";
    case K::GuildDisband: return "SLASH_GUILD_DISBAND";
    case K::ReadyCheck: return "SLASH_READYCHECK";
    case K::LootFreeForAll: return "SLASH_LOOT_FFA";
    case K::LootRoundRobin: return "SLASH_LOOT_ROUNDROBIN";
    case K::LootMaster: return "SLASH_LOOT_MASTER";
    case K::ChatHelp: return "SLASH_CHAT_HELP";
    case K::ChannelPassword: return "SLASH_CHAT_PASSWORD";
    case K::ChannelOwner: return "SLASH_CHAT_OWNER";
    case K::ChannelModerator: return "SLASH_CHAT_MODERATOR";
    case K::ChannelUnmoderator: return "SLASH_CHAT_UNMODERATOR";
    case K::ChannelMute: return "SLASH_CHAT_MUTE";
    case K::ChannelUnmute: return "SLASH_CHAT_UNMUTE";
    case K::ChannelInvite: return "SLASH_CHAT_CINVITE";
    case K::ChannelKick: return "SLASH_CHAT_KICK";
    case K::ChannelBan: return "SLASH_CHAT_BAN";
    case K::ChannelUnban: return "SLASH_CHAT_UNBAN";
    case K::ChannelAnnounce: return "SLASH_CHAT_ANNOUNCE";
    case K::ChannelModerate: return "SLASH_CHAT_MODERATE";
    case K::Script: return "SLASH_SCRIPT";
    case K::GuildInfo: return "SLASH_GUILD_INFO";
    case K::Macro: return "SLASH_MACRO";
    case K::MacroHelp: return "SLASH_MACROHELP";
    default: return nullptr;
    }
}

// The lowest n of `family`<n> that names "/word" (SLASH_SAY1..10), INT_MAX when none does or there is no table.
int slashRank(std::string_view word, const char* family) {
    if (!family || !text::loaded()) {
        return INT_MAX;
    }
    for (int n = 1; n <= 10; ++n) {
        char key[48];
        const int length = std::snprintf(key, sizeof(key), "%s%d", family, n);
        const text::Key k(std::string_view(key, static_cast<usize>(length > 0 ? length : 0)));
        if (!text::has(k)) {
            continue;
        }
        const std::string_view name = text::get(k);
        if (name.size() == word.size() + 1 && name.front() == '/' && same(word, name.substr(1))) {
            return n;
        }
    }
    return INT_MAX;
}

// What the client's language names `word` (phase 21): a chat type, a whisper, /c N, or a command of the table. The
// lowest number wins (the esES client names "/s" both SLASH_SAY1 and SLASH_WHISPER1, "/g" SLASH_PARTY1 and
// SLASH_GUILD5), and at the same number this order: the chat types, the whisper, /c, the commands.
struct SlashMatch {
    enum class What : u8 { None, Channel, Whisper, ChannelNumber, Command } what = What::None;
    const Command* command = nullptr;
};

SlashMatch resolveSlash(std::string_view word) {
    SlashMatch best;
    int bestRank = INT_MAX;
    const auto consider = [&](int rank, SlashMatch::What what, const Command* command) {
        if (rank < bestRank) {
            bestRank = rank;
            best = {what, command};
        }
    };
    for (const Command& c : kChannels) {
        consider(slashRank(word, slashFamily(c)), SlashMatch::What::Channel, &c);
    }
    consider(slashRank(word, "SLASH_WHISPER"), SlashMatch::What::Whisper, nullptr);
    consider(slashRank(word, "SLASH_CHANNEL"), SlashMatch::What::ChannelNumber, nullptr);
    for (const Command& c : kCommands) {
        consider(slashRank(word, slashFamily(c)), SlashMatch::What::Command, &c);
    }
    return best;
}

} // namespace

ChatCommand parseChatInput(std::string_view input, ChatType current, std::string_view currentTarget,
                           u32 currentChannel) {
    ChatCommand cmd;
    input = trim(input);
    if (input.empty()) {
        return cmd;
    }
    if (input.front() != '/') {
        // Plain text (or a '.' server command, sent as said): the chat's channel.
        cmd.kind = K::Message;
        const bool gm = input.front() == '.';
        cmd.type = gm ? ChatType::Say : current;
        cmd.target = gm ? std::string_view{} : currentTarget;
        cmd.channel = gm ? 0 : currentChannel;
        cmd.text = input;
        return cmd;
    }
    std::string_view rest;
    const std::string_view word = firstWord(input.substr(1), rest);
    if (word.empty()) {
        cmd.kind = K::Unknown;
        return cmd;
    }

    // /1 .. /10: the numbered channels.
    u32 number = 0;
    if (parseNumber(word, number)) {
        if (number == 0 || number > 10) {
            cmd.kind = K::Unknown;
            cmd.target = word;
            return cmd;
        }
        cmd.kind = rest.empty() ? K::SetChannel : K::Message;
        cmd.type = ChatType::Channel;
        cmd.channel = number;
        cmd.text = rest;
        return cmd;
    }
    // Phase 21: the client's language names the command (SLASH_*), else the names of the tables below.
    using What = SlashMatch::What;
    const SlashMatch slash = resolveSlash(word);
    const bool byName = slash.what == What::None;
    // Phase 13: SLASH_CHANNEL, /c and /csay N text (ChatEdit_ExtractChannel): as /N text.
    if ((slash.what == What::ChannelNumber || (byName && (same(word, "c") || same(word, "csay")))) && !rest.empty()) {
        std::string_view text;
        const std::string_view first = firstWord(rest, text);
        if (parseNumber(first, number) && number >= 1 && number <= 10) {
            cmd.kind = text.empty() ? K::SetChannel : K::Message;
            cmd.type = ChatType::Channel;
            cmd.channel = number;
            cmd.text = text;
            return cmd;
        }
    }
    // Whispers: /w name [text].
    if (slash.what == What::Whisper ||
        (byName && (same(word, "w") || same(word, "whisper") || same(word, "t") || same(word, "tell") ||
                    same(word, "susurrar") || same(word, "su")))) {
        std::string_view text;
        const std::string_view name = firstWord(rest, text);
        if (name.empty()) {
            cmd.kind = K::Unknown;
            cmd.target = word;
            return cmd;
        }
        cmd.kind = text.empty() ? K::SetChannel : K::Message;
        cmd.type = ChatType::Whisper;
        cmd.target = name;
        cmd.text = text;
        return cmd;
    }
    if (const Command* c = slash.what == What::Channel ? slash.command
                           : byName                    ? findCommand(kChannels, word)
                                                       : nullptr) {
        cmd.kind = rest.empty() && c->type != ChatType::Emote ? K::SetChannel : K::Message;
        cmd.type = c->type;
        cmd.text = rest;
        if (cmd.kind == K::Message && rest.empty()) {
            cmd.kind = K::None; // an empty emote
        }
        return cmd;
    }
    const Command* c = slash.what == What::Command ? slash.command
                       : byName                    ? findCommand(kCommands, word)
                                                   : nullptr;
    if (!c) {
        // Any other word may be a text emote (/wave, /dance, /bow...): the caller checks EmotesText.
        cmd.kind = K::TextEmote;
        cmd.target = word;
        cmd.text = rest;
        return cmd;
    }
    cmd.kind = c->kind;
    switch (c->kind) {
    case K::Join: {
        std::string_view password;
        cmd.target = firstWord(rest, password);
        cmd.text = password;
        if (cmd.target.empty()) {
            cmd.kind = K::Unknown;
            cmd.target = word;
        }
        break;
    }
    case K::Leave:
    case K::ChannelList: {
        std::string_view more;
        cmd.target = firstWord(rest, more);
        if (parseNumber(cmd.target, number)) {
            cmd.channel = number;
        }
        break;
    }
    case K::Roll: {
        std::string_view second;
        const std::string_view first = firstWord(rest, second);
        u32 a = 0, b = 0;
        if (parseNumber(first, a) && parseNumber(firstWord(second, second), b)) {
            cmd.minimum = a;
            cmd.maximum = b;
        } else if (parseNumber(first, a)) {
            cmd.maximum = a;
        }
        break;
    }
    case K::Afk:
    case K::Dnd:
    case K::Who:
    case K::GuildMotd:
    case K::Cast:
    case K::Script:
    case K::Reply: cmd.text = rest; break;
    // Phase 13: GetSlashCmdTarget keeps the whole rest (a creature's name has spaces: /target Stormwind Guard).
    case K::Follow:
    case K::Assist:
    case K::TargetByName: cmd.target = rest; break;
    // The channel's commands: the channel, then the rest (ChatFrame.lua's two gsubs).
    case K::ChannelPassword:
    case K::ChannelOwner:
    case K::ChannelModerator:
    case K::ChannelUnmoderator:
    case K::ChannelMute:
    case K::ChannelUnmute:
    case K::ChannelInvite:
    case K::ChannelKick:
    case K::ChannelBan:
    case K::ChannelUnban:
    case K::ChannelAnnounce:
    case K::ChannelModerate: {
        std::string_view more;
        cmd.target = firstWord(rest, more);
        cmd.text = more;
        if (parseNumber(cmd.target, number)) {
            cmd.channel = number;
        }
        break;
    }
    default: { // a name (optional for the target-based ones)
        std::string_view more;
        cmd.target = firstWord(rest, more);
        break;
    }
    }
    return cmd;
}

} // namespace vd::game
