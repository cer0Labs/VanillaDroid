// The client's Lua string files: Windows-1252 bytes become UTF-8 (formats::utf8FromWindows1252), well-formed UTF-8
// passes unchanged, and the quotes lost as U+FFFD come back (formats::restoreLostQuotes).
#include "bake/UiStrings.h"
#include "formats/LuaStrings.h"

#include "SharedMpq.h"

#include <doctest/doctest.h>

#include <string>

using namespace vd;

namespace {

std::string decode(std::string_view bytes) {
    return formats::utf8FromWindows1252(bytes);
}

std::string restore(std::string_view utf8) {
    return formats::restoreLostQuotes(utf8);
}

constexpr const char* kLost = "\xEF\xBF\xBD"; // U+FFFD

} // namespace

TEST_CASE("Client texts: Windows-1252 bytes become UTF-8") {
    CHECK(decode("") == "");
    CHECK(decode("Plain ASCII, 100% (\t\n)") == "Plain ASCII, 100% (\t\n)");
    CHECK(decode(std::string_view("a\0b", 3)) == std::string("a\0b", 3));
    // The punctuation of 0x80..0x9F.
    CHECK(decode("\x91one\x92 \x93two\x94") == "\xE2\x80\x98one\xE2\x80\x99 \xE2\x80\x9Ctwo\xE2\x80\x9D");
    CHECK(decode("it\x92s") == "it\xE2\x80\x99s");
    CHECK(decode("a\x96" "b\x97" "c\x85") == "a\xE2\x80\x93" "b\xE2\x80\x94" "c\xE2\x80\xA6");
    CHECK(decode("\x80 \x99 \x95 \x8A\x9A \x9F") ==
          "\xE2\x82\xAC \xE2\x84\xA2 \xE2\x80\xA2 \xC5\xA0\xC5\xA1 \xC5\xB8");
    // The five bytes Windows-1252 leaves undefined: their C1 control code points.
    CHECK(decode("\x81\x8D\x8F\x90\x9D") == "\xC2\x81\xC2\x8D\xC2\x8F\xC2\x90\xC2\x9D");
    // 0xA0..0xFF alone: Latin-1.
    CHECK(decode("caf\xE9") == "caf\xC3\xA9");
    CHECK(decode("\xA0\xBF\xC0\xFF") == "\xC2\xA0\xC2\xBF\xC3\x80\xC3\xBF");
}

TEST_CASE("Client texts: well-formed UTF-8 passes unchanged") {
    const char* utf8[] = {
        "Configuraci\xC3\xB3n del juego",              // two bytes
        "\xC2\xBF" "Qu\xC3\xA9 tal?",                  // a question opened with an inverted mark
        "\xE2\x80\x9Cquoted\xE2\x80\x9D \xE2\x82\xAC", // three bytes
        "\xF0\x9F\x98\x80",                            // four bytes
        "\xED\x9F\xBF \xEF\xBF\xBD \xF4\x8F\xBF\xBF",  // the last before the surrogates, U+FFFD, U+10FFFF
    };
    for (const char* text : utf8) {
        CHECK(decode(text) == text);
    }
    // Both in one text.
    CHECK(decode("Espa\xC3\xB1" "a \x93" "x\x94") == "Espa\xC3\xB1" "a \xE2\x80\x9C" "x\xE2\x80\x9D");
    // Decoding is idempotent: its output is UTF-8.
    const std::string once = decode("\x93" "caf\xE9\x94 \xC3\xA9");
    CHECK(decode(once) == once);
}

TEST_CASE("Client texts: malformed UTF-8 is read byte by byte") {
    // Overlong forms, surrogates and code points over U+10FFFF are not UTF-8.
    CHECK(decode("\xC0\xAF") == "\xC3\x80\xC2\xAF");
    CHECK(decode("\xE0\x80\xAF") == "\xC3\xA0\xE2\x82\xAC\xC2\xAF");
    CHECK(decode("\xED\xA0\x80") == "\xC3\xAD\xC2\xA0\xE2\x82\xAC");
    CHECK(decode("\xF4\x90\x80\x80") == "\xC3\xB4\xC2\x90\xE2\x82\xAC\xE2\x82\xAC");
    CHECK(decode("\xF5\x80") == "\xC3\xB5\xE2\x82\xAC");
    // A lone continuation byte, a sequence cut short (at the end, and before ASCII).
    CHECK(decode("\xA9") == "\xC2\xA9");
    CHECK(decode("abc\xE2\x82") == "abc\xC3\xA2\xE2\x80\x9A");
    CHECK(decode("\xC3 x") == "\xC3\x83 x");
    // A Windows-1252 character before well-formed UTF-8.
    CHECK(decode("\xE9\xC3\xA9") == "\xC3\xA9\xC3\xA9");
}

TEST_CASE("Client texts: a parsed Lua string table, decoded") {
    Result<std::vector<formats::LuaString>> parsed =
        formats::parseLuaStrings("SAMPLE_QUOTE = \"The \\\"door\\\" and the \x93key\x94\";\r\n"
                                 "SAMPLE_ESCAPED = \"It\\146s here\\133\";\r\n"
                                 "SAMPLE_ACCENT = \"Configuraci\xC3\xB3n\";\r\n");
    REQUIRE(parsed);
    REQUIRE(parsed.value().size() == 3);
    CHECK(decode(parsed.value()[0].text) == "The \"door\" and the \xE2\x80\x9Ckey\xE2\x80\x9D");
    CHECK(decode(parsed.value()[1].text) == "It\xE2\x80\x99s here\xE2\x80\xA6"); // \ddd escapes are bytes too
    CHECK(decode(parsed.value()[2].text) == "Configuraci\xC3\xB3n");
}

TEST_CASE("Client texts: the quotes lost as U+FFFD come back") {
    const std::string lost(kLost);
    CHECK(restore("No replacement character, caf\xC3\xA9.") == "No replacement character, caf\xC3\xA9.");
    // Between two letters (or digits) an apostrophe; before one an opening quote; otherwise a closing one.
    CHECK(restore("the player" + lost + "s turn") == "the player\xE2\x80\x99s turn");
    CHECK(restore("Port" + lost + "Avel") == "Port\xE2\x80\x99" "Avel");
    CHECK(restore("the 90" + lost + "s") == "the 90\xE2\x80\x99s");
    CHECK(restore("(" + lost + "word" + lost + " and more)") == "(\xE2\x80\x9Cword\xE2\x80\x9D and more)");
    CHECK(restore("a " + lost + "two words" + lost + ".") == "a \xE2\x80\x9Ctwo words\xE2\x80\x9D.");
    CHECK(restore(lost + "start") == "\xE2\x80\x9Cstart");
    CHECK(restore("end" + lost) == "end\xE2\x80\x9D");
    CHECK(restore(lost) == "\xE2\x80\x9D");
    // Other languages' letters are letters too.
    CHECK(restore("vud\xC3\xBA" + lost + "s") == "vud\xC3\xBA\xE2\x80\x99s");
    CHECK(restore("de " + lost + "\xC3\xA9t\xC3\xA9" + lost) == "de \xE2\x80\x9C\xC3\xA9t\xC3\xA9\xE2\x80\x9D");
    // Bytes that are not UTF-8 stay as they are (utf8FromWindows1252 goes first).
    CHECK(restore("a\x93" + lost + "b") == "a\x93\xE2\x80\x9C" "b");
}

TEST_CASE("Client texts: the client's GlobalStrings.lua and GlueStrings.lua come out as UTF-8 with their quotes") {
    VD_REQUIRE_MPQ(mpq);
    // The 1.12.1 enUS files are UTF-8 already, with a few quotes and apostrophes lost as U+FFFD.
    usize lostQuotes = 0;
    for (const char* file :
         {"Interface\\GlueXML\\GlueStrings.lua", "Interface\\FrameXML\\GlobalStrings.lua"}) {
        Result<std::vector<u8>> bytes = mpq->read(file);
        REQUIRE(bytes);
        Result<std::vector<formats::LuaString>> strings = formats::parseLuaStrings(
            std::string_view(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()));
        REQUIRE(strings);
        for (const formats::LuaString& s : strings.value()) {
            CHECK(decode(s.text) == s.text);
            lostQuotes += s.text.find(kLost) != std::string::npos ? 1 : 0;
        }
    }
    CHECK(lostQuotes > 0);
    bake::TextMap texts;
    REQUIRE(bake::readClientTexts(*mpq, texts));
    usize left = 0;
    for (const auto& [key, text] : texts) {
        left += decode(text) != text || text.find(kLost) != std::string::npos ? 1 : 0;
    }
    CHECK(left == 0);
    const std::string& camping = texts.at("PHYSICAL_HARASSMENT_TEXT4");
    CHECK(camping.find("\xE2\x80\x9C") != std::string::npos); // U+201C
    CHECK(camping.find("\xE2\x80\x9D") != std::string::npos); // U+201D
    CHECK(texts.at("PHYSICAL_HARASSMENT_TEXT5").find("\xE2\x80\x99") != std::string::npos); // U+2019
}
