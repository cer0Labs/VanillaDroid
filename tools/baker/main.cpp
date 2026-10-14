#include "BakerCommon.h"

#include "bake/LocaleBaker.h"
#include "bake/PackWriter.h"
#include "bake/TextureBaker.h"
#include "core/Clock.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "formats/Blp.h"
#include "formats/Dbc.h"
#include "formats/MpqArchiveSet.h"
#include "formats/PngWriter.h"
#include "jobs/JobSystem.h"
#include "pack/PackReader.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>

namespace {

constexpr const char* kUsage = R"(vd_baker - converts the WoW 1.12.1 client data into mobile-ready packs

Usage:
  vd_baker bake          [--wow-dir DIR] --out <dir> [--only all|db|textures|models|actors|world|collision|objects|ui|sound|cinematics|grass|worldsound|bodyfx|locale|app]
                         [--locale-clients "DIR;DIR"]   clients of the same build in other languages (default
                                              $VD_WOW_LOCALE_DIRS, then CMake's VD_WOW_LOCALE_DIRS): their interface
                                              texts go into ui.vdpk; locale: their overlays db.<locale>.vdpk,
                                              textures.<locale>.vdpk, sound.<locale>.vdpk (what the language changes)
                         app (phase 22): app.vdpk, ours, without the WoW client (no MPQ is opened):
                         [--locale-dir DIR]   texts of our own (locale/<locale>/*.lang)
                         [--art-dir DIR]      art of our own (art/: the touch cluster's SVG icons, art/fonts)
                         [--filter interface/icons/] [--maps azeroth,kalimdor]   (world, collision: maps, default all)
                         [--skip-archives "patch-3.MPQ;patch-A.MPQ"] [--locale-skip-archives "..."]   phase 22:
                                              archives left out of the base client / of each --locale-clients client
  vd_baker list         [--wow-dir DIR] [--mask *.blp] [--archive patch.MPQ]
  vd_baker scan-locale   [--wow-dir DIR] [--locale-clients "DIR;DIR"] [--diff 1]   the language of each client (its
                         DBC texts); --diff: every file of each against the base client, by extension, and the
                         interface's Lua / XML that differ
  vd_baker dump-texture  [--wow-dir DIR] --file <path in MPQ> --out <file.png> [--mip N]
  vd_baker scan-textures [--wow-dir DIR]      decode every BLP, report encodings and failures
  vd_baker scan-terrain  [--wow-dir DIR]      parse every WDT/ADT, report terrain statistics and failures
  vd_baker scan-maps     [--wow-dir DIR]      every map: Map.dbc id, type and name, ADT tiles, global WMO, WDL
  vd_baker scan-models   [--wow-dir DIR]      parse every WMO (root + groups), report statistics and failures
  vd_baker scan-glue     [--wow-dir DIR]      the glue screens' scenes (Interface\Glues\Models): cameras, lights
  vd_baker scan-actors   [--wow-dir DIR]      parse every M2 with its skeleton, report animation statistics
  vd_baker scan-events   [--wow-dir DIR] [--filter character/]   census of the M2 events (VD_SCAN_VERBOSE=1: each)
  vd_baker scan-sounds   [--wow-dir DIR]      census of the WAV / MP3 files (bake them: --only sound -> sound.vdpk)
  vd_baker probe-ground  [--wow-dir DIR] --map Azeroth --pos x,y   the footstep surface the sound bake sees there
  vd_baker scan-grass    [--wow-dir DIR] [--maps azeroth] [--density 16]   the ground effects of every map (phase 14):
                         chunks that grow grass, doodads per chunk with that frillDensity, triangles, shadows
  vd_baker scan-emitters [--wow-dir DIR]      the terrain's sound emitters (ADT MCSE; bake: --only worldsound)
  vd_baker scan-particles [--wow-dir DIR] [--filter world/generic/]   parse the particle emitters, ribbons and
                         texture transforms of every M2 (VD_SCAN_VERBOSE=1: every emitter of the filtered models)
  vd_baker scan-doodad-anim [--wow-dir DIR] [--filter world/] [--maps azeroth] [--top 60] [--positions lamppost]
                         [--model <m2 path>: its bones, batches and what animates them]
                         placed doodads that move or turn to the camera (bones, vertices, instances in the cities,
                         where the matching models stand; VD_SCAN_VERBOSE=1:
                         their sequences)
  vd_baker extract       [--wow-dir DIR] --file <path in MPQ> --out <file>   raw file copy
  vd_baker dump-baked    --pack <file.vdpk> --file <asset path> --out <file.png> [--mip N] [--layer N]
  vd_baker check-collision [--packs packs] --map Azeroth --points <file: x y z per line> [--show 15]
                         ground found by the runtime under known points (creature spawns) vs their height
  vd_baker probe-walk    [--packs packs] --map Azeroth --from x,y,z --to x,y [--seconds 20]
                         the character controller walks between two points; where it stops, what blocks it
  vd_baker digest        --packs <dir> [--out release/packs.json]   phase 22: the recipe of every client pack (a
                         digest of its content) for the release (stdout without --out)
  vd_baker digest-assets --pack <file.vdpk>   phase 22: "<digest> <type> <raw size> <name>" of every asset, by
                         name (diff two bakes of a pack)
  vd_baker install       --staging <dir> --packs <dir>   phase 22: the packs a bake wrote into a staging folder
                         (its manifest.json) into the packs folder, their recipes into packs/manifest.json

Every command: [--jobs N]   threads that work (default: every core but one; phase 22: fewer on a hot phone)
bake:          [--cache <dir>|off]   the bake caches (default <out>/.cache); off: none, everything baked again

--wow-dir defaults to $VD_WOW_DIR, then to the path configured in CMake (VD_WOW_DIR).)";

using vd::baker::BakeCache;
using vd::baker::kBakerVersion;
using vd::baker::matchesFilter;
using vd::baker::openArchives;
using vd::baker::textureCacheKey;
using vd::baker::Options;

bool parseOptions(int argc, char** argv, Options& options) {
    for (int i = 2; i < argc; i += 2) {
        if (std::strncmp(argv[i], "--", 2) != 0 || i + 1 >= argc) {
            VD_LOG_ERROR("Bad argument: %s", argv[i]);
            return false;
        }
        options[argv[i] + 2] = argv[i + 1];
    }
    return true;
}

int commandList(const Options& options) {
    vd::formats::MpqArchiveSet mpq;
    if (!openArchives(options, mpq)) {
        return 1;
    }
    const auto it = options.find("mask");
    const std::string mask = it != options.end() ? it->second : "*";
    const auto archive = options.find("archive");
    const std::vector<std::string> files =
        mpq.list(mask.c_str(), archive != options.end() ? archive->second : std::string{});
    for (const std::string& file : files) {
        std::printf("%s\n", file.c_str());
    }
    VD_LOG_INFO("%zu files match '%s'", files.size(), mask.c_str());
    return 0;
}

int commandDumpTexture(const Options& options) {
    const auto file = options.find("file");
    const auto out = options.find("out");
    if (file == options.end() || out == options.end()) {
        VD_LOG_ERROR("dump-texture needs --file and --out");
        return 1;
    }
    vd::formats::MpqArchiveSet mpq;
    if (!openArchives(options, mpq)) {
        return 1;
    }
    vd::Result<std::vector<vd::u8>> bytes = mpq.read(file->second);
    if (!bytes) {
        VD_LOG_ERROR("%s", bytes.error().message.c_str());
        return 1;
    }
    vd::Result<vd::formats::BlpImage> image = vd::formats::decodeBlp(bytes.value());
    if (!image) {
        VD_LOG_ERROR("%s: %s", file->second.c_str(), image.error().message.c_str());
        return 1;
    }
    const vd::formats::BlpImage& blp = image.value();
    const auto mipOption = options.find("mip");
    const std::size_t mip = mipOption != options.end() ? std::stoul(mipOption->second) : 0;
    if (mip >= blp.mips.size()) {
        VD_LOG_ERROR("Texture has %zu mips", blp.mips.size());
        return 1;
    }
    const vd::u32 w = std::max(1u, blp.width >> mip);
    const vd::u32 h = std::max(1u, blp.height >> mip);
    if (vd::Result<void> written = vd::formats::writePng(out->second, w, h, blp.mips[mip].data()); !written) {
        VD_LOG_ERROR("%s", written.error().message.c_str());
        return 1;
    }
    VD_LOG_INFO("%s: %ux%u, %zu mips, encoding %d, alpha depth %u -> %s (mip %zu)", file->second.c_str(),
                blp.width, blp.height, blp.mips.size(), static_cast<int>(blp.encoding), blp.alphaDepth,
                out->second.c_str(), mip);
    return 0;
}

int commandExtract(const Options& options) {
    const auto file = options.find("file");
    const auto out = options.find("out");
    if (file == options.end() || out == options.end()) {
        VD_LOG_ERROR("extract needs --file and --out");
        return 1;
    }
    vd::formats::MpqArchiveSet mpq;
    if (!openArchives(options, mpq)) {
        return 1;
    }
    vd::Result<std::vector<vd::u8>> bytes = mpq.read(file->second);
    if (!bytes) {
        VD_LOG_ERROR("%s", bytes.error().message.c_str());
        return 1;
    }
    std::FILE* output = std::fopen(out->second.c_str(), "wb");
    if (!output || std::fwrite(bytes.value().data(), 1, bytes.value().size(), output) != bytes.value().size()) {
        VD_LOG_ERROR("Cannot write %s", out->second.c_str());
        if (output) {
            std::fclose(output);
        }
        return 1;
    }
    std::fclose(output);
    VD_LOG_INFO("%s -> %s (%zu bytes)", file->second.c_str(), out->second.c_str(), bytes.value().size());
    return 0;
}

// Decodes every BLP in the client: robustness check for the decoder and a census of encodings.
int commandScanTextures(const Options& options) {
    vd::formats::MpqArchiveSet mpq;
    if (!openArchives(options, mpq)) {
        return 1;
    }
    const std::vector<std::string> files = mpq.list("*.blp");
    const vd::Clock::time_point start = vd::Clock::now();

    std::array<std::atomic<vd::u32>, 5> byEncoding{};
    std::array<std::atomic<vd::u32>, 9> byAlphaDepth{};
    std::atomic<vd::u64> decodedPixels{0};
    std::mutex failuresMutex;
    std::vector<std::string> failures;

    vd::JobSystem jobs;
    jobs.init();
    jobs.parallelFor(static_cast<vd::u32>(files.size()), 32, [&](vd::u32 begin, vd::u32 end) {
        for (vd::u32 i = begin; i < end; ++i) {
            vd::Result<std::vector<vd::u8>> bytes = mpq.read(files[i]);
            vd::Result<vd::formats::BlpImage> image =
                bytes ? vd::formats::decodeBlp(bytes.value()) : vd::Result<vd::formats::BlpImage>(bytes.error());
            if (!image) {
                std::lock_guard<std::mutex> lock(failuresMutex);
                failures.push_back(files[i] + ": " + image.error().message);
                continue;
            }
            const vd::formats::BlpImage& blp = image.value();
            ++byEncoding[static_cast<std::size_t>(blp.encoding)];
            ++byAlphaDepth[std::min<std::size_t>(blp.alphaDepth, 8)];
            decodedPixels += vd::u64{blp.width} * blp.height;
        }
    });
    jobs.shutdown();

    const double seconds = vd::secondsBetween(start, vd::Clock::now());
    const char* encodingNames[] = {"palette", "dxt1", "dxt3", "dxt5", "argb8888"};
    for (std::size_t i = 0; i < byEncoding.size(); ++i) {
        std::printf("%-10s %6u\n", encodingNames[i], byEncoding[i].load());
    }
    for (std::size_t depth : {0, 1, 4, 8}) {
        std::printf("alpha %zu    %6u\n", depth, byAlphaDepth[depth].load());
    }
    for (const std::string& failure : failures) {
        std::printf("FAILED %s\n", failure.c_str());
    }
    VD_LOG_INFO("%zu textures, %zu failed, %.1f Mpixels (mip 0) in %.1f s", files.size(), failures.size(),
                static_cast<double>(decodedPixels.load()) / 1e6, seconds);
    return failures.empty() ? 0 : 1;
}

// Reads a texture back from a baked pack (through the runtime reader) and writes a PNG.
int commandDumpBaked(const Options& options) {
    const auto packPath = options.find("pack");
    const auto file = options.find("file");
    const auto out = options.find("out");
    if (packPath == options.end() || file == options.end() || out == options.end()) {
        VD_LOG_ERROR("dump-baked needs --pack, --file and --out");
        return 1;
    }
    vd::pack::PackReader reader;
    if (vd::Result<void> opened = reader.open(packPath->second); !opened) {
        VD_LOG_ERROR("%s", opened.error().message.c_str());
        return 1;
    }
    const std::optional<vd::pack::PackAsset> asset = reader.find(file->second);
    if (!asset || asset->type != vd::pack::AssetType::Texture) {
        VD_LOG_ERROR("No texture %s in %s", file->second.c_str(), packPath->second.c_str());
        return 1;
    }
    const auto mipOption = options.find("mip");
    const vd::u32 mip = mipOption != options.end() ? static_cast<vd::u32>(std::stoul(mipOption->second)) : 0;
    const vd::u32 layer = static_cast<vd::u32>(std::stoul(vd::baker::option(options, "layer", "0")));
    std::vector<vd::u8> raw(asset->rawSize);
    if (vd::Result<void> read = vd::pack::PackReader::read(*asset, raw.data(), raw.size()); !read) {
        VD_LOG_ERROR("%s", read.error().message.c_str());
        return 1;
    }
    vd::Result<std::vector<vd::u8>> rgba = vd::bake::decodeTextureBlob(raw.data(), raw.size(), mip, layer);
    if (!rgba) {
        VD_LOG_ERROR("%s", rgba.error().message.c_str());
        return 1;
    }
    vd::pack::TextureHeader header;
    std::memcpy(&header, raw.data(), sizeof(header));
    const vd::u32 w = std::max(1u, vd::u32{header.width} >> mip);
    const vd::u32 h = std::max(1u, vd::u32{header.height} >> mip);
    if (vd::Result<void> written = vd::formats::writePng(out->second, w, h, rgba.value().data()); !written) {
        VD_LOG_ERROR("%s", written.error().message.c_str());
        return 1;
    }
    VD_LOG_INFO("%.*s: %ux%u, format %u, flags 0x%x, %u mips, %u bytes -> %s", static_cast<int>(asset->name.size()),
                asset->name.data(), header.width, header.height, header.format, header.flags, header.mipCount,
                asset->size, out->second.c_str());
    return 0;
}

// Phase 21: a client in another language has its texts moved to the first column of each (the one the runtime reads).
int bakeDbcs(const vd::formats::MpqArchiveSet& mpq, const std::filesystem::path& outPath, const std::string& locale) {
    const vd::Clock::time_point start = vd::Clock::now();
    vd::bake::PackWriter writer;
    if (vd::Result<void> created = writer.create(outPath, kBakerVersion); !created) {
        VD_LOG_ERROR("%s", created.error().message.c_str());
        return 1;
    }
    writer.setLocale(locale);
    const int column = std::max(0, vd::bake::localeColumn(locale));
    vd::u32 texts = 0;
    int failures = 0;
    for (const std::string& file : mpq.list("DBFilesClient\\*.dbc")) {
        vd::Result<std::vector<vd::u8>> bytes = mpq.read(file);
        if (!bytes || !vd::formats::DbcFile::parse(bytes.value()).ok()) {
            VD_LOG_ERROR("Invalid DBC: %s", file.c_str());
            ++failures;
            continue;
        }
        if (column != 0) {
            vd::u32 moved = 0;
            vd::Result<std::vector<vd::u8>> localized =
                vd::bake::localizeDbc(bytes.value(), static_cast<vd::u32>(column), &moved);
            if (localized) {
                bytes.value() = std::move(localized.value());
                texts += moved;
            }
        }
        if (vd::Result<void> added = writer.add(file, vd::pack::AssetType::Dbc, bytes.value()); !added) {
            VD_LOG_ERROR("%s", added.error().message.c_str());
            ++failures;
        }
    }
    if (vd::Result<void> finished = writer.finish(); !finished) {
        VD_LOG_ERROR("%s", finished.error().message.c_str());
        return 1;
    }
    VD_LOG_INFO("db: %u tables in %s (%u texts moved to the first column), %.1f MB, %d failed, %.1f s -> %s",
                writer.entryCount(), locale.c_str(), texts, static_cast<double>(writer.bytesWritten()) / 1e6, failures,
                vd::secondsBetween(start, vd::Clock::now()), outPath.string().c_str());
    return failures == 0 ? 0 : 1;
}

int bakeTextures(const vd::formats::MpqArchiveSet& mpq, const std::filesystem::path& outPath,
                 const std::string& filter) {
    const vd::Clock::time_point start = vd::Clock::now();
    std::vector<std::string> files;
    for (const std::string& file : mpq.list("*.blp")) {
        if (matchesFilter(vd::normalizeAssetPath(file), filter)) {
            files.push_back(file);
        }
    }
    vd::bake::PackWriter writer;
    if (vd::Result<void> created = writer.create(outPath, kBakerVersion); !created) {
        VD_LOG_ERROR("%s", created.error().message.c_str());
        return 1;
    }

    const BakeCache cache(outPath.parent_path() / ".cache" / "textures");
    std::atomic<vd::u32> done{0};
    std::atomic<vd::u32> cacheHits{0};
    std::atomic<vd::u64> inputBytes{0};
    std::array<std::atomic<vd::u32>, 6> byFormat{};
    std::mutex failuresMutex;
    std::vector<std::string> failures;
    const vd::u32 total = static_cast<vd::u32>(files.size());
    const vd::u32 progressStep = std::max(1u, total / 20);

    vd::JobSystem jobs;
    jobs.init();
    jobs.parallelFor(total, 4, [&](vd::u32 begin, vd::u32 end) {
        for (vd::u32 i = begin; i < end; ++i) {
            const std::string& file = files[i];
            const std::string canonical = vd::normalizeAssetPath(file);
            const vd::pack::TextureFormat format = vd::bake::chooseTextureFormat(canonical);
            const vd::Result<void> added = [&]() -> vd::Result<void> {
                vd::Result<std::vector<vd::u8>> bytes = mpq.read(file);
                if (!bytes) {
                    return bytes.error();
                }
                inputBytes += bytes.value().size();
                const vd::u64 key = textureCacheKey(bytes.value(), format);
                std::vector<vd::u8> blob;
                if (cache.load(key, blob)) {
                    ++cacheHits;
                } else {
                    vd::Result<vd::formats::BlpImage> image = vd::formats::decodeBlp(bytes.value());
                    if (!image) {
                        return image.error();
                    }
                    vd::Result<std::vector<vd::u8>> encoded = vd::bake::bakeTexture(image.value(), format);
                    if (!encoded) {
                        return encoded.error();
                    }
                    blob = std::move(encoded.value());
                    cache.store(key, blob);
                }
                return writer.add(canonical, vd::pack::AssetType::Texture, blob);
            }();
            if (!added) {
                std::lock_guard<std::mutex> lock(failuresMutex);
                failures.push_back(file + ": " + added.error().message);
            } else {
                ++byFormat[static_cast<std::size_t>(format)];
            }
            const vd::u32 count = ++done;
            if (count % progressStep == 0) {
                VD_LOG_INFO("textures: %u / %u (%.0f s)", count, total, vd::secondsBetween(start, vd::Clock::now()));
            }
        }
    });
    jobs.shutdown();

    if (vd::Result<void> finished = writer.finish(); !finished) {
        VD_LOG_ERROR("%s", finished.error().message.c_str());
        return 1;
    }
    for (const std::string& failure : failures) {
        VD_LOG_ERROR("FAILED %s", failure.c_str());
    }
    VD_LOG_INFO("textures: %u baked (astc4x4 %u, astc6x6 %u, %u from cache), %zu failed | %.1f MB BLP -> %.1f MB | "
                "%.1f s -> %s",
                writer.entryCount(), byFormat[1].load(), byFormat[3].load(), cacheHits.load(), failures.size(),
                static_cast<double>(inputBytes.load()) / 1e6, static_cast<double>(writer.bytesWritten()) / 1e6,
                vd::secondsBetween(start, vd::Clock::now()), outPath.string().c_str());
    return failures.empty() ? 0 : 1;
}

int commandBake(const Options& options) {
    const auto out = options.find("out");
    if (out == options.end()) {
        VD_LOG_ERROR("bake needs --out <directory>");
        return 1;
    }
    const auto onlyOption = options.find("only");
    const std::string only = onlyOption != options.end() ? onlyOption->second : "all";
    const auto filterOption = options.find("filter");
    const std::string filter = filterOption != options.end() ? vd::normalizeAssetPath(filterOption->second) : "";
    // Phase 22 (apppack): app.vdpk alone needs no WoW client (the CI bakes it on a machine without one).
    if (only == "app") {
        return vd::baker::bakeApp(std::filesystem::path(out->second) / "app.vdpk",
                                  vd::baker::option(options, "locale-dir", VD_DEFAULT_LOCALE_DIR),
                                  vd::baker::option(options, "art-dir", VD_DEFAULT_ART_DIR));
    }

    vd::formats::MpqArchiveSet mpq;
    if (!openArchives(options, mpq)) {
        return 1;
    }
    // Phase 21: the client's language (its DBC texts), and the clients in other languages for ui and locale.
    std::string baseLocale = vd::baker::detectClientLocale(mpq);
    if (baseLocale.empty()) {
        VD_LOG_WARN("The client's language was not found in its DBC texts: taken as enUS");
        baseLocale = "enUS";
    }
    VD_LOG_INFO("Client in %s: %s", baseLocale.c_str(), vd::baker::wowDir(options).string().c_str());
    std::vector<vd::baker::LocaleClient> localeClients;
    if (only == "all" || only == "ui" || only == "locale") {
        localeClients = vd::baker::openLocaleClients(options, baseLocale);
    }
    const std::filesystem::path outDir = out->second;
    const std::filesystem::file_time_type bakeStart = std::filesystem::file_time_type::clock::now(); // phase 22
    int result = 0;
    if (only == "all" || only == "db") {
        result |= bakeDbcs(mpq, outDir / "db.vdpk", baseLocale);
    }
    if (only == "all" || only == "textures") {
        result |= bakeTextures(mpq, outDir / "textures.vdpk", filter);
    }
    if (only == "all" || only == "models") {
        result |= vd::baker::bakeModels(mpq, outDir / "models.vdpk");
    }
    if (only == "all" || only == "actors") {
        result |= vd::baker::bakeActors(mpq, outDir / "actors.vdpk");
    }
    if (only == "all" || only == "world") {
        result |= vd::baker::bakeWorld(mpq, outDir, vd::baker::option(options, "maps"));
    }
    if (only == "all" || only == "collision") {
        result |= vd::baker::bakeCollision(mpq, outDir, vd::baker::option(options, "maps"));
    }
    if (only == "all" || only == "objects") { // phase 8: transports' collision
        result |= vd::baker::bakeObjectCollision(mpq, outDir);
    }
    if (only == "all" || only == "ui") {
        result |= vd::baker::bakeUi(mpq, outDir / "ui.vdpk", baseLocale, localeClients); // phase 21; 22: ours apart
    }
    if (only == "all" || only == "sound") {
        result |= vd::baker::bakeSound(mpq, outDir / "sound.vdpk");
    }
    if (only == "all" || only == "cinematics") { // phase 14
        result |= vd::baker::bakeCinematics(mpq, outDir / "cinematics.vdpk");
    }
    if (only == "all" || only == "grass") { // phase 14: the terrain's detail doodads
        result |= vd::baker::bakeGrass(mpq, outDir / "grass.vdpk");
    }
    if (only == "all" || only == "worldsound") { // phase 14: doodad sounds, WMO areas, terrain emitters
        result |= vd::baker::bakeWorldSound(mpq, outDir / "worldsound.vdpk");
    }
    if (only == "all" || only == "bodyfx") { // phase 14: the units' sequence boxes (blob shadows)
        result |= vd::baker::bakeBodyFx(mpq, outDir / "bodyfx.vdpk");
    }
    if (only == "all" || only == "locale") { // phase 21: the overlays of the clients in other languages
        if (localeClients.empty() && only == "locale") {
            VD_LOG_ERROR("No client in another language (--locale-clients, VD_WOW_LOCALE_DIRS)");
            result |= 1;
        }
        for (const vd::baker::LocaleClient& client : localeClients) {
            result |= vd::baker::bakeLocale(mpq, client, outDir);
        }
    }
    // phase 22 (apppack): the app.vdpk step here
    if (only == "all") { // ours (texts, art, font): from the repository alone
        const std::string localeDir = vd::baker::option(options, "locale-dir", VD_DEFAULT_LOCALE_DIR);
        const std::string artDir = vd::baker::option(options, "art-dir", VD_DEFAULT_ART_DIR);
        // Phase 22 (polish): a published baker knows no repository (VD_DIST): there app.vdpk comes in the APK.
        if (!localeDir.empty() || !artDir.empty()) {
            result |= vd::baker::bakeApp(outDir / "app.vdpk", localeDir, artDir);
        }
    }

    // Phase 22: what this bake wrote, with this program's recipes (manifest.json in the packs folder).
    std::vector<std::string> overlayLocales;
    if (only == "all" || only == "locale") {
        for (const vd::baker::LocaleClient& client : localeClients) {
            overlayLocales.push_back(client.locale);
        }
    }
    vd::baker::recordBake(outDir, bakeStart, baseLocale, overlayLocales);
    return result;
}

// Phase 21: the language of the base client and of each client in another language.
int commandScanLocale(const Options& options) {
    vd::formats::MpqArchiveSet mpq;
    if (!openArchives(options, mpq)) {
        return 1;
    }
    const std::string baseLocale = vd::baker::detectClientLocale(mpq);
    std::printf("%-6s %s (base)\n", baseLocale.empty() ? "?" : baseLocale.c_str(),
                vd::baker::wowDir(options).string().c_str());
    const bool diff = vd::baker::option(options, "diff") == "1";
    for (const vd::baker::LocaleClient& client : vd::baker::openLocaleClients(options, baseLocale)) {
        std::printf("%-6s %s\n", client.locale.c_str(), client.dir.string().c_str());
        if (!diff) {
            continue;
        }
        // Every file of the client against the base client's: same bytes, other bytes of the same size, other
        // size, or only in one of them; by extension.
        struct Count {
            std::atomic<vd::u32> same{0}, sameSize{0}, otherSize{0}, onlyHere{0};
        };
        const std::vector<std::string> files = client.mpq->list("*");
        std::map<std::string, Count> byExtension;
        for (const std::string& file : files) {
            const std::string canonical = vd::normalizeAssetPath(file);
            const std::size_t dot = canonical.rfind('.');
            byExtension[dot == std::string::npos ? std::string("(none)") : canonical.substr(dot)];
        }
        std::mutex listMutex;
        std::vector<std::string> interfaceFiles;
        vd::JobSystem jobs;
        jobs.init();
        jobs.parallelFor(static_cast<vd::u32>(files.size()), 32, [&](vd::u32 begin, vd::u32 end) {
            for (vd::u32 i = begin; i < end; ++i) {
                const std::string canonical = vd::normalizeAssetPath(files[i]);
                const std::size_t dot = canonical.rfind('.');
                Count& count =
                    byExtension.at(dot == std::string::npos ? std::string("(none)") : canonical.substr(dot));
                vd::Result<std::vector<vd::u8>> mine = client.mpq->read(files[i]);
                vd::Result<std::vector<vd::u8>> theirs = mpq.read(files[i]);
                if (!mine) {
                    continue;
                }
                if (!theirs) {
                    ++count.onlyHere;
                } else if (mine.value() == theirs.value()) {
                    ++count.same;
                    continue;
                } else if (mine.value().size() == theirs.value().size()) {
                    ++count.sameSize;
                } else {
                    ++count.otherSize;
                }
                if (canonical.rfind("interface/", 0) == 0 &&
                    (canonical.ends_with(".lua") || canonical.ends_with(".xml") || canonical.ends_with(".toc"))) {
                    std::lock_guard<std::mutex> lock(listMutex);
                    interfaceFiles.push_back(files[i]);
                }
            }
        });
        jobs.shutdown();
        std::printf("  %-10s %8s %8s %10s %10s %8s\n", "extension", "files", "same", "same size", "other size",
                    "only it");
        for (const auto& [extension, count] : byExtension) {
            const vd::u32 total = count.same + count.sameSize + count.otherSize + count.onlyHere;
            if (total > count.same) {
                std::printf("  %-10s %8u %8u %10u %10u %8u\n", extension.c_str(), total, count.same.load(),
                            count.sameSize.load(), count.otherSize.load(), count.onlyHere.load());
            }
        }
        std::sort(interfaceFiles.begin(), interfaceFiles.end());
        for (const std::string& file : interfaceFiles) {
            std::printf("  interface: %s\n", file.c_str());
        }
    }
    return baseLocale.empty() ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::puts(kUsage);
        return 1;
    }
    Options options;
    if (!parseOptions(argc, argv, options)) {
        std::puts(kUsage);
        return 1;
    }
    if (!vd::baker::applyRunOptions(options)) { // phase 22: --jobs, --cache
        return 1;
    }
    const std::string command = argv[1];
    if (command == "list") {
        return commandList(options);
    }
    if (command == "scan-locale") { // phase 21
        return commandScanLocale(options);
    }
    if (command == "dump-texture") {
        return commandDumpTexture(options);
    }
    if (command == "scan-textures") {
        return commandScanTextures(options);
    }
    if (command == "scan-glue") { // phase 18
        return vd::baker::commandScanGlue(options);
    }
    if (command == "scan-models") {
        return vd::baker::commandScanModels(options);
    }
    if (command == "scan-actors") {
        return vd::baker::commandScanActors(options);
    }
    if (command == "scan-events") {
        return vd::baker::commandScanEvents(options);
    }
    if (command == "scan-sounds") {
        return vd::baker::commandScanSounds(options);
    }
    if (command == "scan-emitters") {
        return vd::baker::commandScanEmitters(options);
    }
    if (command == "probe-ground") {
        return vd::baker::commandProbeGround(options);
    }
    if (command == "scan-particles") {
        return vd::baker::commandScanParticles(options);
    }
    if (command == "scan-doodad-anim") {
        return vd::baker::commandScanDoodadAnim(options);
    }
    if (command == "scan-maps") {
        return vd::baker::commandScanMaps(options);
    }
    if (command == "scan-terrain") {
        return vd::baker::commandScanTerrain(options);
    }
    if (command == "extract") {
        return commandExtract(options);
    }
    if (command == "bake") {
        const vd::Clock::time_point start = vd::Clock::now();
        const int result = commandBake(options);
        vd::baker::logRunCost("bake", start); // phase 22: time and memory of each step, measured on a phone
        return result;
    }
    if (command == "dump-baked") {
        return commandDumpBaked(options);
    }
    if (command == "check-collision") {
        return vd::baker::commandCheckCollision(options);
    }
    if (command == "probe-walk") {
        return vd::baker::commandProbeWalk(options);
    }
    if (command == "scan-grass") {
        return vd::baker::commandScanGrass(options);
    }
    if (command == "digest") { // phase 22
        return vd::baker::commandDigest(options);
    }
    // phase 22 (wave 1) commands here
    if (command == "digest-assets") { // phase 22 (phonebaker)
        return vd::baker::commandDigestAssets(options);
    }
    if (command == "install") { // phase 22 (phonebake): a step baked on the phone into packs/
        return vd::baker::commandInstall(options);
    }
    VD_LOG_ERROR("Unknown command: %s", command.c_str());
    std::puts(kUsage);
    return 1;
}
