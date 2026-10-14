#include "ui/TextInput.h"

#include <algorithm>
#include <cstring>

namespace vd::ui {
namespace {

constexpr u32 kInvalid = 0xFFFFFFFFu;

// One codepoint at s[i], advancing i. Malformed bytes give kInvalid and are skipped one lead byte at a time. Overlong
// forms are rejected except C0 80, modified UTF-8's NUL (a control character, dropped by the callers anyway).
u32 decodeOne(std::string_view s, usize& i) {
    const u8 c = static_cast<u8>(s[i]);
    if (c < 0x80) {
        ++i;
        return c;
    }
    u32 needed = 0;
    u32 cp = 0;
    u32 smallest = 0;
    if ((c & 0xE0) == 0xC0) {
        needed = 1;
        cp = c & 0x1F;
        smallest = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        needed = 2;
        cp = c & 0x0F;
        smallest = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        needed = 3;
        cp = c & 0x07;
        smallest = 0x10000;
    } else {
        ++i; // a stray continuation byte or 0xF8..0xFF
        return kInvalid;
    }
    if (i + needed >= s.size()) {
        ++i; // truncated: its continuation bytes are skipped as strays
        return kInvalid;
    }
    for (u32 k = 1; k <= needed; ++k) {
        const u8 next = static_cast<u8>(s[i + k]);
        if ((next & 0xC0) != 0x80) {
            i += k; // resume at the byte that broke the sequence
            return kInvalid;
        }
        cp = (cp << 6) | (next & 0x3F);
    }
    i += needed + 1;
    if (cp < smallest) {
        return (c == 0xC0 && cp == 0) ? 0u : kInvalid;
    }
    return cp > 0x10FFFF ? kInvalid : cp;
}

// Like decodeOne, joining modified UTF-8's surrogate pairs (two 3-byte sequences) into one codepoint. Lone surrogates
// are invalid.
u32 decode(std::string_view s, usize& i) {
    const u32 cp = decodeOne(s, i);
    if (cp >= 0xD800 && cp < 0xDC00) {
        usize j = i;
        if (j < s.size()) {
            const u32 low = decodeOne(s, j);
            if (low >= 0xDC00 && low < 0xE000) {
                i = j;
                return 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            }
        }
        return kInvalid;
    }
    if (cp >= 0xDC00 && cp < 0xE000) {
        return kInvalid;
    }
    return cp;
}

// A 16-bit value as a 3-byte sequence (surrogate halves in modified UTF-8).
void encode3(u32 unit, char* out) {
    out[0] = static_cast<char>(0xE0 | (unit >> 12));
    out[1] = static_cast<char>(0x80 | ((unit >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (unit & 0x3F));
}

// The printable codepoints of `in` as UTF-8 into `out`, up to `maxBytes` (cut at a codepoint boundary). With
// `lineBreaks` (TextKind::MultiLine) '\n' stays and "\r\n" / '\r' become one.
u32 sanitize(std::string_view in, char* out, u32 maxBytes, bool lineBreaks = false) {
    u32 length = 0;
    for (usize i = 0; i < in.size();) {
        u32 cp = decode(in, i);
        if (lineBreaks && cp == '\r') {
            if (i < in.size() && in[i] == '\n') {
                continue; // the '\n' that follows is the break
            }
            cp = '\n';
        }
        if (cp == kInvalid || (!isPrintable(cp) && !(lineBreaks && cp == '\n'))) {
            continue;
        }
        char bytes[4];
        const u32 count = encodeUtf8(cp, bytes);
        if (length + count > maxBytes) {
            break;
        }
        std::memcpy(out + length, bytes, count);
        length += count;
    }
    return length;
}

} // namespace

bool isPrintable(u32 codepoint) {
    if (codepoint < 0x20 || (codepoint >= 0x7F && codepoint < 0xA0)) {
        return false; // C0 controls, DEL, C1 controls
    }
    if (codepoint >= 0xD800 && codepoint < 0xE000) {
        return false; // surrogates are halves of a codepoint, never one
    }
    if (codepoint == 0x2028 || codepoint == 0x2029) {
        return false; // line / paragraph separators: a field is one line
    }
    return codepoint <= 0x10FFFF;
}

u32 encodeUtf8(u32 codepoint, char out[4]) {
    if (codepoint < 0x80) {
        out[0] = static_cast<char>(codepoint);
        return 1;
    }
    if (codepoint < 0x800) {
        out[0] = static_cast<char>(0xC0 | (codepoint >> 6));
        out[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint < 0x10000) {
        if (codepoint >= 0xD800 && codepoint < 0xE000) {
            return 0;
        }
        encode3(codepoint, out);
        return 3;
    }
    if (codepoint <= 0x10FFFF) {
        out[0] = static_cast<char>(0xF0 | (codepoint >> 18));
        out[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
        return 4;
    }
    return 0;
}

usize toModifiedUtf8(std::string_view utf8, std::span<char> out) {
    if (out.empty()) {
        return 0;
    }
    usize length = 0;
    for (usize i = 0; i < utf8.size();) {
        const u32 cp = decode(utf8, i);
        if (cp == kInvalid) {
            continue;
        }
        char bytes[6];
        u32 count = 0;
        if (cp == 0) {
            bytes[0] = static_cast<char>(0xC0); // never a 0 byte inside the string
            bytes[1] = static_cast<char>(0x80);
            count = 2;
        } else if (cp >= 0x10000) {
            encode3(0xD800 + ((cp - 0x10000) >> 10), bytes);
            encode3(0xDC00 + ((cp - 0x10000) & 0x3FF), bytes + 3);
            count = 6;
        } else {
            count = encodeUtf8(cp, bytes);
        }
        if (length + count + 1 > out.size()) {
            break;
        }
        std::memcpy(out.data() + length, bytes, count);
        length += count;
    }
    out[length] = '\0';
    return length;
}

u32 utf16Length(std::string_view utf8) {
    u32 units = 0;
    for (usize i = 0; i < utf8.size();) {
        const u32 cp = decode(utf8, i);
        if (cp != kInvalid) {
            units += cp >= 0x10000 ? 2 : 1;
        }
    }
    return units;
}

void TextInput::begin(u64 fieldId, std::string_view initial, u32 maxBytes, TextKind kind) {
    if (fieldId == 0) {
        end();
        return;
    }
    clear(); // a secret of the previous field never lingers past its end
    m_field = fieldId;
    m_kind = kind;
    m_maxBytes = std::min(maxBytes, kCapacity);
    m_length = sanitize(initial, m_text.data(), m_maxBytes, kind == TextKind::MultiLine);
    m_submit = Flag::Off;
    m_cancel = Flag::Off;
    ++m_version; // always: the keyboard may still hold another field's text
}

void TextInput::end() {
    m_field = 0;
    m_submit = Flag::Off;
    m_cancel = Flag::Off;
    clear();
}

void TextInput::clear() {
    if (m_length > 0) {
        // Wiped, not just forgotten: the field may have held a password.
        volatile char* bytes = m_text.data();
        for (u32 i = 0; i < m_length; ++i) {
            bytes[i] = 0;
        }
        m_length = 0;
        ++m_version;
    }
}

void TextInput::setText(std::string_view utf8) {
    if (!active() || m_cancel != Flag::Off) {
        return;
    }
    std::array<char, kCapacity> next;
    const u32 length = sanitize(utf8, next.data(), m_maxBytes, m_kind == TextKind::MultiLine);
    if (text() == std::string_view(next.data(), length)) {
        return;
    }
    std::memcpy(m_text.data(), next.data(), length);
    m_length = length;
    ++m_version;
}

void TextInput::typeChar(u32 codepoint) {
    if (!active() || m_cancel != Flag::Off || !isPrintable(codepoint)) {
        return;
    }
    char bytes[4];
    const u32 count = encodeUtf8(codepoint, bytes);
    if (count == 0 || !fits(count)) {
        return;
    }
    std::memcpy(m_text.data() + m_length, bytes, count);
    m_length += count;
    ++m_version;
}

void TextInput::backspace() {
    if (!active() || m_cancel != Flag::Off || m_length == 0) {
        return;
    }
    u32 i = m_length - 1;
    while (i > 0 && (static_cast<u8>(m_text[i]) & 0xC0) == 0x80) {
        --i; // back over continuation bytes to the codepoint's lead byte
    }
    m_length = i;
    ++m_version;
}

void TextInput::submit() {
    if (active() && m_cancel == Flag::Off && m_kind == TextKind::MultiLine) {
        if (fits(1)) { // Enter types a line break in a field of several lines
            m_text[m_length++] = '\n';
            ++m_version;
        }
        return;
    }
    if (active() && m_cancel == Flag::Off) {
        m_submit = Flag::Pending;
    }
}

void TextInput::cancel() {
    if (active()) {
        m_cancel = Flag::Pending;
    }
}

void TextInput::newFrame() {
    m_submit = m_submit == Flag::Pending ? Flag::Shown : Flag::Off;
    if (m_cancel == Flag::Shown) {
        end(); // its widget had the frame to see it
    } else if (m_cancel == Flag::Pending) {
        m_cancel = Flag::Shown;
    }
}

} // namespace vd::ui
