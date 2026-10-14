#pragma once

#include "core/Types.h"
#include "game/CharCreate.h"
#include "game/Chat.h"
#include "net/SocialMessages.h"

#include <span>
#include <string>
#include <string_view>

namespace vd::game {

class SocialData;

// The texts of the social side (phase 8, docs/decisions/ADR-0003): which of the original client's strings it shows
// for each answer of the server and each chat line (their keys in GlobalStrings.lua / GlueStrings.lua, read through
// core/Text), and the lines it builds with them. What only this client says has VD_SOCIAL_* keys
// (locale/*/social.lang). Key lookups return nullptr when the client shows nothing; the line builders write into the
// caller's storage and do not allocate (a std::string only grows past the room reserved in it).

// The chat types the player can talk in, by name: CHAT_MSG_SAY "Say", CHAT_MSG_PARTY "Party"... (nullptr: other).
const char* chatTypeKey(net::ChatType type);
// A group's loot method (net::LootMethod): LOOT_GROUP_LOOT "Loot: Group Loot"...
const char* lootMethodKey(u8 method);
// What the client prints when the leader changes it: ERR_SET_LOOT_GROUP "Looting changed to group loot."...
const char* lootMethodChangedKey(u8 method);
// An item quality's name (ITEM_QUALITY2_DESC "Uncommon") and its color code ("|cff1eff00", ITEM_QUALITY_COLORS).
const char* itemQualityKey(u8 quality);
std::string_view itemQualityColor(u8 quality);
// SMSG_PARTY_COMMAND_RESULT (nullptr for Ok), SMSG_FRIEND_STATUS and SMSG_GUILD_COMMAND_RESULT (nullptr for Ok;
// `command` is its net::kGuildCommand*: the guild master quitting gets its own text).
const char* partyResultKey(net::PartyResult result);
const char* friendResultKey(net::FriendResult result);
const char* guildErrorKey(net::GuildError error, u32 command);
// SMSG_GUILD_EVENT (nullptr for the silent ones). The strings fill the text's %s in the event's order.
const char* guildEventKey(net::GuildEventKind kind);
// The text of a character's creation, deletion or name check result (net/ResponseCodes.gen.h): GlueStrings has
// one under each code's own name (CHAR_CREATE_NAME_IN_USE...). Empty for CHAR_NAME_SUCCESS.
std::string_view charResultText(u8 code);
// The creation screen's labels of its customization options, as CharacterCreate.lua builds them from ChrRaces'
// tokens: CHAR_CUSTOMIZATION1_DESC "Skin Color", HAIR_HORNS_STYLE "Horn Style", FACIAL_HAIR_TUSKS "Tusks"...
const char* customizationKey(AppearanceOption option, FeatureKind hair, FeatureKind facialHair);

// A chat message (ChatSource::Message) or the server's chat errors as the client's ChatFrame prints them: "[Aeryn]
// says: hi", "To [Aeryn]: on my way", "[2. Trade] [Brakk]: WTS...", "<AFK>[Aeryn] whispers: ...", "Hogger yells:
// ...", "No player named 'Aeryn' is currently playing.". `sender` is the sender's name; `channelNumber` the line's
// channel's number (0: none). A player's name is a link ("|Hplayer:Aeryn|h[Aeryn]|h", game/ChatLinks); the text
// keeps its colours and well-formed links (appendChatText). Text emotes: appendTextEmote.
void appendChatLine(std::string& out, const ChatLine& line, std::string_view sender, u32 channelNumber);
// An EmotesText line (SocialData::textEmoteLine: "%s waves at %s.") with the emoter and the target in place of its
// %s; with no line, "Aeryn makes some strange gestures." (CHAT_EMOTE_UNKNOWN).
void appendTextEmote(std::string& out, std::string_view line, std::string_view first, std::string_view second);
// A channel notice (SMSG_CHANNEL_NOTIFY) as the client prints its CHAT_*_NOTICE string: `channel` as the chat names
// it ("2. Trade - City"; joins and leaves show it without its zone), `player` whom the notice is about (its GUID's name, or the name it carries) and `by` who
// did it (kicks and bans). Empty when the client prints nothing.
std::string_view formatChannelNotice(std::span<char> out, net::ChannelNotice notice, std::string_view channel,
                                     std::string_view player, std::string_view by);

// Phase 21 (languages): the zone channels go out and come back with the base client's names ("Trade - City": the
// server matches them against its enUS ChatChannels.dbc) and show in the player's language through `data`
// (SocialData::displayChannel). Set once at start-up on the main thread; without it they show as the server names
// them.
void setChannelNames(const SocialData* data);
// A channel's name as the player reads it ("2. Trade - City" -> "2. Comercio - Ciudad"), into `out` or as it is.
std::string_view channelDisplayName(std::string_view channel, std::span<char> out);
// Without its zone, as ChatFrame shows it before a line ("Trade - City" -> "Trade"; gsub(arg4, "%s%-%s.*", "")), into
// `out` (or a part of `channel`).
std::string_view channelDisplayBase(std::string_view channel, std::span<char> out);

} // namespace vd::game
