#pragma once

#include "core/Types.h"
#include "game/EventRing.h"
#include "net/GameMessages.h"

#include <string>
#include <string_view>

namespace vd::game {

// What a chat line comes from.
enum class ChatSource : u8 {
    Message,        // SMSG_MESSAGECHAT (see `type`)
    Notification,   // SMSG_NOTIFICATION: a centered error / info text
    AreaTrigger,    // SMSG_AREA_TRIGGER_MESSAGE
    PlayerNotFound, // SMSG_CHAT_PLAYER_NOT_FOUND: text = the name whispered to
    WrongFaction,   // SMSG_CHAT_WRONG_FACTION
    Restricted,     // SMSG_CHAT_RESTRICTED
    TextEmote,      // SMSG_TEXT_EMOTE: textEmote = EmotesText.dbc id, text = the target's name
    // Phase 13: SMSG_SERVER_MESSAGE, a system line already worded with ServerMessages.dbc (the shutdown countdown).
    ServerMessage,
};

struct ChatLine {
    u32 timeMs = 0;
    ChatSource source = ChatSource::Message;
    net::ChatType type = net::ChatType::System;
    u32 language = 0;       // net::Language
    u64 sender = 0;         // player / creature GUID (0 for system lines and nameless monster emotes)
    u64 target = 0;         // monster messages: whom it is addressed to
    u8 tag = 0;             // 1 AFK, 2 DND, 3 GM
    u32 playerRank = 0;     // channel messages
    u32 textEmote = 0;      // TextEmote
    u32 emoteNum = 0;       // TextEmote
    std::string senderName; // monster messages (players: Gameplay::senderName resolves the GUID)
    std::string channel;
    std::string text;
    // Phase 18 (extras, B4): ChatFrame's "[Orcish] " before the text: the line's language (Languages.dbc) when it is
    // not Universal nor the player's own (GetDefaultLanguage); empty otherwise. The text is already as the player
    // reads it (garbled in a language it does not know: game/Languages).
    std::string_view languageName;
};

// The last chat lines, every channel and system text together (an EventRing: keep your own cursor).
class ChatLog {
public:
    static constexpr u32 kCapacity = 128;
    using Ring = EventRing<ChatLine, kCapacity>;

    ChatLine& add(const net::ChatMessage& m, u32 nowMs); // phase 18 (extras): the line added
    void addNotice(ChatSource source, std::string_view text, u32 nowMs);
    void addTextEmote(const net::TextEmote& m, u32 nowMs);
    const Ring& lines() const { return m_lines; }

private:
    ChatLine& push(ChatSource source, u32 nowMs);

    Ring m_lines;
};

// Who the text placeholders refer to.
struct TextSubject {
    std::string_view name;
    u8 race = 0;   // ChrRaces id
    u8 cls = 0;    // ChrClasses id
    u8 gender = 0; // 0 male, 1 female
};
// Expands the placeholders of quest and gossip texts into `out` (cleared first): $N / $n the player's name,
// $R / $r race, $C / $c class (their names: game/CharacterNames), $G<male>:<female>; by gender, $B line break.
// Anything else is copied as is.
void expandText(std::string_view text, const TextSubject& subject, std::string& out);

} // namespace vd::game
