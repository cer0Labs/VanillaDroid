#include "BakerCommon.h"

#include "bake/TextureBaker.h"
#include "core/Hash.h"
#include "core/Log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace vd::baker {

std::string option(const Options& options, const char* name, const std::string& fallback) {
    const auto it = options.find(name);
    return it != options.end() ? it->second : fallback;
}

std::filesystem::path wowDir(const Options& options) {
    if (auto it = options.find("wow-dir"); it != options.end()) {
        return it->second;
    }
    if (const char* env = std::getenv("VD_WOW_DIR")) {
        return env;
    }
    return VD_DEFAULT_WOW_DIR;
}

std::vector<std::string> skipArchives(const Options& options, const char* name) {
    const std::string list = option(options, name);
    std::vector<std::string> names;
    usize begin = 0;
    while (begin <= list.size()) {
        const usize end = std::min(list.find_first_of(";|", begin), list.size());
        if (end > begin) {
            names.push_back(list.substr(begin, end - begin));
        }
        begin = end + 1;
    }
    return names;
}

bool openArchives(const Options& options, formats::MpqArchiveSet& mpq) {
    const std::filesystem::path dataDir = formats::findDataDir(wowDir(options)); // phase 22: whatever its case
    if (Result<void> opened = mpq.open(dataDir, skipArchives(options, "skip-archives")); !opened) {
        VD_LOG_ERROR("%s", opened.error().message.c_str());
        return false;
    }
    return true;
}

u64 textureCacheKey(const std::vector<u8>& blpBytes, pack::TextureFormat format) {
    const u32 settings[] = {static_cast<u32>(format), bake::kTextureEncoderVersion};
    return fnv1a64(settings, sizeof(settings), fnv1a64(blpBytes.data(), blpBytes.size()));
}

bool matchesFilter(const std::string& canonicalPath, const std::string& filter) {
    return filter.empty() || canonicalPath.compare(0, filter.size(), filter) == 0;
}

BakeCache::BakeCache(std::filesystem::path dir) : m_dir(bakeCacheDir(dir)) {
    if (m_dir.empty()) {
        return; // phase 22: --cache off
    }
    std::error_code error;
    std::filesystem::create_directories(m_dir, error);
}

std::filesystem::path BakeCache::pathFor(u64 key) const {
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.bin", static_cast<unsigned long long>(key));
    return m_dir / name;
}

bool BakeCache::load(u64 key, std::vector<u8>& out) const {
    if (m_dir.empty()) {
        return false;
    }
    std::FILE* file = std::fopen(pathFor(key).string().c_str(), "rb");
    if (!file) {
        return false;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    out.resize(size > 0 ? static_cast<std::size_t>(size) : 0);
    const bool ok = size > 0 && std::fread(out.data(), 1, out.size(), file) == out.size();
    std::fclose(file);
    return ok;
}

void BakeCache::store(u64 key, const std::vector<u8>& blob) const {
    if (m_dir.empty()) {
        return;
    }
    // Write to a temp name then rename: an interrupted bake never leaves a truncated entry.
    const std::filesystem::path final = pathFor(key);
    std::filesystem::path temp = final;
    temp += ".tmp";
    std::FILE* file = std::fopen(temp.string().c_str(), "wb");
    if (!file) {
        return;
    }
    const bool ok = std::fwrite(blob.data(), 1, blob.size(), file) == blob.size();
    std::fclose(file);
    std::error_code error;
    if (ok) {
        std::filesystem::rename(temp, final, error);
    } else {
        std::filesystem::remove(temp, error);
    }
}

} // namespace vd::baker
