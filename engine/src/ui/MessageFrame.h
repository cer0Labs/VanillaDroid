#pragma once

#include "core/Types.h"

#include <array>
#include <span>
#include <string_view>

namespace vd::ui {

// The client's MessageFrame (FrameXML UI.xsd MessageFrameType), as UIErrorsFrame uses it: a few lines that
// show for `displayMs`, then fade out over `fadeMs`; a new line goes on top (insertMode TOP) or at the
// bottom, and only the newest `maxLines` fit in the frame. Lines are stamped the first time update() sees
// them (whoever adds one does not need the time). Fixed storage: no allocation.
class MessageFrame {
public:
    static constexpr u32 kCapacity = 8;
    static constexpr usize kTextSize = 256; // phase 16 (alerts): a whole chat message (raid warnings: 255 bytes)

    struct Config {
        u32 maxLines = 3;     // UIErrorsFrame: 60 units high, three lines of ErrorFont
        u32 displayMs = 5000; // displayDuration="5"
        u32 fadeMs = 3000;    // fadeDuration (UI.xsd default 3.0)
        bool insertTop = true;
    };
    struct Line {
        std::array<char, kTextSize> text{};
        u32 color = 0;
        bool big = false;
        u64 shownMs = 0; // 0: not stamped yet
    };
    // One line as drawn: from the top of the frame down.
    struct Shown {
        const Line* line = nullptr;
        f32 alpha = 1.0f;
    };

    MessageFrame() = default;
    explicit MessageFrame(Config config) : m_config(config) {}

    // The buffer of a new line (emptied), for the caller to write its NUL-terminated text into.
    std::span<char> add(u32 color, bool big = false);
    void add(std::string_view text, u32 color, bool big = false);
    // Stamps the new lines with `nowMs` and forgets the ones faded out.
    void update(u64 nowMs);
    // The lines shown at `nowMs` in display order (top first), at most maxLines. Returns how many were
    // written.
    u32 visible(u64 nowMs, std::span<Shown> out) const;
    void clear();

private:
    Config m_config;
    std::array<Line, kCapacity> m_lines{};
    u32 m_next = 0;  // where the next line goes
    u32 m_count = 0; // lines held (newest m_count before m_next)
};

} // namespace vd::ui
