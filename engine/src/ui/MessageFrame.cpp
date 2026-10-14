#include "ui/MessageFrame.h"

#include <algorithm>
#include <cstring>

namespace vd::ui {

std::span<char> MessageFrame::add(u32 color, bool big) {
    Line& line = m_lines[m_next];
    m_next = (m_next + 1) % kCapacity;
    m_count = std::min(m_count + 1, kCapacity);
    line.text[0] = 0;
    line.color = color;
    line.big = big;
    line.shownMs = 0;
    return line.text;
}

void MessageFrame::add(std::string_view text, u32 color, bool big) {
    const std::span<char> out = add(color, big);
    const usize n = std::min(text.size(), out.size() - 1);
    std::memcpy(out.data(), text.data(), n);
    out[n] = 0;
}

void MessageFrame::update(u64 nowMs) {
    const u64 life = static_cast<u64>(m_config.displayMs) + m_config.fadeMs;
    // From the oldest: the first one still alive keeps every newer one.
    while (m_count > 0) {
        Line& oldest = m_lines[(m_next + kCapacity - m_count) % kCapacity];
        if (oldest.shownMs == 0 || nowMs - oldest.shownMs < life) {
            break;
        }
        oldest.text[0] = 0;
        --m_count;
    }
    for (u32 i = 0; i < m_count; ++i) {
        Line& line = m_lines[(m_next + kCapacity - 1 - i) % kCapacity];
        if (line.shownMs == 0) {
            line.shownMs = std::max<u64>(nowMs, 1);
        }
    }
}

u32 MessageFrame::visible(u64 nowMs, std::span<Shown> out) const {
    // The newest maxLines (newest first), then top first: newest on top with insertMode TOP, oldest on top
    // otherwise.
    const u32 room = std::min(m_config.maxLines, static_cast<u32>(out.size()));
    u32 written = 0;
    for (u32 age = 0; age < m_count && written < room; ++age) {
        const Line& line = m_lines[(m_next + kCapacity - 1 - age) % kCapacity];
        if (line.text[0] == 0) {
            continue;
        }
        f32 alpha = 1.0f;
        if (line.shownMs != 0 && nowMs > line.shownMs + m_config.displayMs) {
            const u64 fading = nowMs - line.shownMs - m_config.displayMs;
            alpha = fading >= m_config.fadeMs
                        ? 0.0f
                        : 1.0f - static_cast<f32>(fading) / static_cast<f32>(m_config.fadeMs);
        }
        if (alpha > 0.0f) {
            out[written++] = {&line, alpha};
        }
    }
    if (!m_config.insertTop) {
        std::reverse(out.begin(), out.begin() + written);
    }
    return written;
}

void MessageFrame::clear() {
    for (Line& line : m_lines) {
        line.text[0] = 0;
    }
    m_count = 0;
}

} // namespace vd::ui
