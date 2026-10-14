// Phase 21 (languages): clients in other languages. The packs are baked from one client (VD_WOW_DIR, normally enUS);
// a client of the same build in another language (C:\World of Warcraft 1.12 esES) adds language overlays next to
// them, holding only what that language changes under the same paths: db.<locale>.vdpk (its DBC texts, moved to the
// first column of each text), textures.<locale>.vdpk (the world map tiles with painted names) and sound.<locale>.vdpk
// (the creature voices recorded again). Its interface texts (GlobalStrings.lua, GlueStrings.lua) go into ui.vdpk
// with the others (UiCommands.cpp). The runtime opens the overlays of the configured locale (pack::PackReader).
#include "BakerCommon.h"

#include "bake/LocaleBaker.h"
#include "bake/PackWriter.h"
#include "bake/SoundBaker.h"
#include "bake/TextureBaker.h"
#include "core/Clock.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "formats/Blp.h"
#include "jobs/JobSystem.h"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <set>

namespace vd::baker {
namespace {

bool endsWith(const std::string& canonical, const char* suffix) {
    const usize n = std::char_traits<char>::length(suffix);
    return canonical.size() >= n && canonical.compare(canonical.size() - n, n, suffix) == 0;
}

// --locale-clients, then $VD_WOW_LOCALE_DIRS, then CMake's VD_WOW_LOCALE_DIRS; ';'- or '|'-separated.
std::vector<std::filesystem::path> localeClientDirs(const Options& options) {
    std::string list = option(options, "locale-clients");
    if (list.empty()) {
        const char* env = std::getenv("VD_WOW_LOCALE_DIRS");
        list = env ? env : VD_DEFAULT_WOW_LOCALE_DIRS;
    }
    std::vector<std::filesystem::path> dirs;
    usize begin = 0;
    while (begin <= list.size()) {
        const usize end = std::min(list.find_first_of(";|", begin), list.size());
        if (end > begin) {
            dirs.emplace_back(list.substr(begin, end - begin));
        }
        begin = end + 1;
    }
    return dirs;
}

// A field equal in both tables: the same value, or two offsets of the same string (each table lays its string block
// out its own way).
bool sameField(const db::DbcView& a, const db::DbcView& b, u32 record, u32 field) {
    const u32 va = a.getU32(record, field);
    const u32 vb = b.getU32(record, field);
    if (va == vb) {
        return true;
    }
    return va < a.stringBlockSize() && vb < b.stringBlockSize() && a.looksLikeString(record, field) &&
           b.looksLikeString(record, field) && a.getString(record, field) == b.getString(record, field);
}

// db.<locale>.vdpk: the tables that differ from the base client's, with their texts in the first column.
int bakeLocaleDb(const formats::MpqArchiveSet& base, const LocaleClient& client, const std::filesystem::path& outPath) {
    const Clock::time_point start = Clock::now();
    const u32 column = static_cast<u32>(bake::localeColumn(client.locale));
    bake::PackWriter writer;
    if (Result<void> created = writer.create(outPath, kBakerVersion); !created) {
        VD_LOG_ERROR("%s", created.error().message.c_str());
        return 1;
    }
    writer.setLocale(client.locale);
    u32 identical = 0, texts = 0, mismatches = 0, failures = 0;
    for (const std::string& file : client.mpq->list("DBFilesClient\\*.dbc")) {
        Result<std::vector<u8>> bytes = client.mpq->read(file);
        if (!bytes) {
            VD_LOG_ERROR("%s: %s", file.c_str(), bytes.error().message.c_str());
            ++failures;
            continue;
        }
        Result<std::vector<u8>> baseBytes = base.read(file);
        if (baseBytes && baseBytes.value() == bytes.value()) {
            ++identical;
            continue;
        }
        u32 moved = 0;
        Result<std::vector<u8>> localized = bake::localizeDbc(bytes.value(), column, &moved);
        if (!localized) {
            VD_LOG_ERROR("%s: %s", file.c_str(), localized.error().message.c_str());
            ++failures;
            continue;
        }
        if (moved == 0) {
            // Different without texts in this language (SpellAuraNames, SpellEffectNames: the developers' tables
            // have rows the base lacks): the base client's table stays.
            VD_LOG_INFO("%s: differs, no texts in %s (the base table stays)", file.c_str(), client.locale.c_str());
            continue;
        }
        // Everything but the texts must be the base client's: the overlay replaces whole tables.
        Result<db::DbcView> mine = db::DbcView::parse(localized.value().data(), localized.value().size());
        Result<db::DbcView> theirs = baseBytes ? db::DbcView::parse(baseBytes.value().data(), baseBytes.value().size())
                                               : Result<db::DbcView>(Error{"not in the base client"});
        if (!theirs || theirs.value().recordCount() != mine.value().recordCount() ||
            theirs.value().fieldCount() != mine.value().fieldCount()) {
            VD_LOG_WARN("%s: the table is not the base client's (%s)", file.c_str(),
                        theirs ? "other rows or fields" : theirs.error().message.c_str());
            ++mismatches;
        } else {
            std::set<u32> textFields;
            for (const u32 first : bake::findLocaleTexts(theirs.value(), 0)) {
                for (u32 k = 0; k < bake::kLocaleColumns; ++k) {
                    textFields.insert(first + k);
                }
            }
            u32 different = 0;
            for (u32 r = 0; r < mine.value().recordCount(); ++r) {
                for (u32 f = 0; f < mine.value().fieldCount(); ++f) {
                    different += !textFields.count(f) && !sameField(theirs.value(), mine.value(), r, f);
                }
            }
            if (different > 0) {
                VD_LOG_WARN("%s: %u fields besides the texts differ from the base client's", file.c_str(), different);
                ++mismatches;
            }
        }
        if (Result<void> added = writer.add(file, pack::AssetType::Dbc, localized.value()); !added) {
            VD_LOG_ERROR("%s", added.error().message.c_str());
            ++failures;
            continue;
        }
        texts += moved;
    }
    if (Result<void> finished = writer.finish(); !finished) {
        VD_LOG_ERROR("%s", finished.error().message.c_str());
        return 1;
    }
    VD_LOG_INFO("db %s: %u tables (%u texts), %u as the base client's, %u with other differences, %u failed, %.1f MB, "
                "%.1f s -> %s",
                client.locale.c_str(), writer.entryCount(), texts, identical, mismatches, failures,
                static_cast<f64>(writer.bytesWritten()) / 1e6, secondsBetween(start, Clock::now()),
                outPath.string().c_str());
    return failures == 0 ? 0 : 1;
}

// textures.<locale>.vdpk: the textures whose pixels differ (the world map tiles with painted names; most other
// differing files are the same picture compressed again).
int bakeLocaleTextures(const formats::MpqArchiveSet& base, const LocaleClient& client,
                       const std::filesystem::path& outPath) {
    const Clock::time_point start = Clock::now();
    const std::vector<std::string> files = base.list("*.blp");
    bake::PackWriter writer;
    if (Result<void> created = writer.create(outPath, kBakerVersion); !created) {
        VD_LOG_ERROR("%s", created.error().message.c_str());
        return 1;
    }
    writer.setLocale(client.locale);
    const BakeCache cache(outPath.parent_path() / ".cache" / "textures"); // the same keys as textures.vdpk's
    std::atomic<u32> identical{0}, samePixels{0}, cacheHits{0}, done{0};
    std::mutex failuresMutex;
    std::vector<std::string> failures;
    const u32 total = static_cast<u32>(files.size());
    const u32 progressStep = std::max(1u, total / 10);
    JobSystem jobs;
    jobs.init();
    jobs.parallelFor(total, 16, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            const std::string& file = files[i];
            const Result<void> added = [&]() -> Result<void> {
                if (!client.mpq->contains(file)) {
                    return {};
                }
                Result<std::vector<u8>> mine = client.mpq->read(file);
                Result<std::vector<u8>> theirs = base.read(file);
                if (!mine || !theirs) {
                    return mine ? theirs.error() : mine.error();
                }
                if (mine.value() == theirs.value()) {
                    ++identical;
                    return {};
                }
                Result<formats::BlpImage> image = formats::decodeBlp(mine.value());
                Result<formats::BlpImage> baseImage = formats::decodeBlp(theirs.value());
                if (!image) {
                    return image.error();
                }
                if (baseImage && baseImage.value().width == image.value().width &&
                    baseImage.value().height == image.value().height && !image.value().mips.empty() &&
                    !baseImage.value().mips.empty() && baseImage.value().mips[0] == image.value().mips[0]) {
                    ++samePixels;
                    return {};
                }
                const std::string canonical = normalizeAssetPath(file);
                const pack::TextureFormat format = bake::chooseTextureFormat(canonical);
                const u64 key = textureCacheKey(mine.value(), format);
                std::vector<u8> blob;
                if (cache.load(key, blob)) {
                    ++cacheHits;
                } else {
                    Result<std::vector<u8>> encoded = bake::bakeTexture(image.value(), format);
                    if (!encoded) {
                        return encoded.error();
                    }
                    blob = std::move(encoded.value());
                    cache.store(key, blob);
                }
                return writer.add(canonical, pack::AssetType::Texture, blob);
            }();
            if (!added) {
                std::lock_guard<std::mutex> lock(failuresMutex);
                failures.push_back(file + ": " + added.error().message);
            }
            const u32 count = ++done;
            if (count % progressStep == 0) {
                VD_LOG_INFO("textures %s: %u / %u (%.0f s)", client.locale.c_str(), count, total,
                            secondsBetween(start, Clock::now()));
            }
        }
    });
    jobs.shutdown();
    if (Result<void> finished = writer.finish(); !finished) {
        VD_LOG_ERROR("%s", finished.error().message.c_str());
        return 1;
    }
    for (const std::string& failure : failures) {
        VD_LOG_ERROR("FAILED %s", failure.c_str());
    }
    VD_LOG_INFO("textures %s: %u with other pixels (%u from cache), %u identical files, %u the same picture compressed "
                "again, %zu failed, %.1f MB, %.1f s -> %s",
                client.locale.c_str(), writer.entryCount(), cacheHits.load(), identical.load(), samePixels.load(),
                failures.size(), static_cast<f64>(writer.bytesWritten()) / 1e6, secondsBetween(start, Clock::now()),
                outPath.string().c_str());
    return failures.empty() ? 0 : 1;
}

// sound.<locale>.vdpk: the sounds SoundEntries names that were recorded again (another length: the creature voices,
// the race narrations). The files of the same size are the same recording with the MPQ's lossy ADPCM applied again.
// The player voices the esES client also carries under other folders are named by no table: the 1.12 never plays
// them, nor does this.
int bakeLocaleSound(const formats::MpqArchiveSet& base, const LocaleClient& client,
                    const std::filesystem::path& outPath) {
    const Clock::time_point start = Clock::now();
    u32 references = 0;
    const std::vector<std::string> paths = soundPaths(base, references);
    if (paths.empty()) {
        return 1;
    }
    bake::PackWriter writer;
    if (Result<void> created = writer.create(outPath, kBakerVersion); !created) {
        VD_LOG_ERROR("%s", created.error().message.c_str());
        return 1;
    }
    writer.setLocale(client.locale);
    const BakeCache cache(outPath.parent_path() / ".cache" / "sound");
    std::atomic<u32> identical{0}, sameSize{0}, cacheHits{0};
    std::atomic<u64> outBytes{0};
    std::mutex failuresMutex;
    std::vector<std::string> failures;
    JobSystem jobs;
    jobs.init();
    jobs.parallelFor(static_cast<u32>(paths.size()), 16, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            const std::string& path = paths[i];
            const std::string canonical = normalizeAssetPath(path);
            const bool mp3 = endsWith(canonical, ".mp3");
            if ((!mp3 && !endsWith(canonical, ".wav")) || !client.mpq->contains(path) || !base.contains(path)) {
                continue;
            }
            const Result<void> added = [&]() -> Result<void> {
                Result<std::vector<u8>> mine = client.mpq->read(path);
                Result<std::vector<u8>> theirs = base.read(path);
                if (!mine || !theirs) {
                    return mine ? theirs.error() : mine.error();
                }
                if (mine.value() == theirs.value()) {
                    ++identical;
                    return {};
                }
                if (mine.value().size() == theirs.value().size()) {
                    ++sameSize;
                    return {};
                }
                const u64 key = soundCacheKey(mine.value(), mp3);
                std::vector<u8> blob;
                if (cache.load(key, blob)) {
                    ++cacheHits;
                } else {
                    Result<std::vector<u8>> baked =
                        mp3 ? bake::bakeMp3Sound(mine.value()) : bake::bakeWavSound(mine.value());
                    if (!baked) {
                        return baked.error();
                    }
                    blob = std::move(baked.value());
                    cache.store(key, blob);
                }
                outBytes += blob.size();
                return writer.add(canonical, pack::AssetType::Sound, blob);
            }();
            if (!added) {
                std::lock_guard<std::mutex> lock(failuresMutex);
                failures.push_back(path + ": " + added.error().message);
            }
        }
    });
    jobs.shutdown();
    if (Result<void> finished = writer.finish(); !finished) {
        VD_LOG_ERROR("%s", finished.error().message.c_str());
        return 1;
    }
    for (const std::string& failure : failures) {
        VD_LOG_ERROR("FAILED %s", failure.c_str());
    }
    VD_LOG_INFO("sound %s: %u recorded again (%u from cache, %.1f MB), %u identical, %u the same recording compressed "
                "again, %zu failed, %.1f s -> %s",
                client.locale.c_str(), writer.entryCount(), cacheHits.load(), static_cast<f64>(outBytes.load()) / 1e6,
                identical.load(), sameSize.load(), failures.size(), secondsBetween(start, Clock::now()),
                outPath.string().c_str());
    return failures.empty() ? 0 : 1;
}

} // namespace

std::string detectClientLocale(const formats::MpqArchiveSet& mpq) {
    return bake::detectClientLocale(mpq); // phase 22: in vd_formats, for VanillaDroid Setup too
}

std::vector<LocaleClient> openLocaleClients(const Options& options, const std::string& baseLocale) {
    std::vector<LocaleClient> clients;
    for (const std::filesystem::path& dir : localeClientDirs(options)) {
        std::error_code error;
        const std::filesystem::path dataDir = formats::findDataDir(dir); // phase 22: whatever its case
        if (!std::filesystem::is_directory(dataDir, error)) {
            VD_LOG_INFO("Client in another language not found (skipped): %s", dir.string().c_str());
            continue;
        }
        LocaleClient client;
        client.dir = dir;
        client.mpq = std::make_unique<formats::MpqArchiveSet>();
        if (Result<void> opened = client.mpq->open(dataDir, skipArchives(options, "locale-skip-archives"));
            !opened) {
            VD_LOG_ERROR("%s", opened.error().message.c_str());
            continue;
        }
        client.locale = detectClientLocale(*client.mpq);
        if (client.locale.empty() || client.locale == baseLocale) {
            VD_LOG_ERROR("%s: its texts are in %s, the base client's language (skipped)", dir.string().c_str(),
                         client.locale.empty() ? "no language found" : client.locale.c_str());
            continue;
        }
        bool repeated = false;
        for (const LocaleClient& other : clients) {
            repeated |= other.locale == client.locale;
        }
        if (repeated) {
            VD_LOG_ERROR("%s: a second %s client (skipped)", dir.string().c_str(), client.locale.c_str());
            continue;
        }
        VD_LOG_INFO("Client in %s: %s", client.locale.c_str(), dir.string().c_str());
        clients.push_back(std::move(client));
    }
    return clients;
}

int bakeLocale(const formats::MpqArchiveSet& base, const LocaleClient& client, const std::filesystem::path& outDir) {
    int result = 0;
    result |= bakeLocaleDb(base, client, outDir / ("db." + client.locale + ".vdpk"));
    result |= bakeLocaleTextures(base, client, outDir / ("textures." + client.locale + ".vdpk"));
    result |= bakeLocaleSound(base, client, outDir / ("sound." + client.locale + ".vdpk"));
    return result;
}

} // namespace vd::baker
