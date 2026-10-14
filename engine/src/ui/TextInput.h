#pragma once

#include "core/Types.h"

#include <array>
#include <span>
#include <string_view>

namespace vd::ui {

// What a field holds: the platform picks its keyboard by it.
enum class TextKind : u8 {
    Plain,   // names, addresses: no suggestions, no autocorrect
    Secret,  // passwords: masked, never learnt by the keyboard
    Message, // chat: suggestions welcome, the action key sends
    // Phase 15 (options): several lines (a macro's body, Blizzard_MacroUI's multiLine edit box): line breaks stay in
    // the text ("\r\n" and '\r' become '\n') and Enter types one instead of submitting.
    MultiLine,
    Digits, // phase 22 (pairing): a number (the pairing code): the phone's number pad
};

// The one text field being edited (the chat line, the login fields): what the platform's keyboard types into.
// Widgets start and stop editing; the platform feeds it (desktop: WM_CHAR and Backspace/Enter/Escape; Android: the
// soft keyboard's whole text through GameTextInput, its action key, Back) and shows the soft keyboard while it is
// active(). The text is always valid UTF-8 without control characters, at most maxBytes long. Fixed capacity: nothing
// allocates.
//
// submit() and cancel() are events that usually arrive between frames. submitted() / cancelled() report them from the
// newFrame() that follows the event until the next one: exactly the frame the widgets see. A cancelled field stays
// active (field() and text() still valid) during that frame so its widget can tell; editing ends at the next
// newFrame(). begin() and end() clear both.
class TextInput {
public:
    static constexpr u32 kCapacity = 1024; // bytes: the most maxBytes can be

    // A widget starts editing (id = the field's ui::WidgetId), with its current text and a byte limit. Starting again
    // (the same or another field) always counts as a change of text: the platform re-seeds its keyboard.
    void begin(u64 fieldId, std::string_view initial, u32 maxBytes = 255, TextKind kind = TextKind::Plain);
    void end(); // stops editing (keyboard hidden); the text is wiped from memory
    bool active() const { return m_field != 0; }
    u64 field() const { return m_field; } // 0 when not editing
    std::string_view text() const { return {m_text.data(), m_length}; }
    u32 maxBytes() const { return m_maxBytes; }
    TextKind kind() const { return m_kind; }

    // Platform events (ignored while not editing):
    // The whole field as the IME has it (Android GameTextInput). Accepts Java's modified UTF-8 too (a codepoint above
    // U+FFFF as two 3-byte surrogates); invalid bytes and control characters are dropped, the rest cut at maxBytes.
    void setText(std::string_view utf8);
    void typeChar(u32 codepoint); // desktop WM_CHAR: printable characters only, if they fit
    void backspace();             // removes the last codepoint
    void submit();                // Enter / IME action: submitted() for one frame (MultiLine: a line break)
    void cancel();                // Escape / Back: cancelled() for one frame, then editing ends
    bool submitted() const { return m_submit != Flag::Off; }
    bool cancelled() const { return m_cancel != Flag::Off; }

    void newFrame(); // the game calls it once per frame, before the widgets
    // Bumps on every change of the text (and on every begin()). The platform re-seeds its keyboard only when the
    // version moved past the one it last saw, so changes that came from the keyboard are not sent back to it.
    u32 version() const { return m_version; }

private:
    enum class Flag : u8 {
        Off,
        Pending, // happened since the last newFrame()
        Shown,   // reported during this frame
    };

    void clear();
    bool fits(u32 bytes) const { return m_length + bytes <= m_maxBytes; }

    std::array<char, kCapacity> m_text{};
    u32 m_length = 0;
    u32 m_maxBytes = 0;
    u64 m_field = 0;
    TextKind m_kind = TextKind::Plain;
    u32 m_version = 0;
    Flag m_submit = Flag::Off;
    Flag m_cancel = Flag::Off;
};

// A codepoint that belongs in a text field: not a control character (C0, DEL, C1), a surrogate or beyond U+10FFFF.
bool isPrintable(u32 codepoint);
// A codepoint as UTF-8 into `out` (up to 4 bytes); returns the bytes written (0 for surrogates / beyond U+10FFFF).
u32 encodeUtf8(u32 codepoint, char out[4]);
// UTF-8 -> Java's modified UTF-8 (what GameTextInput and JNI expect): a codepoint above U+FFFF becomes a surrogate pair
// of 3-byte sequences. Always 0-terminated; stops before a codepoint that would not fit. Returns the bytes written
// (without the terminator).
usize toModifiedUtf8(std::string_view utf8, std::span<char> out);
// Length in UTF-16 code units: Java string indices, as the IME's selection counts.
u32 utf16Length(std::string_view utf8);

} // namespace vd::ui
