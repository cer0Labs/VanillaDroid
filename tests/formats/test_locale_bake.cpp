// Phase 21 (languages): the texts of a client in another language moved to the first column of each DBC text, the
// language overlays of the packs, and with real data (when the overlays were baked) the Spanish names under the
// English ones the server matches.
#include "bake/LocaleBaker.h"
#include "bake/PackWriter.h"
#include "core/Locale.h"
#include "db/DbcView.h"
#include "game/SocialData.h"
#include "pack/PackReader.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using namespace vd;

// A WDBC table: `records` of `fields` u32 each, then the string block.
std::vector<u8> makeDbc(const std::vector<std::vector<u32>>& records, u32 fields, const std::string& strings) {
    std::vector<u8> out(20 + records.size() * fields * 4 + strings.size());
    const u32 header[5] = {0x43424457u, static_cast<u32>(records.size()), fields, fields * 4,
                           static_cast<u32>(strings.size())};
    std::memcpy(out.data(), header, sizeof(header));
    for (usize r = 0; r < records.size(); ++r) {
        std::memcpy(out.data() + 20 + r * fields * 4, records[r].data(), fields * 4);
    }
    std::memcpy(out.data() + 20 + records.size() * fields * 4, strings.data(), strings.size());
    return out;
}

std::filesystem::path tempPath(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

bool writePack(const std::filesystem::path& path, const char* locale,
               const std::vector<std::pair<std::string, std::string>>& assets) {
    bake::PackWriter writer;
    if (!writer.create(path, 1).ok()) {
        return false;
    }
    if (locale) {
        writer.setLocale(locale);
    }
    for (const auto& [name, bytes] : assets) {
        if (!writer.add(name, pack::AssetType::Dbc, {reinterpret_cast<const u8*>(bytes.data()), bytes.size()}).ok()) {
            return false;
        }
    }
    return writer.finish().ok();
}

std::string_view text(const pack::PackAsset& asset) {
    return {reinterpret_cast<const char*>(asset.data), asset.size};
}

} // namespace

TEST_CASE("Client languages: codes and their columns") {
    CHECK(localeIndex("enUS") == 0);
    CHECK(localeIndex("esES") == 6);
    CHECK(localeIndex("ESES") == 6);
    CHECK(localeIndex("esMX") == 7);
    CHECK(localeIndex("caES") == -1);
    CHECK(canonicalLocale("eses") == "esES");
    CHECK(canonicalLocale("frfr") == "frFR");
    CHECK(canonicalLocale("xx").empty());
}

TEST_CASE("Client languages: a DBC text in the esES column moves to the first") {
    // id, a text (8 strings + flags) with esES in its 7th column, a number, another text.
    const std::string strings = std::string("\0Bosque de Elwynn\0Ventormenta\0Ciudad\0", 37);
    const u32 elwynn = 1, stormwind = 18, city = 30;
    const u32 kFields = 1 + 9 + 1 + 9;
    std::vector<std::vector<u32>> records = {
        {12, 0, 0, 0, 0, 0, 0, elwynn, 0, 0x3f007e, 7, 0, 0, 0, 0, 0, 0, city, 0, 0x3f007e},
        {1519, 0, 0, 0, 0, 0, 0, stormwind, 0, 0x3f007e, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x3f007e},
    };
    const std::vector<u8> dbc = makeDbc(records, kFields, strings);
    Result<db::DbcView> view = db::DbcView::parse(dbc.data(), dbc.size());
    REQUIRE(view);
    CHECK(bake::findLocaleTexts(view.value(), 6) == std::vector<u32>{1, 11});
    CHECK(bake::findLocaleTexts(view.value(), 0).empty());
    CHECK(bake::detectLocaleColumn(view.value()) == 6);

    u32 moved = 0;
    Result<std::vector<u8>> localized = bake::localizeDbc(dbc, 6, &moved);
    REQUIRE(localized);
    CHECK(moved == 2);
    Result<db::DbcView> out = db::DbcView::parse(localized.value().data(), localized.value().size());
    REQUIRE(out);
    CHECK(out.value().getString(0, 1) == "Bosque de Elwynn");
    CHECK(out.value().getString(1, 1) == "Ventormenta");
    CHECK(out.value().getString(0, 11) == "Ciudad");
    CHECK(out.value().getString(1, 11).empty());
    CHECK(out.value().getU32(0, 10) == 7); // the number stays
    CHECK(out.value().getU32(0, 9) == 0x3f007e);

    // The enUS client's column needs nothing.
    Result<std::vector<u8>> same = bake::localizeDbc(dbc, 0, &moved);
    REQUIRE(same);
    CHECK(moved == 0);
    CHECK(same.value() == dbc);
}

TEST_CASE("Client languages: numbers are not taken for texts") {
    // Mostly zeros, one column with small numbers that are no string starts ("Ab" at 1: 2 is inside it).
    const std::string strings = std::string("\0Ab\0", 4);
    std::vector<std::vector<u32>> records = {
        {1, 0, 0, 0, 0, 0, 0, 2, 0, 5},
        {2, 0, 0, 0, 0, 0, 0, 0, 0, 5},
    };
    const std::vector<u8> dbc = makeDbc(records, 10, strings);
    Result<db::DbcView> view = db::DbcView::parse(dbc.data(), dbc.size());
    REQUIRE(view);
    CHECK(bake::findLocaleTexts(view.value(), 6).empty());
    CHECK(bake::detectLocaleColumn(view.value()) == -1);
}

TEST_CASE("Packs: a language overlay replaces assets under the same paths") {
    const std::filesystem::path base = tempPath("vd_locale_test.vdpk");
    const std::filesystem::path overlay = tempPath("vd_locale_test.esES.vdpk");
    REQUIRE(writePack(base, "enUS", {{"a.dbc", "base a"}, {"b.dbc", "base b"}}));
    REQUIRE(writePack(overlay, "esES", {{"b.dbc", "overlay b"}, {"c.dbc", "overlay c"}}));

    pack::PackReader reader;
    REQUIRE(reader.open(base).ok());
    CHECK(reader.locale() == "enUS");
    CHECK(reader.dataLocale() == "enUS");
    REQUIRE(reader.openOverlay("frFR").ok()); // no such file: nothing changes
    CHECK_FALSE(reader.hasOverlay());
    REQUIRE(reader.openOverlay("esES").ok());
    REQUIRE(reader.hasOverlay());
    CHECK(reader.dataLocale() == "esES");
    CHECK(text(*reader.find("a.dbc")) == "base a");
    CHECK(text(*reader.find("b.dbc")) == "overlay b");
    CHECK(text(*reader.find("c.dbc")) == "overlay c");
    CHECK(text(*reader.findBase("b.dbc")) == "base b");
    CHECK_FALSE(reader.findBase("c.dbc"));
    // Iteration walks the base's entries, with the overlay's asset where it has one.
    REQUIRE(reader.entryCount() == 2);
    std::vector<std::string> seen;
    for (u32 i = 0; i < reader.entryCount(); ++i) {
        seen.emplace_back(text(reader.entry(i)));
    }
    CHECK(std::find(seen.begin(), seen.end(), "overlay b") != seen.end());
    CHECK(std::find(seen.begin(), seen.end(), "base a") != seen.end());
    reader.close();
    CHECK_FALSE(reader.hasOverlay());

    // An overlay must say its language (a pack of another kind is refused).
    REQUIRE(writePack(overlay, nullptr, {{"b.dbc", "no locale"}}));
    REQUIRE(reader.open(base).ok());
    CHECK_FALSE(reader.openOverlay("esES").ok());
    CHECK_FALSE(reader.hasOverlay());
    reader.close();
    std::error_code error;
    std::filesystem::remove(base, error);
    std::filesystem::remove(overlay, error);
}

TEST_CASE("Client languages (real data): Spanish names over the English the server matches") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    if (!dir || !std::filesystem::exists(std::filesystem::path(dir) / "db.esES.vdpk")) {
        MESSAGE("skipped: no db.esES.vdpk (vd_baker bake --only locale with the esES client)");
        return;
    }
    pack::PackReader db;
    REQUIRE(db.open(std::filesystem::path(dir) / "db.vdpk").ok());
    REQUIRE(db.openOverlay("esES").ok());
    REQUIRE(db.hasOverlay());
    CHECK(db.overlay()->locale() == "esES");

    const auto areaName = [](const std::optional<pack::PackAsset>& asset, u32 id) {
        Result<db::DbcView> view = db::DbcView::parse(asset->data, asset->size);
        const u32 r = view.value().findById(id);
        return std::string(view.value().getString(r, 11)); // AreaTable: the name's first column
    };
    CHECK(areaName(db.find("DBFilesClient\\AreaTable.dbc"), 12) == "Bosque de Elwynn");
    CHECK(areaName(db.findBase("DBFilesClient\\AreaTable.dbc"), 12) == "Elwynn Forest");

    // The zone channels go out with the English names and show in Spanish.
    game::SocialData social;
    REQUIRE(social.init(db));
    const std::span<const game::ZoneChannel> channels = social.zoneChannels(12);
    REQUIRE(!channels.empty());
    CHECK(channels[0].name == "General - Elwynn Forest");
    char shown[96];
    // The esES client's patterns put a colon where enUS has " - " ("General: %s").
    CHECK(social.displayChannel("1. General - Elwynn Forest", shown) == "1. General: Bosque de Elwynn");
    CHECK(social.displayChannel("Trade - City", shown) == "Comercio: Ciudad");
    CHECK(social.displayChannel("MyChannel", shown) == "MyChannel");
    CHECK(social.displayChannel("General - Somewhere", shown) == "General: Somewhere"); // an unknown zone stays
    // ChatFrame's cut of " - " leaves those whole, as in the esES client.
    CHECK(social.displayChannelBase("Trade - City", shown) == "Comercio: Ciudad");
    CHECK(social.displayChannelBase("MyChannel", shown) == "MyChannel");
    CHECK(social.channelKind("Trade - City") == 2);
    CHECK(social.areaName(12) == "Bosque de Elwynn");
}
