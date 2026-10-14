#include "pack/PackDigest.h"

#include "core/Sha256.h"
#include "pack/PackReader.h"

#include <array>
#include <cstring>
#include <vector>

namespace vd::pack {

Result<std::string> packDigest(const std::filesystem::path& packFile) {
    PackReader reader;
    if (Result<void> opened = reader.open(packFile); !opened) {
        return opened.error();
    }
    Sha256 sha;
    constexpr char kTag[] = "VDPK-recipe-1";
    sha.update(kTag, sizeof(kTag) - 1);
    const PackHeader& header = reader.header();
    sha.update(&header.version, sizeof(header.version));
    sha.update(&header.clientBuild, sizeof(header.clientBuild));
    std::vector<u8> raw;
    for (u32 i = 0; i < reader.entryCount(); ++i) {
        const PackAsset asset = reader.entry(i);
        const u64 pathHash = reader.entryHash(i);
        const u16 type = static_cast<u16>(asset.type);
        sha.update(&pathHash, sizeof(pathHash));
        sha.update(&type, sizeof(type));
        sha.update(&asset.rawSize, sizeof(asset.rawSize));
        if (asset.compression == Compression::None) {
            sha.update(asset.data, asset.rawSize);
            continue;
        }
        raw.resize(asset.rawSize);
        if (Result<void> read = PackReader::read(asset, raw.data(), raw.size()); !read) {
            return Error{packFile.string() + ": " + read.error().message};
        }
        sha.update(raw.data(), raw.size());
    }
    const std::array<u8, 32> digest = sha.finish();
    return Sha256::hex(std::span(digest.data(), 16));
}

} // namespace vd::pack
