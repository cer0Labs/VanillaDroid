#include "formats/Blp.h"
#include "formats/MpqArchiveSet.h"

#include "SharedMpq.h"

#include <doctest/doctest.h>

namespace {

vd::formats::BlpImage loadBlp(const vd::formats::MpqArchiveSet& mpq, const char* path) {
    vd::Result<std::vector<vd::u8>> bytes = mpq.read(path);
    REQUIRE_MESSAGE(bytes.ok(), path);
    vd::Result<vd::formats::BlpImage> image = vd::formats::decodeBlp(bytes.value());
    REQUIRE_MESSAGE(image.ok(), path);
    return std::move(image.value());
}

} // namespace

TEST_CASE("MPQ set reads files honouring patch priority") {
    VD_REQUIRE_MPQ(mpqPtr);
    const vd::formats::MpqArchiveSet& mpq = *mpqPtr;

    const std::vector<std::string> archives = mpq.archiveNames();
    REQUIRE(archives.size() >= 2);
    CHECK(archives[0] == "patch-2.MPQ");
    CHECK(archives[1] == "patch.MPQ");

    CHECK(mpq.contains("DBFilesClient/Map.dbc")); // '/' accepted
    CHECK_FALSE(mpq.contains("Does\\Not\\Exist.blp"));
    CHECK_FALSE(mpq.read("Does\\Not\\Exist.blp").ok());

    // patch.MPQ holds a 0-byte deletion marker for this tile; the real data is in texture.MPQ.
    vd::Result<std::vector<vd::u8>> tile = mpq.read("textures\\Minimap\\d787029b3fc7aff8dd4a6015d86752ce.blp");
    REQUIRE(tile.ok());
    CHECK(tile.value().size() > 0);
}

TEST_CASE("BLP: DXT1 icon with 1-bit alpha") {
    VD_REQUIRE_MPQ(mpqPtr);
    const vd::formats::MpqArchiveSet& mpq = *mpqPtr;
    const vd::formats::BlpImage icon = loadBlp(mpq, "Interface\\Icons\\INV_Misc_QuestionMark.blp");
    CHECK(icon.width == 64);
    CHECK(icon.height == 64);
    CHECK(icon.encoding == vd::formats::BlpEncoding::Dxt1);
    CHECK(icon.alphaDepth == 1);
    CHECK(icon.mips.size() == 7); // 64 -> 1
    CHECK(icon.mips[6].size() == 4);
}

TEST_CASE("BLP: palette texture") {
    VD_REQUIRE_MPQ(mpqPtr);
    const vd::formats::MpqArchiveSet& mpq = *mpqPtr;
    const vd::formats::BlpImage face = loadBlp(mpq, "Character\\Human\\Male\\HumanMaleFaceUpper05_01.blp");
    CHECK(face.encoding == vd::formats::BlpEncoding::Palette);
    CHECK(face.width == 128);
    CHECK(face.height == 32);
    CHECK(face.mips[0].size() == 128u * 32u * 4u);
}

TEST_CASE("BLP: truncated tiny DXT mips are regenerated down to 1x1") {
    VD_REQUIRE_MPQ(mpqPtr);
    const vd::formats::MpqArchiveSet& mpq = *mpqPtr;
    // 32x256: Blizzard stored one block for the 2x16 and 1x8 mips.
    const vd::formats::BlpImage tall = loadBlp(mpq, "World\\Azeroth\\Elwynn\\PassiveDoodads\\CornCrop\\corncrop2.blp");
    CHECK(tall.width == 32);
    CHECK(tall.height == 256);
    REQUIRE(tall.mips.size() == 9); // 256 -> 1
    CHECK(tall.mips[4].size() == 2u * 16u * 4u);
    CHECK(tall.mips[8].size() == 4u);
}
