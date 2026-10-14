#include "ui/TextInput.h"

#include <doctest/doctest.h>

#include <array>
#include <ostream> // doctest prints std::string_view operands with operator<<
#include <string>
#include <string_view>

using vd::u32;
using vd::u8;
using vd::ui::TextInput;

namespace {

constexpr vd::u64 kChat = 0x1234567;
constexpr vd::u64 kPassword = 0x7654321;

// U+00F1 (2 bytes), U+20AC (3 bytes), U+1F600 (4 bytes).
constexpr std::string_view kEnye = "\xC3\xB1";
constexpr std::string_view kEuro = "\xE2\x82\xAC";
constexpr std::string_view kGrin = "\xF0\x9F\x98\x80";
// U+1F600 in Java's modified UTF-8: the surrogate pair D83D DE00, each as a 3-byte sequence.
constexpr std::string_view kGrinModified = "\xED\xA0\xBD\xED\xB8\x80";

std::string str(std::string_view s) {
    return std::string(s);
}

} // namespace

TEST_CASE("TextInput: begin, end and the field being edited") {
    TextInput input;
    CHECK_FALSE(input.active());
    CHECK(input.field() == 0);
    CHECK(input.text().empty());
    const u32 v0 = input.version();

    input.begin(kChat, "hola");
    CHECK(input.active());
    CHECK(input.field() == kChat);
    CHECK(input.text() == "hola");
    CHECK(input.maxBytes() == 255);
    CHECK(input.version() != v0);

    input.end();
    CHECK_FALSE(input.active());
    CHECK(input.field() == 0);
    CHECK(input.text().empty());

    // Starting again always changes the version, even with the same (empty) text: the IME must be re-seeded.
    input.begin(kChat, "");
    const u32 v1 = input.version();
    input.end();
    input.begin(kChat, "");
    CHECK(input.version() != v1);

    // Field 0 is "the world": beginning it is ending.
    input.begin(0, "x");
    CHECK_FALSE(input.active());
}

TEST_CASE("TextInput: typing printable characters of every UTF-8 length") {
    TextInput input;
    input.begin(kChat, "");
    const u32 v0 = input.version();
    input.typeChar('a');
    input.typeChar(0xF1);    // n with tilde
    input.typeChar(0x20AC);  // euro sign
    input.typeChar(0x1F600); // grinning face
    CHECK(input.text() == str("a") + str(kEnye) + str(kEuro) + str(kGrin));
    CHECK(input.version() == v0 + 4);

    // Control characters, lone surrogates and out-of-range codepoints are ignored, without a version change.
    const u32 v1 = input.version();
    for (const u32 cp : {0x00u, 0x08u, 0x09u, 0x0Au, 0x0Du, 0x1Bu, 0x7Fu, 0x85u, 0x9Fu, 0xD800u, 0xDFFFu, 0x2028u,
                         0x110000u}) {
        input.typeChar(cp);
    }
    CHECK(input.version() == v1);
    CHECK(input.text().size() == 1 + 2 + 3 + 4);
}

TEST_CASE("TextInput: the byte limit is enforced at codepoint boundaries") {
    TextInput input;
    input.begin(kChat, "", 5);
    for (const char c : std::string_view("abcd")) {
        input.typeChar(static_cast<u8>(c));
    }
    input.typeChar(0xF1); // 2 bytes: 6 > 5, refused whole
    CHECK(input.text() == "abcd");
    input.typeChar('e');
    CHECK(input.text() == "abcde");
    input.typeChar('f');
    CHECK(input.text() == "abcde");

    // The IME's text is cut before the first codepoint that does not fit.
    input.begin(kChat, "", 4);
    input.setText(str("ab") + str(kEnye) + "c");
    CHECK(input.text() == str("ab") + str(kEnye));
    input.begin(kChat, "", 3);
    input.setText(str("a") + str(kEuro) + "b");
    CHECK(input.text() == "a");

    // The initial text too.
    input.begin(kPassword, "secret", 3);
    CHECK(input.text() == "sec");

    // maxBytes never exceeds the fixed capacity.
    input.begin(kChat, "", 5000);
    CHECK(input.maxBytes() == TextInput::kCapacity);
    for (u32 i = 0; i < TextInput::kCapacity + 10; ++i) {
        input.typeChar('x');
    }
    CHECK(input.text().size() == TextInput::kCapacity);
}

TEST_CASE("TextInput: backspace removes whole codepoints") {
    TextInput input;
    input.begin(kChat, str("a") + str(kEuro) + str(kGrin));
    input.backspace();
    CHECK(input.text() == str("a") + str(kEuro));
    input.backspace();
    CHECK(input.text() == "a");
    input.backspace();
    CHECK(input.text().empty());
    const u32 v = input.version();
    input.backspace(); // nothing left: no change
    CHECK(input.version() == v);
}

TEST_CASE("TextInput: the IME's whole text, sanitized") {
    TextInput input;
    input.begin(kChat, "");
    input.setText("hello");
    CHECK(input.text() == "hello");
    const u32 v = input.version();
    input.setText("hello"); // the same text (the IME echoing it back): no change
    CHECK(input.version() == v);

    input.setText("a\nb\tc\r");
    CHECK(input.text() == "abc");
    input.setText("a\xFF" "b\x80" "c");      // stray bytes
    CHECK(input.text() == "abc");
    input.setText("x\xE2\x82");               // truncated sequence at the end
    CHECK(input.text() == "x");
    input.setText("\xC1\x81y");               // overlong 'A'
    CHECK(input.text() == "y");
    input.setText("n\xC0\x80m");              // modified UTF-8's NUL
    CHECK(input.text() == "nm");

    // Java's modified UTF-8: surrogate pairs become the 4-byte form; lone halves are dropped.
    input.setText(str("go ") + str(kGrinModified));
    CHECK(input.text() == str("go ") + str(kGrin));
    input.setText("\xED\xA0\xBDz");           // high half without its low half
    CHECK(input.text() == "z");
    input.setText("\xED\xB8\x80z");           // low half alone
    CHECK(input.text() == "z");
    input.setText(str(kGrin)); // plain UTF-8 works as well
    CHECK(input.text() == kGrin);
}

TEST_CASE("TextInput: nothing happens while not editing") {
    TextInput input;
    const u32 v = input.version();
    input.typeChar('a');
    input.setText("abc");
    input.backspace();
    input.submit();
    input.cancel();
    input.newFrame();
    CHECK_FALSE(input.active());
    CHECK(input.text().empty());
    CHECK_FALSE(input.submitted());
    CHECK_FALSE(input.cancelled());
    CHECK(input.version() == v);
}

TEST_CASE("TextInput: submit is reported for the next frame only") {
    TextInput input;
    input.begin(kChat, "/say hi");
    input.newFrame();
    CHECK_FALSE(input.submitted());

    input.submit(); // Enter arrives between frames...
    input.newFrame(); // ...the game starts its frame: the widgets see it
    CHECK(input.submitted());
    CHECK(input.field() == kChat);
    CHECK(input.text() == "/say hi");
    input.newFrame();
    CHECK_FALSE(input.submitted());
    CHECK(input.active()); // submitting does not end editing: the widget decides

    // A new field (Enter on the account name moves to the password) does not inherit it.
    input.submit();
    input.newFrame();
    input.begin(kPassword, "");
    CHECK_FALSE(input.submitted());
}

TEST_CASE("TextInput: cancel is reported for one frame, then editing ends") {
    TextInput input;
    input.begin(kChat, "draft");
    input.cancel();
    input.typeChar('x'); // typing after Escape is ignored
    input.newFrame();
    CHECK(input.cancelled());
    CHECK(input.active()); // still the field's, so its widget can tell
    CHECK(input.field() == kChat);
    CHECK(input.text() == "draft");
    input.submit(); // a cancelled field cannot be submitted
    input.newFrame();
    CHECK_FALSE(input.cancelled());
    CHECK_FALSE(input.submitted());
    CHECK_FALSE(input.active());
    CHECK(input.text().empty());

    // The widget may end it itself when it sees the cancel.
    input.begin(kChat, "x");
    input.cancel();
    input.newFrame();
    input.end();
    CHECK_FALSE(input.cancelled());
    input.newFrame();
    CHECK_FALSE(input.active());
}

TEST_CASE("TextInput: modified UTF-8 and UTF-16 lengths for GameTextInput") {
    using vd::ui::toModifiedUtf8;
    using vd::ui::utf16Length;

    std::array<char, 32> out{};
    const std::string text = str("a") + str(kEuro) + str(kGrin);
    const vd::usize written = toModifiedUtf8(text, out);
    CHECK(std::string_view(out.data(), written) == str("a") + str(kEuro) + str(kGrinModified));
    CHECK(out[written] == '\0');
    CHECK(utf16Length(text) == 1 + 1 + 2);
    CHECK(utf16Length("") == 0);

    // Round trip through the IME's encoding.
    TextInput input;
    input.begin(kChat, "");
    input.setText(std::string_view(out.data(), written));
    CHECK(input.text() == text);

    // Codepoints that do not fit are left out whole; the terminator always fits.
    std::array<char, 8> small{};
    CHECK(toModifiedUtf8(text, small) == 4); // "a" + the euro sign; the pair needs 6 more bytes
    CHECK(small[4] == '\0');
    std::array<char, 1> tiny{};
    CHECK(toModifiedUtf8("abc", tiny) == 0);
    CHECK(tiny[0] == '\0');
    CHECK(toModifiedUtf8("abc", std::span<char>()) == 0);
}

TEST_CASE("TextInput: printable codepoints and UTF-8 encoding") {
    using vd::ui::encodeUtf8;
    using vd::ui::isPrintable;
    CHECK(isPrintable(' '));
    CHECK(isPrintable('~'));
    CHECK(isPrintable(0xA0));
    CHECK(isPrintable(0x10FFFF));
    CHECK_FALSE(isPrintable(0x1F));
    CHECK_FALSE(isPrintable(0x7F));
    CHECK_FALSE(isPrintable(0x9F));
    CHECK_FALSE(isPrintable(0xDC00));
    CHECK_FALSE(isPrintable(0x110000));

    char bytes[4];
    CHECK(encodeUtf8('A', bytes) == 1);
    CHECK(encodeUtf8(0xF1, bytes) == 2);
    CHECK(std::string_view(bytes, 2) == kEnye);
    CHECK(encodeUtf8(0x20AC, bytes) == 3);
    CHECK(std::string_view(bytes, 3) == kEuro);
    CHECK(encodeUtf8(0x1F600, bytes) == 4);
    CHECK(std::string_view(bytes, 4) == kGrin);
    CHECK(encodeUtf8(0xD800, bytes) == 0);
    CHECK(encodeUtf8(0x110000, bytes) == 0);
}

// Phase 15 (options): a macro's body keeps its lines; Enter types a break instead of submitting.
TEST_CASE("TextInput: MultiLine keeps line breaks and Enter types one") {
    TextInput input;
    input.begin(kChat, "/cast Heal\r\n/say hi\rbye", 255, vd::ui::TextKind::MultiLine);
    CHECK(input.text() == "/cast Heal\n/say hi\nbye");
    input.submit();
    CHECK_FALSE(input.submitted());
    CHECK(input.text() == "/cast Heal\n/say hi\nbye\n");
    input.setText("a\r\nb");
    CHECK(input.text() == "a\nb");
    // Any other kind drops them (a control character) and Enter submits.
    input.begin(kChat, "a\nb", 255, vd::ui::TextKind::Message);
    CHECK(input.text() == "ab");
    input.submit();
    CHECK(input.submitted());
}
