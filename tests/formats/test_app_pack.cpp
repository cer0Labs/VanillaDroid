// Phase 22 (apppack, ADR-0005 §2): app.vdpk, what is ours (texts, art, font), baked without the WoW client
// and read next to ui.vdpk (the clients' texts and art) or alone (a phone without data); the texts' lookup
// order over both packs gives every key the text the merged tables of ui.vdpk gave before.
#include "TestData.h"

#include "bake/AppPackBaker.h"
#include "bake/UiStrings.h"
#include "core/Hash.h"
#include "core/Text.h"
#include "formats/MpqArchiveSet.h"
#include "pack/AppPack.h"
#include "pack/PackReader.h"
#include "pack/UiFormat.h"
#include "ui/UiAssets.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace vd;

std::vector<u8> tableBlob(const bake::TextMap& texts, const char* locale) {
    std::vector<std::pair<std::string, std::vector<u8>>> blobs;
    REQUIRE(bake::stringTableBlobs({{locale, texts}}, blobs));
    REQUIRE(blobs.size() == 1);
    return std::move(blobs.front().second);
}

// The interface's texts as ui.vdpk merged them until phase 22 (UiCommands.cpp's bakeStrings): English is the
// base client's texts with ours on top (our fixes of client texts too); another language with a client of its
// own is that client's texts, then our translations of what it does not define, then English; without a
// client, English with our translations on top.
bake::TextMap oldMerged(const bake::TextMap& baseClient, const bake::TextMap& ownEnglish,
                        const bake::TextMap* client, const bake::TextMap* own) {
    bake::TextMap base = baseClient;
    for (const auto& [key, text] : ownEnglish) {
        base[key] = text;
    }
    if (!own) {
        return base; // English
    }
    bake::TextMap texts = client ? *client : bake::TextMap{};
    for (const auto& [key, text] : base) {
        texts.emplace(key, text);
    }
    for (const auto& [key, text] : *own) {
        if (!client || !client->count(key)) {
            texts[key] = text;
        }
    }
    return texts;
}

// Every key of `merged` reads its text through core/Text, and no key the sources have beyond it.
void checkSameTexts(const bake::TextMap& merged, const std::vector<const bake::TextMap*>& sources) {
    usize differ = 0;
    for (const auto& [key, text] : merged) {
        const std::string_view got = text::get(text::Key(std::string_view(key)));
        if (got != text && ++differ <= 10) {
            MESSAGE(key << ": \"" << std::string(got) << "\" instead of \"" << text << "\"");
        }
    }
    CHECK(differ == 0);
    usize extra = 0;
    for (const bake::TextMap* source : sources) {
        for (const auto& [key, text] : *source) {
            extra += merged.count(key) == 0 && text::has(text::Key(std::string_view(key))) ? 1 : 0;
        }
    }
    CHECK(extra == 0);
}

std::filesystem::path bakeAppPackForTest(const char* name) {
    const std::filesystem::path out = std::filesystem::temp_directory_path() / name;
    std::error_code error;
    std::filesystem::remove(out, error);
    Result<bake::AppPackStats> stats = bake::bakeAppPack(out, VD_LOCALE_DIR, VD_ART_DIR, 1);
    REQUIRE(stats.ok());
    CHECK(stats.value().failures == 0);
    return out;
}

} // namespace

TEST_CASE("App pack: baked from the repository alone, without a WoW client") {
    // No MPQ anywhere: bakeAppPack takes no client (the CI bakes it on a machine without Blizzard data).
    const std::filesystem::path path = bakeAppPackForTest("vd_test_app.vdpk");
    pack::PackReader reader;
    REQUIRE(reader.open(path).ok());
    CHECK(reader.locale().empty()); // ours: no client language
    for (const char* asset : {"ui/strings/enus", "ui/strings/eses", pack::kUiAtlasAsset,
                              pack::kUiSpritesAsset, pack::kUiFontAtlasAsset, pack::kAppFontAsset}) {
        CHECK_MESSAGE(reader.find(asset).has_value(), asset);
    }
    // Our texts only: VD_* keys and our fixes and translations, never a whole client table.
    const std::optional<pack::PackAsset> english = reader.find("ui/strings/enus");
    REQUIRE(english);
    pack::UiStringsHeader header{};
    std::memcpy(&header, english->data, sizeof(header));
    CHECK(header.count > 300);
    CHECK(header.count < 1000);
    // The sprites of our art, under their own paths.
    const std::optional<pack::PackAsset> sprites = reader.find(pack::kUiSpritesAsset);
    REQUIRE(sprites);
    pack::UiSpritesHeader spritesHeader{};
    std::memcpy(&spritesHeader, sprites->data, sizeof(spritesHeader));
    CHECK(spritesHeader.count == bake::ownArt().size());
    // The atlas is mostly empty space: compressed.
    CHECK(reader.find(pack::kUiAtlasAsset)->compression == pack::Compression::Lz4);
    reader.close();
    std::error_code error;
    std::filesystem::remove(path, error);
}

TEST_CASE("App pack: the UI's art, font and texts with app.vdpk alone (a phone without data)") {
    const std::filesystem::path path = bakeAppPackForTest("vd_test_app_alone.vdpk");
    ui::Assets assets;
    REQUIRE(assets.open({}, path).ok());
    CHECK_FALSE(assets.hasClientArt());
    for (const bake::OwnArt& art : bake::ownArt()) {
        const std::optional<ui::Sprite> sprite = assets.sprite(art.path);
        REQUIRE_MESSAGE(sprite.has_value(), art.path);
        CHECK(sprite->width == art.size);
        CHECK(sprite->page == 0);
    }
    CHECK_FALSE(assets.sprite("Interface\\Buttons\\UI-Quickslot2.blp").has_value()); // the client's: not here
    const ui::Font& app = assets.font(ui::FontId::App);
    REQUIRE(app.valid());
    CHECK(app.glyph(U'ñ').codepoint == U'ñ');
    CHECK(app.glyph(U'¿').codepoint == U'¿');
    CHECK(app.layerBase() == 0);
    // Without the client's fonts every font is ours.
    CHECK(&assets.font(ui::FontId::Friz).glyph('A') == &app.glyph('A'));
    CHECK(assets.atlasBlob().size() > sizeof(pack::TextureHeader));
    CHECK(assets.fontAtlasBlob().size() > sizeof(pack::TextureHeader));
    assets.close();

    // Texts: ours in the chosen language, then in English; the client's keys are not there.
    REQUIRE(text::load({}, path, "esES"));
    CHECK(text::get("VD_TOUCH_NEW") == "Nuevo");
    CHECK(text::get("VD_TOUCH_TEMPLATE_MAIN") != "VD_TOUCH_TEMPLATE_MAIN");
    REQUIRE(text::load({}, path, "enUS"));
    CHECK(text::get("VD_TOUCH_NEW") == "New");
    CHECK_FALSE(text::has("ERR_INV_FULL"));
    text::unload();
    std::error_code error;
    std::filesystem::remove(path, error);
}

TEST_CASE("App pack: our pages and font layers after ui.vdpk's (VD_PACKS_DIR)") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    const std::filesystem::path packs = dir ? std::filesystem::path(dir) : std::filesystem::path();
    std::error_code error;
    if (!dir || !std::filesystem::exists(packs / "ui.vdpk", error) ||
        !std::filesystem::exists(packs / pack::kAppPackFile, error)) {
        MESSAGE("VD_PACKS_DIR not set or ui.vdpk / app.vdpk missing: skipping");
        return;
    }
    // ui.vdpk's own layers.
    pack::PackReader ui;
    REQUIRE(ui.open(packs / "ui.vdpk").ok());
    const auto layersOf = [](const pack::PackAsset& asset) {
        std::vector<u8> raw(asset.rawSize);
        REQUIRE(pack::PackReader::read(asset, raw.data(), raw.size()).ok());
        pack::TextureHeader header{};
        std::memcpy(&header, raw.data(), sizeof(header));
        return u32{header.layers};
    };
    const u32 uiPages = layersOf(*ui.find(pack::kUiAtlasAsset));
    const u32 uiFontLayers = layersOf(*ui.find(pack::kUiFontAtlasAsset));
    CHECK_FALSE(ui.find(pack::kAppFontAsset).has_value()); // ours are not in it any more
    ui.close();

    ui::Assets assets;
    REQUIRE(assets.open(packs / "ui.vdpk").ok()); // app.vdpk next to it
    CHECK(assets.hasClientArt());
    CHECK(assets.hasOwnArt());
    pack::TextureHeader atlas{}, fonts{};
    std::memcpy(&atlas, assets.atlasBlob().data(), sizeof(atlas));
    std::memcpy(&fonts, assets.fontAtlasBlob().data(), sizeof(fonts));
    CHECK(atlas.layers > uiPages);
    CHECK(fonts.layers > uiFontLayers);
    // Our sprites on the pages after ui.vdpk's, the client's where they were.
    for (const bake::OwnArt& art : bake::ownArt()) {
        const std::optional<ui::Sprite> sprite = assets.sprite(art.path);
        REQUIRE_MESSAGE(sprite.has_value(), art.path);
        CHECK(sprite->page >= uiPages);
        CHECK(sprite->page < atlas.layers);
    }
    const std::optional<ui::Sprite> quickslot = assets.sprite("Interface\\Buttons\\UI-Quickslot2.blp");
    REQUIRE(quickslot.has_value());
    CHECK(quickslot->page < uiPages);
    // Our font's glyphs count from the layer after ui.vdpk's; the client's fonts stay theirs.
    CHECK(assets.font(ui::FontId::App).layerBase() == uiFontLayers);
    CHECK(assets.font(ui::FontId::Friz).layerBase() == 0);
    CHECK(&assets.font(ui::FontId::App).glyph('A') != &assets.font(ui::FontId::Friz).glyph('A'));
    assets.releaseTextureBlobs();
    CHECK(assets.atlasBlob().empty());
    CHECK(assets.sprite("Interface\\Buttons\\UI-Quickslot2.blp").has_value()); // the tables stay
}

TEST_CASE("Texts: the lookup order over the clients' tables and ours") {
    // The base client (English), a Spanish client, our English and our Spanish.
    const std::vector<u8> clientBase = tableBlob({{"ERR_A", "client A"},
                                                  {"ERR_FIXED", "client typo"},
                                                  {"ERR_ONLY_BASE", "base only"},
                                                  {"VD_GLUE_SHOW_ESRB_RATING", "1"}},
                                                 "enUS");
    const std::vector<u8> clientSpanish = tableBlob(
        {{"ERR_A", "cliente A"}, {"ERR_FIXED", "cliente"}, {"VD_GLUE_SHOW_ESRB_RATING", "0"}}, "esES");
    const std::vector<u8> ownEnglish =
        tableBlob({{"VD_X", "ours X"}, {"VD_Y", "ours Y"}, {"ERR_FIXED", "client fixed"}}, "enUS");
    const std::vector<u8> ownSpanish = tableBlob({{"VD_X", "nuestro X"}, {"ERR_A", "traducido A"}}, "esES");

    SUBCASE("English: our texts (and fixes) over the client's") {
        text::setTables({"enUS", {}, clientBase, ownEnglish, ownEnglish});
        CHECK(text::get("ERR_A") == "client A");
        CHECK(text::get("ERR_FIXED") == "client fixed");
        CHECK(text::get("VD_X") == "ours X");
        CHECK(text::get("VD_GLUE_SHOW_ESRB_RATING") == "1");
    }
    SUBCASE("Spanish with its client: the client's, then ours, then our English, then the base client") {
        text::setTables({"esES", clientSpanish, clientBase, ownSpanish, ownEnglish});
        CHECK(text::get("ERR_A") == "cliente A"); // the client's own wording wins over our translation
        CHECK(text::get("ERR_FIXED") == "cliente");
        CHECK(text::get("VD_X") == "nuestro X");
        CHECK(text::get("VD_Y") == "ours Y");
        CHECK(text::get("ERR_ONLY_BASE") == "base only");
        CHECK(text::get("VD_GLUE_SHOW_ESRB_RATING") == "0");
    }
    SUBCASE("Spanish without its client: our translations over English") {
        text::setTables({"esES", {}, clientBase, ownSpanish, ownEnglish});
        CHECK(text::get("ERR_A") == "traducido A");
        CHECK(text::get("ERR_FIXED") == "client fixed");
        CHECK(text::get("VD_X") == "nuestro X");
        CHECK(text::get("ERR_ONLY_BASE") == "base only");
    }
    text::unload();
}

TEST_CASE("Texts: the clients' tables (ui.vdpk) and ours (app.vdpk) give every key the text the merged table "
          "gave") {
    VD_REQUIRE_WOW_DATA(data);
    formats::MpqArchiveSet base;
    REQUIRE(base.open(*data).ok());
    bake::TextMap baseClient;
    REQUIRE(bake::readClientTexts(base, baseClient));
    REQUIRE(baseClient.at("SLASH_WHO1") == "/who"); // the reference client is enUS
    bake::TextTables own;
    REQUIRE(bake::ownTextTables(VD_LOCALE_DIR, own));
    REQUIRE(own.front().first == "enUS");
    const bake::TextMap& ownEnglish = own.front().second;
    const bake::TextMap* ownSpanish = nullptr;
    for (const auto& [locale, texts] : own) {
        ownSpanish = locale == "esES" ? &texts : ownSpanish;
    }
    REQUIRE(ownSpanish);
    // Our translations of client keys must be keys of the client (VD_* keys are checked when baking).
    usize unknown = 0;
    for (const auto& [key, text] : *ownSpanish) {
        if (key.rfind("VD_", 0) != 0 && !baseClient.count(key) && ++unknown <= 10) {
            MESSAGE("esES: " << key << " is not a key of the enUS client");
        }
    }
    CHECK(unknown == 0);

    const std::vector<u8> baseBlob = tableBlob(baseClient, "enUS");
    const std::vector<u8> ownEnglishBlob = tableBlob(ownEnglish, "enUS");
    const std::vector<u8> ownSpanishBlob = tableBlob(*ownSpanish, "esES");

    // English.
    text::setTables({"enUS", {}, baseBlob, ownEnglishBlob, ownEnglishBlob});
    checkSameTexts(oldMerged(baseClient, ownEnglish, nullptr, nullptr), {&baseClient, &ownEnglish});
    // Spanish without its client: our translations over the English client.
    text::setTables({"esES", {}, baseBlob, ownSpanishBlob, ownEnglishBlob});
    checkSameTexts(oldMerged(baseClient, ownEnglish, nullptr, ownSpanish),
                   {&baseClient, &ownEnglish, ownSpanish});

    // Spanish with its own client (VD_WOW_LOCALE_DIRS, else the esES client next to the enUS one).
    std::filesystem::path spanishDir = std::filesystem::path(data->parent_path().string() + " esES");
    if (const char* dirs = std::getenv("VD_WOW_LOCALE_DIRS"); dirs && *dirs) {
        const std::string list(dirs);
        spanishDir = list.substr(0, list.find(';'));
    }
    formats::MpqArchiveSet spanish;
    std::error_code error;
    if (!std::filesystem::is_directory(spanishDir / "Data", error) ||
        !spanish.open(spanishDir / "Data").ok()) {
        MESSAGE("no esES client at " << spanishDir.string() << ": skipping its texts");
        text::unload();
        return;
    }
    bake::TextMap spanishClient;
    REQUIRE(bake::readClientTexts(spanish, spanishClient));
    REQUIRE(spanishClient.at("SLASH_WHO1") == "/quien");
    const std::vector<u8> spanishBlob = tableBlob(spanishClient, "esES");
    text::setTables({"esES", spanishBlob, baseBlob, ownSpanishBlob, ownEnglishBlob});
    checkSameTexts(oldMerged(baseClient, ownEnglish, &spanishClient, ownSpanish),
                   {&baseClient, &ownEnglish, ownSpanish, &spanishClient});
    // English is still the base client with ours, the Spanish client aside.
    text::setTables({"enUS", {}, baseBlob, ownEnglishBlob, ownEnglishBlob});
    checkSameTexts(oldMerged(baseClient, ownEnglish, nullptr, nullptr), {&baseClient, &ownEnglish});
    text::unload();
}
