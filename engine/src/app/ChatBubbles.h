#pragma once

#include "core/Types.h"
#include "game/Chat.h"

#include <array>
#include <span>
#include <string_view>

namespace vd {

namespace game {
class Gameplay;
}

// Chat bubbles (parity; the client's engine draws them: ChatBubbleFrame in WoW.exe, CVars ChatBubbles and
// ChatBubblesParty, both on by default in UIOptionsFrame): what a unit says or yells (players and NPCs: /say,
// /yell, monster say and yell) and, with the party option, its party lines, over the speaker's head for a
// while — longer for longer lines — fading out at the end. One bubble per speaker (a new line replaces it)
// and at most kMax at once (a new speaker takes the oldest one's place). Reads Gameplay::chat() with its own
// cursor; drawn by the HUD over the speakers it shows (app/HudWorld.cpp, views in app/WorldLabels). Game
// thread; nothing allocates.
class ChatBubbles {
public:
    static constexpr u32 kMax = 6;
    static constexpr u32 kTextBytes = 256; // a chat line is at most 255 bytes
    static constexpr u32 kFadeMs = 600;    // the last part of its life

    struct Options {
        bool say = true;   // ChatBubbles: say, yell, monster say / yell
        bool party = true; // ChatBubblesParty
    };
    struct Bubble {
        u64 speaker = 0; // 0: a free slot
        u32 startMs = 0;
        u32 lifeMs = 0;
        u32 color = 0; // the text's: its chat type's (ChatTypeInfo)
        u32 length = 0;
        std::array<char, kTextBytes> text{};
        std::string_view view() const { return {text.data(), length}; }
    };

    // How long a line of `length` bytes stays: a few seconds, plus the time to read it.
    static u32 lifetimeMs(usize length);
    // The lines that arrived since the last call become bubbles (the options say which), expired bubbles go.
    // The first call (and the first after another player enters the world) starts from the newest line.
    void update(const game::Gameplay& play, u64 player, u32 nowMs, const Options& options);
    // One chat line: a bubble when it is a kind the options show and a unit said it. True if it made one.
    bool add(const game::ChatLine& line, u32 nowMs, const Options& options);
    // Bubbles past their life go.
    void expire(u32 nowMs);
    void clear() { m_bubbles = {}; }

    // Every slot: the free ones have speaker 0.
    std::span<const Bubble> bubbles() const { return m_bubbles; }
    u32 count() const;
    // Opacity now: 1, then down to 0 over the last kFadeMs.
    static f32 alpha(const Bubble& bubble, u32 nowMs);
    // The bubble a unit shows, nullptr if none.
    const Bubble* of(u64 speaker) const;

private:
    std::array<Bubble, kMax> m_bubbles{};
    u32 m_cursor = 0;
    u64 m_cursorPlayer = 0; // the player the cursor was set for
};

} // namespace vd
