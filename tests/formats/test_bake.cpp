#include "bake/PackWriter.h"
#include "bake/TextureBaker.h"
#include "core/Hash.h"
#include "formats/Blp.h"
#include "pack/PackReader.h"

#include "SharedMpq.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>

namespace {

double psnr(const std::vector<vd::u8>& a, const std::vector<vd::u8>& b, bool withAlpha) {
    double squaredError = 0.0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!withAlpha && i % 4 == 3) {
            continue;
        }
        const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        squaredError += d * d;
        ++count;
    }
    const double mse = squaredError / static_cast<double>(count);
    return mse == 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

std::filesystem::path tempPack(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

} // namespace

TEST_CASE("Asset path hashing ignores case and separators") {
    CHECK(vd::assetPathHash("Interface\\Icons\\X.blp") == vd::assetPathHash("interface/icons/x.blp"));
    CHECK(vd::assetPathHash("a/b") != vd::assetPathHash("a/c"));
    CHECK(vd::normalizeAssetPath("DBFilesClient\\Map.dbc") == "dbfilesclient/map.dbc");
}

TEST_CASE("PackWriter and PackReader round-trip") {
    const std::filesystem::path path = tempPack("vd_test_roundtrip.vdpk");
    {
        vd::bake::PackWriter writer;
        REQUIRE(writer.create(path, 7).ok());
        const std::vector<vd::u8> a{1, 2, 3};
        const std::vector<vd::u8> b(1000, 0xAB);
        REQUIRE(writer.add("Dir\\First.dbc", vd::pack::AssetType::Dbc, a).ok());
        REQUIRE(writer.add("dir/second.blp", vd::pack::AssetType::Texture, b).ok());
        CHECK_FALSE(writer.add("DIR/FIRST.DBC", vd::pack::AssetType::Dbc, a).ok()); // duplicate
        REQUIRE(writer.finish().ok());
    }
    vd::pack::PackReader reader;
    REQUIRE(reader.open(path).ok());
    CHECK(reader.entryCount() == 2);
    CHECK(reader.header().bakerVersion == 7);

    const auto first = reader.find("dir/first.dbc");
    REQUIRE(first.has_value());
    CHECK(first->size == 3);
    CHECK(first->data[2] == 3);
    CHECK(first->name == "dir/first.dbc");
    CHECK(reinterpret_cast<std::uintptr_t>(first->data) % vd::pack::kBlobAlignment == 0);

    const auto second = reader.find("DIR\\SECOND.BLP");
    REQUIRE(second.has_value());
    CHECK(second->type == vd::pack::AssetType::Texture);
    CHECK(second->size == 1000);
    CHECK_FALSE(reader.find("dir/missing.dbc").has_value());

    reader.close();
    std::filesystem::remove(path);
}

TEST_CASE("PackReader rejects files that are not packs") {
    const std::filesystem::path path = tempPack("vd_test_notapack.vdpk");
    {
        std::FILE* file = std::fopen(path.string().c_str(), "wb");
        REQUIRE(file != nullptr);
        const char junk[100] = "definitely not a pack";
        std::fwrite(junk, 1, sizeof(junk), file);
        std::fclose(file);
    }
    vd::pack::PackReader reader;
    CHECK_FALSE(reader.open(path).ok());
    std::filesystem::remove(path);
}

TEST_CASE("ASTC baking keeps textures faithful") {
    VD_REQUIRE_MPQ(mpq);
    struct Case {
        const char* path;
        vd::pack::TextureFormat format;
        double minPsnr;
    };
    const Case cases[] = {
        {"Interface\\Icons\\INV_Misc_QuestionMark.blp", vd::pack::TextureFormat::Astc4x4, 34.0},
        {"Tileset\\Elwynn\\ElwynnRockBase.blp", vd::pack::TextureFormat::Astc6x6, 28.0},
        {"Character\\Human\\Male\\HumanMaleFaceUpper05_01.blp", vd::pack::TextureFormat::Astc4x4, 38.0},
    };
    for (const Case& c : cases) {
        CAPTURE(c.path);
        vd::Result<std::vector<vd::u8>> bytes = mpq->read(c.path);
        REQUIRE(bytes.ok());
        vd::Result<vd::formats::BlpImage> image = vd::formats::decodeBlp(bytes.value());
        REQUIRE(image.ok());
        vd::Result<std::vector<vd::u8>> blob = vd::bake::bakeTexture(image.value(), c.format);
        REQUIRE(blob.ok());

        vd::pack::TextureHeader header;
        std::memcpy(&header, blob.value().data(), sizeof(header));
        CHECK(header.width == image.value().width);
        CHECK(header.mipCount == image.value().mips.size());

        vd::Result<std::vector<vd::u8>> decoded = vd::bake::decodeTextureBlob(blob.value().data(), blob.value().size(), 0);
        REQUIRE(decoded.ok());
        const bool alpha = (header.flags & vd::pack::kTextureHasAlpha) != 0;
        const double quality = psnr(image.value().mips[0], decoded.value(), alpha);
        MESSAGE(std::string(c.path) << ": PSNR " << quality << " dB");
        CHECK(quality >= c.minPsnr);
    }
}
