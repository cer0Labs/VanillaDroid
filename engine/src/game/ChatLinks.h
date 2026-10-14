#pragma once

#include "core/Types.h"

#include <array>
#include <span>
#include <string>
#include <string_view>

namespace vd::game {

// The chat's hyperlinks (parity): "|H<link>|h<text>|h" inside a line, as the 1.12 client writes them and its
// ChatFrame_OnHyperlinkShow -> SetItemRef (ItemRef.lua) opens them. <link> is
// "item:<entry>:<enchant>:<suffix>:<unique>" (an item, usually inside its quality's "|cff..|r" colour),
// "enchant:<spell>" (a craft), "player:<name>" (the sender's name ChatFrame links in every player's line) or
// "quest:<id>:<level>"; <text> is what the chat shows
// ("[Linen Cloth]"). Pure text functions: nothing allocates but the std::string they append to.

enum class ChatLinkKind : u8 { Other, Item, Enchant, Player, Quest };

struct ChatLink {
    ChatLinkKind kind = ChatLinkKind::Other;
    std::string_view link;       // "item:2589:0:0:0"
    std::string_view text;       // "[Linen Cloth]"
    usize begin = 0;             // byte offsets in the line: its "|H"...
    usize end = 0;               // ...and just past its closing "|h"
    u32 id = 0;                  // item entry, enchant spell, quest id (0: none)
    std::array<u32, 3> values{}; // item: enchant, random property (suffix), unique id; quest: its level
    std::string_view player;     // player links: the name
};

// The first well-formed link of `line` at or after byte `from` (true, `out` filled), or false. Well formed:
// "|H", a non-empty <link> without bars, "|h", a <text> without bars, "|h".
bool nextChatLink(std::string_view line, usize from, ChatLink& out);
// The link whose bytes cover `at` (what a tap on that part of the drawn line opens), if any.
bool chatLinkAt(std::string_view line, usize at, ChatLink& out);
// Reads a link's <link> part into out.kind, id, values and player; Other for an unknown or malformed one (an
// item or quest without its numbers, a player without a name).
ChatLinkKind parseChatLink(std::string_view link, ChatLink& out);

// Appends text a player typed (or the server wrote) as the chat may draw it: colours (|cAARRGGBB, |r),
// escaped bars
// (||) and well-formed links kept; any other escape is written as text (its bar escaped), so a stray "|H"
// never hides the rest of the line and nothing typed starts a new one.
void appendChatText(std::string& out, std::string_view text);

// "|cff1eff00|Hitem:2132:0:0:0|h[Militia Shortsword]|h|r": an item as the client links it, `colorCode` its
// quality's "|cffRRGGBB" (game::itemQualityColor). Returns what was written into `out` (NUL-terminated, cut
// to fit).
std::string_view formatItemLink(std::span<char> out, u32 entry, std::string_view colorCode,
                                std::string_view name);
// Phase 13 (chat links): an item the player has, as WoW.exe 1.12.1 builds GetContainerItemLink & co.
// ("%s|Hitem:%d:%d:%d:%d|h[%s]|h%s"): its permanent enchantment and random property in the link, and the random
// property's suffix after its name ("[Bandit Cinch of the Monkey]", ITEM_SUFFIX_TEMPLATE).
std::string_view formatItemLink(std::span<char> out, u32 entry, std::string_view colorCode, std::string_view name,
                                u32 enchant, u32 randomProperty, std::string_view suffix);

// Phase 13 (chat links): a line the player typed as the client sends it. Typed bars are escaped ("||", as the
// edit box keeps them) and the links inserted whole ("|cAARRGGBB|H...|h[...]|h|r") kept, so the server's check
// of every "|" (ChatHandler::isValidChatMessage: |c |H |h |h |r in that order, ChatStrictLinkChecking) passes and
// a stray "|c" or "|H" shows as typed. Cut to `maxBytes` (the server drops longer lines) without splitting an
// escape or a link. `out` is replaced.
void escapeChatInput(std::string& out, std::string_view typed, usize maxBytes = 255);

} // namespace vd::game
