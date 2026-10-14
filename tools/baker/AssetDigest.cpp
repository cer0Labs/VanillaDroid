// Phase 22 (phonebaker): the digest of every asset of one pack, to find which assets two bakes of the same
// pack disagree on (a phone's and the PC's) without copying the pack: diff the two listings.
#include "BakerCommon.h"

#include "core/Log.h"
#include "core/Sha256.h"
#include "pack/PackReader.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace vd::baker {

int commandDigestAssets(const Options& options) {
    const std::string path = option(options, "pack");
    if (path.empty()) {
        VD_LOG_ERROR("digest-assets needs --pack <file.vdpk>");
        return 1;
    }
    pack::PackReader reader;
    if (Result<void> opened = reader.open(path); !opened) {
        VD_LOG_ERROR("%s", opened.error().message.c_str());
        return 1;
    }
    // One line per asset, by name (the path hash where the pack has no names): the first 16 bytes of the
    // SHA-256 of its raw bytes, its type, its raw size and its name, last (names may hold spaces).
    std::vector<std::pair<std::string, std::string>> lines; // name, the rest
    lines.reserve(reader.entryCount());
    std::vector<u8> raw;
    Sha256 sha;
    for (u32 i = 0; i < reader.entryCount(); ++i) {
        const pack::PackAsset asset = reader.entry(i);
        raw.resize(asset.rawSize);
        if (Result<void> read = pack::PackReader::read(asset, raw.data(), raw.size()); !read) {
            VD_LOG_ERROR("%s", read.error().message.c_str());
            return 1;
        }
        sha.update(raw.data(), raw.size());
        const std::array<u8, 32> digest = sha.finish();
        char hash[32];
        std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(reader.entryHash(i)));
        lines.emplace_back(asset.name.empty() ? std::string(hash) : std::string(asset.name),
                           Sha256::hex(std::span(digest.data(), 16)) + " " +
                               std::to_string(static_cast<u32>(asset.type)) + " " +
                               std::to_string(asset.rawSize));
    }
    std::sort(lines.begin(), lines.end());
    for (const auto& [name, rest] : lines) {
        std::printf("%s %s\n", rest.c_str(), name.c_str()); // "<digest> <type> <raw size> <name>"
    }
    VD_LOG_INFO("digest-assets: %u assets in %s", reader.entryCount(), path.c_str());
    return 0;
}

} // namespace vd::baker
