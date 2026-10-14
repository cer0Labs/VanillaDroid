#pragma once

#include "core/Types.h"
#include "game/EventRing.h"

#include <array>
#include <string_view>

namespace vd::game {

// Phase 15 (guild, help): a line WoW.exe shows in the UIErrorsFrame for a guild charter, the tabard designer
// or a GM ticket (its error table: ERR_PETITION_SIGNED_S, ERR_GUILDEMBLEM_SUCCESS,
// ERR_TICKET_CREATE_ERROR...): the client string's key and, for its %s, a player by GUID (the screens ask the
// name cache) or a text. Read the rings with your own cursor (game/EventRing).
struct GuildNotice {
    const char* key = nullptr;
    u64 guid = 0;                // a player whose name fills %s (0: none)
    std::array<char, 48> text{}; // ... or this text (empty: none)
    u32 timeMs = 0;

    void setText(std::string_view s) {
        const usize n = s.size() < text.size() - 1 ? s.size() : text.size() - 1;
        for (usize i = 0; i < n; ++i) {
            text[i] = s[i];
        }
        text[n] = '\0';
    }
    std::string_view textView() const { return text.data(); }
};
using GuildNotices = EventRing<GuildNotice, 16>;

} // namespace vd::game
