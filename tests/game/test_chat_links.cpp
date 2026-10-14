// The chat's hyperlinks (parity, game/ChatLinks): "|H<link>|h<text>|h" as the 1.12 client writes them (items,
// enchants, players), found in a line, read, kept in what players type and built for the client's own lines.
#include "TestData.h"
#include "core/Text.h"
#include "game/ChatLinks.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <ostream>
#include <string>

using namespace vd;
using namespace vd::game;

TEST_CASE("Chat links: an item, a player and an enchant are found and read") {
    const std::string line =
        "[2. Trade] |Hplayer:Galen|h[Galen]|h: WTS |cff1eff00|Hitem:2132:0:15:0|h[Militia Shortsword]|h|r "
        "and |Henchant:7418|h[Enchant Bracer - Minor Health]|h";
    ChatLink link;
    REQUIRE(nextChatLink(line, 0, link));
    CHECK(link.kind == ChatLinkKind::Player);
    CHECK(link.player == "Galen");
    CHECK(link.text == "[Galen]");
    CHECK(line.substr(link.begin, 2) == "|H");
    CHECK(line.substr(link.end - 2, 2) == "|h");

    REQUIRE(nextChatLink(line, link.end, link));
    CHECK(link.kind == ChatLinkKind::Item);
    CHECK(link.id == 2132);
    CHECK(link.values[0] == 0);  // enchant
    CHECK(link.values[1] == 15); // random property
    CHECK(link.link == "item:2132:0:15:0");
    CHECK(link.text == "[Militia Shortsword]");
    CHECK(line.substr(link.end, 2) == "|r"); // the quality colour closes after the link

    REQUIRE(nextChatLink(line, link.end, link));
    CHECK(link.kind == ChatLinkKind::Enchant);
    CHECK(link.id == 7418);
    CHECK(!nextChatLink(line, link.end, link));

    // What a tap at a byte of the drawn line opens: the link around it, nothing between links.
    const usize sword = line.find("Militia");
    REQUIRE(chatLinkAt(line, sword, link));
    CHECK(link.kind == ChatLinkKind::Item);
    CHECK(!chatLinkAt(line, line.find("WTS"), link));
    CHECK(!chatLinkAt(line, 0, link));
    REQUIRE(chatLinkAt(line, line.find("[Galen]"), link));
    CHECK(link.kind == ChatLinkKind::Player);
}

TEST_CASE("Chat links: malformed links are not links") {
    ChatLink link;
    for (const char* bad : {"|Hitem:12", "|Hitem:12|h[Sword]", "|Hitem:12|x[Sword]|h",
                            "||Hitem:12|h[Sword]|h", "|H|h[Nothing]|h", "text|", "|"}) {
        INFO(bad);
        CHECK(!nextChatLink(bad, 0, link));
    }
    // Unknown or incomplete <link>s are links of no kind (shown, nothing to open).
    REQUIRE(nextChatLink("|Hitem:abc:0|h[x]|h", 0, link));
    CHECK(link.kind == ChatLinkKind::Other);
    REQUIRE(nextChatLink("|Hitem:0:0:0:0|h[x]|h", 0, link));
    CHECK(link.kind == ChatLinkKind::Other);
    REQUIRE(nextChatLink("|Hplayer:|h[x]|h", 0, link));
    CHECK(link.kind == ChatLinkKind::Other);
    REQUIRE(nextChatLink("|Htrade:1:2|h[x]|h", 0, link));
    CHECK(link.kind == ChatLinkKind::Other);
    // An item link with only its entry (missing numbers are 0) and a quest's.
    REQUIRE(nextChatLink("|Hitem:6948|h[Hearthstone]|h", 0, link));
    CHECK(link.kind == ChatLinkKind::Item);
    CHECK(link.id == 6948);
    REQUIRE(nextChatLink("|Hquest:7:5|h[Kobold Camp Cleanup]|h", 0, link));
    CHECK(link.kind == ChatLinkKind::Quest);
    CHECK(link.id == 7);
    CHECK(link.values[0] == 5);
}

TEST_CASE("Chat links: typed text keeps colours and links, broken escapes show as text") {
    const auto typed = [](std::string_view text) {
        std::string out;
        appendChatText(out, text);
        return out;
    };
    CHECK(typed("hello") == "hello");
    CHECK(typed("WTS |cff1eff00|Hitem:2132:0:0:0|h[Militia Shortsword]|h|r 1g") ==
          "WTS |cff1eff00|Hitem:2132:0:0:0|h[Militia Shortsword]|h|r 1g");
    CHECK(typed("a || b") == "a || b");
    // A link that never closes would hide the rest of the line; a stray |h, a line break, a texture: shown as
    // typed.
    CHECK(typed("look |Hitem:12 here") == "look ||Hitem:12 here");
    CHECK(typed("x |h y") == "x ||h y");
    CHECK(typed("one|ntwo") == "one||ntwo");
    CHECK(typed("bad |cffzz0000colour") == "bad ||cffzz0000colour");
    CHECK(typed("end |") == "end ||");
    // Every link of the result is one of the input's.
    const std::string mixed = typed("|Hitem:1|h[A]|h |Hitem:2|h[B] |Hplayer:Aeryn|h[Aeryn]|h");
    CHECK(mixed == "|Hitem:1|h[A]|h ||Hitem:2||h[B] |Hplayer:Aeryn|h[Aeryn]|h");
    ChatLink link;
    REQUIRE(nextChatLink(mixed, 0, link));
    CHECK(link.id == 1);
    REQUIRE(nextChatLink(mixed, link.end, link));
    CHECK(link.player == "Aeryn");
    CHECK(!nextChatLink(mixed, link.end, link));
}

TEST_CASE("Chat links: items as the client links them") {
    char out[128];
    CHECK(formatItemLink(out, 2589, "|cffffffff", "Linen Cloth") ==
          "|cffffffff|Hitem:2589:0:0:0|h[Linen Cloth]|h|r");
    ChatLink link;
    REQUIRE(nextChatLink(out, 0, link));
    CHECK(link.kind == ChatLinkKind::Item);
    CHECK(link.id == 2589);
    // Cut to fit, always terminated.
    char small[12];
    CHECK(formatItemLink(small, 2589, "|cffffffff", "Linen Cloth").size() == 11);
    CHECK(small[11] == 0);
}

TEST_CASE("Chat texts: the docked tabs' names and the edit box's headers, in English and Spanish") {
    if (!test::loadTexts()) {
        MESSAGE("VD_PACKS_DIR not set or packs missing: skipping");
        return;
    }
    constexpr const char* kKeys[] = {"GENERAL",
                                     "COMBAT_LOG",
                                     "CHAT_MSG_WHISPER_INFORM",
                                     "PARTY",
                                     "GUILD",
                                     "CHAT_SAY_SEND",
                                     "CHAT_YELL_SEND",
                                     "CHAT_PARTY_SEND",
                                     "CHAT_RAID_SEND",
                                     "CHAT_GUILD_SEND",
                                     "CHAT_OFFICER_SEND",
                                     "CHAT_EMOTE_SEND",
                                     "CHAT_CHANNEL_SEND",
                                     "CHAT_WHISPER_SEND",
                                     "CHAT_RAID_WARNING_SEND",
                                     "EMOTE_MESSAGE",
                                     "SEND_LABEL",
                                     "VD_COMBAT_LOG_ONLY_MINE",
                                     "VD_COMBAT_LOG_CLEAR"};
    char buffer[96];
    for (const char* key : kKeys) {
        INFO(key);
        CHECK(text::has(key));
    }
    // The client writes the header's trailing space as a Lua decimal escape: baked as a space.
    CHECK(text::get("CHAT_SAY_SEND") == "Say: ");
    CHECK(text::format(buffer, "CHAT_CHANNEL_SEND", 2u, "Trade") == "[2. Trade]: ");
    REQUIRE(text::load(std::filesystem::path(std::getenv("VD_PACKS_DIR")) / "ui.vdpk", "esES"));
    CHECK(text::get("CHAT_SAY_SEND") == "Decir: ");
    CHECK(text::get("COMBAT_LOG") == (test::esESClientTexts() ? "Registro del combate" : "Registro de combate"));
    CHECK(text::format(buffer, "CHAT_CHANNEL_SEND", 2u, "Comercio") == "[2. Comercio]: ");
    text::unload();
}
