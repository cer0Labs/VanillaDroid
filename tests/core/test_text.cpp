// The interface's texts (core/Text): the strings table lookup and the safe formatter for texts that come from data.
#include "core/Hash.h"
#include "core/Text.h"
#include "pack/UiFormat.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace vd;

// A table blob as the baker writes it (pack::UiStringsHeader + sorted UiString + texts).
std::vector<u8> buildTable(std::vector<std::pair<std::string, std::string>> texts) {
    std::sort(texts.begin(), texts.end(),
              [](const auto& a, const auto& b) { return stringHash(a.first) < stringHash(b.first); });
    pack::UiStringsHeader header{};
    header.count = static_cast<u32>(texts.size());
    header.textOffset = static_cast<u32>(sizeof(header) + texts.size() * sizeof(pack::UiString));
    std::vector<pack::UiString> entries;
    std::string block;
    for (const auto& [key, text] : texts) {
        entries.push_back({stringHash(key), static_cast<u32>(block.size()), static_cast<u32>(text.size())});
        block += text;
        block.push_back('\0');
    }
    header.textSize = static_cast<u32>(block.size());
    std::vector<u8> blob(header.textOffset + block.size());
    std::memcpy(blob.data(), &header, sizeof(header));
    std::memcpy(blob.data() + sizeof(header), entries.data(), entries.size() * sizeof(pack::UiString));
    std::memcpy(blob.data() + header.textOffset, block.data(), block.size());
    return blob;
}

std::string fmt(std::string_view format, std::vector<text::Arg> args) {
    char buffer[256];
    return std::string(text::vformat(buffer, format, args));
}

} // namespace

TEST_CASE("Texts: lookup by key, missing keys show themselves") {
    const std::vector<u8> blob = buildTable({{"ERR_INV_FULL", "Inventory is full."},
                                            {"LOOT_ITEM_SELF", "You receive loot: %s."},
                                            {"VD_SELL_JUNK", "Sell junk (%s)"}});
    text::setTable(blob);
    CHECK(text::loaded());
    CHECK(text::get("ERR_INV_FULL") == "Inventory is full.");
    CHECK(text::has("VD_SELL_JUNK"));
    CHECK_FALSE(text::has("ERR_NOPE"));
    CHECK(text::get("ERR_NOPE") == "ERR_NOPE");
    char buffer[64];
    CHECK(text::format(buffer, "LOOT_ITEM_SELF", "Linen Cloth") == "You receive loot: Linen Cloth.");
    std::string out = "> ";
    text::append(out, "VD_SELL_JUNK", "78c");
    CHECK(out == "> Sell junk (78c)");
    text::unload();
    CHECK_FALSE(text::loaded());
    CHECK(text::get("ERR_INV_FULL") == "ERR_INV_FULL");
}

TEST_CASE("Texts: the client's format specifiers, safely") {
    // Specifiers as GlobalStrings.lua uses them: COMBATLOG_XPGAIN_FIRSTPERSON's, ITEM_MOD_STAMINA's (%c is the sign),
    // DURABILITY_TEMPLATE's...
    CHECK(fmt("%s slain: +%d experience.", {"Kobold Vermin", 45}) == "Kobold Vermin slain: +45 experience.");
    CHECK(fmt("%c%d Stamina", {'+', 3}) == "+3 Stamina");
    CHECK(fmt("Durability %d / %d", {12u, 40u}) == "Durability 12 / 40");
    CHECK(fmt("%.1f%%", {5.26f}) == "5.3%");
    CHECK(fmt("%.2f sec", {1.5}) == "1.50 sec");
    CHECK(fmt("+%g damage a second", {2.5}) == "+2.5 damage a second");
    CHECK(fmt("%02d:%02d", {7, 5}) == "07:05");
    CHECK(fmt("%-4s|", {"ab"}) == "ab  |");
    CHECK(fmt("Level |cffffffff%d|r", {10}) == "Level |cffffffff10|r");
    // Positional arguments: a translation may reorder them.
    CHECK(fmt("%2$s de %1$s", {"Elwynn", "Bosque"}) == "Bosque de Elwynn");
    CHECK(fmt("%1$s, %1$s!", {"Hola"}) == "Hola, Hola!");
    // Wrong or missing arguments never crash nor read garbage.
    CHECK(fmt("%s and %s", {"one"}) == "one and ");
    CHECK(fmt("%d items", {"five"}) == "five items");
    CHECK(fmt("%s items", {5}) == "5 items");
    CHECK(fmt("%n %z %", {}) == "%n %z %");
    CHECK(fmt("100%% done", {}) == "100% done");
    // Truncation keeps the NUL inside the buffer.
    char small[8];
    CHECK(text::vformat(small, "%s", std::vector<text::Arg>{"a long text"}) == "a long ");
    CHECK(small[7] == '\0');
}

TEST_CASE("Texts: the gender markers of translations and cuts between characters (phase 21)") {
    // "$g<male>:<female>;" by the player's gender, as the esES client's texts use it
    // (ABANDON_QUEST_CONFIRM_WITH_ITEMS).
    const char* abandon = "Vas a tirar \"%s\" y %s. ¿Estás $glisto:lista;?";
    text::setGender(0);
    CHECK(fmt(abandon, {"Hogger", "2 objetos"}) == "Vas a tirar \"Hogger\" y 2 objetos. ¿Estás listo?");
    text::setGender(1);
    CHECK(fmt(abandon, {"Hogger", "2 objetos"}) == "Vas a tirar \"Hogger\" y 2 objetos. ¿Estás lista?");
    text::setGender(0);
    // A '$' that is no marker stays.
    CHECK(fmt("$5 or $gnothing", {}) == "$5 or $gnothing");
    // A full buffer cuts before a character, not inside one ("ñ" is two bytes).
    char tiny[5];
    CHECK(text::vformat(tiny, "%s", std::vector<text::Arg>{"abcñd"}) == "abc");
}
