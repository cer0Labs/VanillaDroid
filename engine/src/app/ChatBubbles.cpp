#include "app/ChatBubbles.h"

#include "core/Profile.h"
#include "game/Gameplay.h"
#include "ui/DrawList.h"

#include <algorithm>

namespace vd {
namespace {

using net::ChatType;

constexpr u32 kBaseMs = 4000;  // a short line
constexpr u32 kPerByteMs = 60; // plus reading time
constexpr u32 kMaxLifeMs = 15000;

// The line's text without hyperlinks (|Hitem:...|h[Name]|h keeps [Name]) and color codes (|cAARRGGBB, |r):
// the bubble has one color and fades as a whole. Returns the bytes written.
u32 stripCodes(std::string_view text, std::span<char> out) {
    u32 used = 0;
    for (usize i = 0; i < text.size() && used < out.size(); ++i) {
        if (text[i] == '|' && i + 1 < text.size()) {
            const char code = text[i + 1];
            if (code == 'H') {
                const usize end = text.find("|h", i + 2);
                if (end == std::string_view::npos) {
                    break;
                }
                i = end + 1;
                continue;
            }
            if (code == 'h' || code == 'r') {
                ++i;
                continue;
            }
            if (code == 'c' && i + 9 < text.size()) {
                i += 9;
                continue;
            }
        }
        out[used++] = text[i];
    }
    return used;
}

} // namespace

u32 ChatBubbles::lifetimeMs(usize length) {
    return static_cast<u32>(std::min<usize>(kMaxLifeMs, kBaseMs + kPerByteMs * length));
}

void ChatBubbles::update(const game::Gameplay& play, u64 player, u32 nowMs, const Options& options) {
    VD_PROFILE_ZONE("ChatBubbles::update");
    if (player != m_cursorPlayer) {
        m_cursorPlayer = player;
        m_cursor = play.chat().end();
        clear();
    }
    while (const game::ChatLine* line = play.chat().next(m_cursor)) {
        // Ignored players are not heard (the client filters them).
        if (line->sender != 0 && play.social().isIgnored(line->sender)) {
            continue;
        }
        add(*line, nowMs, options);
    }
    if (!options.say && !options.party) {
        clear();
    }
    expire(nowMs);
}

bool ChatBubbles::add(const game::ChatLine& line, u32 nowMs, const Options& options) {
    if (line.source != game::ChatSource::Message || line.sender == 0) {
        return false;
    }
    // Text colors of the client's default chat settings (ChatTypeInfo), like the chat frame's.
    u32 color = 0;
    switch (line.type) {
    case ChatType::Say:
    case ChatType::MonsterSay: color = options.say ? ui::rgba(255, 255, 255) : 0; break;
    case ChatType::Yell:
    case ChatType::MonsterYell: color = options.say ? ui::rgba(255, 64, 64) : 0; break;
    case ChatType::Party: color = options.party ? ui::rgba(170, 170, 255) : 0; break;
    default: break;
    }
    if (color == 0) {
        return false;
    }
    // Its speaker's bubble, a free one, or the oldest.
    Bubble* slot = nullptr;
    for (Bubble& b : m_bubbles) {
        if (b.speaker == line.sender) {
            slot = &b;
            break;
        }
    }
    for (Bubble& b : m_bubbles) {
        if (!slot && b.speaker == 0) {
            slot = &b;
        }
    }
    if (!slot) {
        slot = &m_bubbles[0];
        for (Bubble& b : m_bubbles) {
            if (nowMs - b.startMs > nowMs - slot->startMs) {
                slot = &b;
            }
        }
    }
    const u32 length = stripCodes(line.text, slot->text);
    if (length == 0) {
        if (slot->speaker == line.sender) {
            slot->speaker = 0; // an empty line leaves nothing over the head
        }
        return false;
    }
    slot->speaker = line.sender;
    slot->startMs = nowMs;
    slot->lifeMs = lifetimeMs(length);
    slot->color = color;
    slot->length = length;
    return true;
}

void ChatBubbles::expire(u32 nowMs) {
    for (Bubble& b : m_bubbles) {
        if (b.speaker != 0 && nowMs - b.startMs >= b.lifeMs) {
            b.speaker = 0;
        }
    }
}

u32 ChatBubbles::count() const {
    return static_cast<u32>(
        std::count_if(m_bubbles.begin(), m_bubbles.end(), [](const Bubble& b) { return b.speaker != 0; }));
}

f32 ChatBubbles::alpha(const Bubble& bubble, u32 nowMs) {
    const u32 age = nowMs - bubble.startMs;
    if (age >= bubble.lifeMs) {
        return 0.0f;
    }
    const u32 left = bubble.lifeMs - age;
    return left >= kFadeMs ? 1.0f : static_cast<f32>(left) / static_cast<f32>(kFadeMs);
}

const ChatBubbles::Bubble* ChatBubbles::of(u64 speaker) const {
    for (const Bubble& b : m_bubbles) {
        if (speaker != 0 && b.speaker == speaker) {
            return &b;
        }
    }
    return nullptr;
}

} // namespace vd
