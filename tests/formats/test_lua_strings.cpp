// The client's string tables (GlobalStrings.lua, GlueStrings.lua) and our locale/*.lang files: parsing, and the
// baked "ui/strings" table read back through core/Text.
#include "bake/UiBaker.h"
#include "core/Text.h"
#include "formats/LuaStrings.h"

#include "SharedMpq.h"

#include <doctest/doctest.h>

#include <string>

using namespace vd;

TEST_CASE("Lua string tables: assignments, comments, escapes, errors") {
    const char* source = "\xEF\xBB\xBF-- AUTOMATICALLY GENERATED -- DO NOT EDIT!\r\n"
                         "\r\n"
                         "ABANDON_QUEST_CONFIRM = \"Abandon \\\"%s\\\"?\";\r\n"
                         "CHAT_SAY_GET = \"%s says:\\32\"; -- Normal message from player %s\r\n"
                         "ARMOR_TOOLTIP = \"First line.\\nSecond line.\";\n"
                         "  NUM_BAG_SLOTS = 4;\n"
                         "VOICEMACRO_0_Dw_0 = \"Help me!\";\n"
                         "PATH = \"a\\\\b\";";
    Result<std::vector<formats::LuaString>> parsed = formats::parseLuaStrings(source);
    REQUIRE(parsed);
    const std::vector<formats::LuaString>& s = parsed.value();
    REQUIRE(s.size() == 5);
    CHECK(s[0].key == "ABANDON_QUEST_CONFIRM");
    CHECK(s[0].text == "Abandon \"%s\"?");
    CHECK(s[1].text == "%s says: ");
    CHECK(s[2].text == "First line.\nSecond line.");
    CHECK(s[3].key == "VOICEMACRO_0_Dw_0");
    CHECK(s[4].text == "a\\b");

    CHECK_FALSE(formats::parseLuaStrings("KEY = \"no end;\n"));
    CHECK_FALSE(formats::parseLuaStrings("KEY \"text\";\n"));
    CHECK_FALSE(formats::parseLuaStrings("1KEY = \"text\";\n"));
}

TEST_CASE("Lua string tables: baked table, looked up and formatted") {
    const std::vector<bake::UiText> texts = {{"LOOT_ITEM_SELF", "You receive loot: %s."},
                                             {"SPELLLOGSELFOTHER", "Your %s hits %s for %d."},
                                             {"VD_SELL_JUNK", "Sell junk"}};
    Result<std::vector<u8>> blob = bake::buildStringTable(texts);
    REQUIRE(blob);
    text::setTable(blob.value());
    char buffer[96];
    CHECK(text::format(buffer, "SPELLLOGSELFOTHER", "Fireball", "Young Wolf", 25) ==
          "Your Fireball hits Young Wolf for 25.");
    CHECK(text::get("VD_SELL_JUNK") == "Sell junk");
    text::unload();
}

TEST_CASE("Lua string tables: the client's GlobalStrings.lua and GlueStrings.lua") {
    VD_REQUIRE_MPQ(mpq);
    Result<std::vector<u8>> global = mpq->read("Interface\\FrameXML\\GlobalStrings.lua");
    REQUIRE(global);
    Result<std::vector<formats::LuaString>> strings = formats::parseLuaStrings(
        std::string_view(reinterpret_cast<const char*>(global.value().data()), global.value().size()));
    REQUIRE(strings);
    CHECK(strings.value().size() == 5470);
    std::vector<bake::UiText> texts;
    for (const formats::LuaString& s : strings.value()) {
        texts.push_back({s.key, s.text});
    }
    Result<std::vector<u8>> glue = mpq->read("Interface\\GlueXML\\GlueStrings.lua");
    REQUIRE(glue);
    Result<std::vector<formats::LuaString>> glueStrings = formats::parseLuaStrings(
        std::string_view(reinterpret_cast<const char*>(glue.value().data()), glue.value().size()));
    REQUIRE(glueStrings);
    CHECK(glueStrings.value().size() == 429);
    Result<std::vector<u8>> blob = bake::buildStringTable(texts); // no two keys share a hash
    REQUIRE(blob);
    text::setTable(blob.value());
    CHECK(text::get("ERR_INV_FULL") == "Inventory is full.");
    CHECK(text::get("CHAT_SAY_GET") == "%s says: ");
    char buffer[96];
    CHECK(text::format(buffer, "ITEM_MOD_STAMINA", '+', 3) == "+3 Stamina");
    const std::string_view xp = text::format(buffer, "COMBATLOG_XPGAIN_FIRSTPERSON", "Kobold Vermin", 45);
    CHECK(xp.starts_with("Kobold Vermin dies, "));
    CHECK(xp.ends_with(" 45 experience."));
    text::unload();
}
