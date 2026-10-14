// Phase 22 (distribution, ADR-0005 §3): the recipes of the packs and the packs folder's manifest.
#include "BakerCommon.h"

#include "core/Clock.h"
#include "core/Log.h"
#include "core/Version.h"
#include "jobs/JobSystem.h"
#include "pack/PackDigest.h"
#include "pack/PackManifest.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>

namespace vd::baker {

namespace {

// The client packs of a folder (*.vdpk but app.vdpk), sorted by name.
std::vector<std::string> clientPacks(const std::filesystem::path& dir) {
    std::vector<std::string> files;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file() && pack::isClientPack(name)) {
            files.push_back(name);
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace

int commandDigest(const Options& options) {
    const std::filesystem::path dir = option(options, "packs", "packs");
    const std::vector<std::string> files = clientPacks(dir);
    if (files.empty()) {
        VD_LOG_ERROR("digest: no packs in %s", dir.string().c_str());
        return 1;
    }
    const auto start = Clock::now();
    std::vector<pack::Recipe> recipes(files.size());
    std::atomic<u32> failures{0};
    JobSystem jobs;
    jobs.init();
    jobs.parallelFor(static_cast<u32>(files.size()), 1, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            recipes[i].file = files[i];
            Result<std::string> digest = pack::packDigest(dir / files[i]);
            if (!digest) {
                VD_LOG_ERROR("digest: %s", digest.error().message.c_str());
                ++failures;
                continue;
            }
            recipes[i].recipe = std::move(digest.value());
        }
    });
    jobs.shutdown();
    const std::string text = pack::writeRecipes(recipes);
    const std::string out = option(options, "out");
    if (out.empty()) {
        std::fputs(text.c_str(), stdout);
    } else {
        std::ofstream file(out, std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file) {
            VD_LOG_ERROR("digest: cannot write %s", out.c_str());
            return 1;
        }
    }
    VD_LOG_INFO("digest: %zu packs, %u failed, %.1f s%s%s", files.size(), failures.load(),
                secondsBetween(start, Clock::now()), out.empty() ? "" : " -> ", out.c_str());
    return failures == 0 ? 0 : 1;
}

void recordBake(const std::filesystem::path& outDir, std::filesystem::file_time_type since,
                const std::string& baseLocale, const std::vector<std::string>& overlayLocales) {
    Result<pack::PackManifest> loaded = pack::PackManifest::load(outDir);
    if (!loaded) {
        VD_LOG_WARN("%s: written again from scratch", loaded.error().message.c_str());
    }
    pack::PackManifest manifest = loaded ? std::move(loaded.value()) : pack::PackManifest{};
    u32 recorded = 0;
    for (const std::string& file : clientPacks(outDir)) {
        std::error_code error;
        const std::filesystem::path path = outDir / file;
        const std::filesystem::file_time_type written = std::filesystem::last_write_time(path, error);
        if (error || written < since) {
            continue;
        }
        pack::PackManifest::Entry& entry = manifest.record(file);
        entry.recipe = std::string(pack::releaseRecipeOf(file));
        entry.size = std::filesystem::file_size(path, error);
        entry.sha256.clear(); // the file changed: its digest is taken again when it is sent
        ++recorded;
    }
    if (recorded == 0) {
        return;
    }
    manifest.bakedBy = kVersion;
    manifest.clientBuild = pack::kClientBuild;
    manifest.clientLocale = baseLocale;
    for (const std::string& locale : overlayLocales) {
        if (std::find(manifest.overlays.begin(), manifest.overlays.end(), locale) ==
            manifest.overlays.end()) {
            manifest.overlays.push_back(locale);
        }
    }
    std::sort(manifest.overlays.begin(), manifest.overlays.end());
    if (Result<void> saved = manifest.save(outDir); !saved) {
        VD_LOG_ERROR("%s", saved.error().message.c_str());
        return;
    }
    VD_LOG_INFO("manifest: %u packs recorded in %s", recorded,
                (outDir / pack::kManifestFile).string().c_str());
}

} // namespace vd::baker
