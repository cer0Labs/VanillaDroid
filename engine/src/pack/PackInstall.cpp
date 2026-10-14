#include "pack/PackInstall.h"

#include "pack/PackManifest.h"

#include <algorithm>

namespace vd::pack {

Result<InstallResult> installStagedPacks(const std::filesystem::path& stagingDir,
                                         const std::filesystem::path& packsDir) {
    std::error_code error;
    if (!std::filesystem::exists(stagingDir / kManifestFile, error)) {
        return Error{"install: no bake in " + stagingDir.string() + " (its manifest.json is missing)"};
    }
    Result<PackManifest> staged = PackManifest::load(stagingDir);
    if (!staged) {
        return staged.error();
    }
    if (staged.value().packs.empty()) {
        return Error{"install: the bake in " + stagingDir.string() + " wrote no pack"};
    }
    // Every pack first: a step whose packs are not all there whole moves nothing.
    for (const PackManifest::Entry& entry : staged.value().packs) {
        const std::filesystem::path from = stagingDir / entry.file;
        const u64 size = std::filesystem::file_size(from, error);
        if (error || (entry.size != 0 && size != entry.size)) {
            return Error{"install: " + from.string() + " is missing or not the size its bake recorded"};
        }
    }
    Result<PackManifest> loaded = PackManifest::load(packsDir);
    PackManifest manifest =
        loaded ? std::move(loaded.value()) : PackManifest{}; // a damaged one: written again
    std::filesystem::create_directories(packsDir, error);
    InstallResult result;
    for (const PackManifest::Entry& entry : staged.value().packs) {
        const std::filesystem::path from = stagingDir / entry.file;
        const std::filesystem::path to = packsDir / entry.file;
        std::filesystem::rename(from, to, error); // replaces the pack of an older bake
        if (error) {
            return Error{"install: cannot move " + from.string() + " to " + to.string() + ": " +
                         error.message()};
        }
        PackManifest::Entry& installed = manifest.record(entry.file);
        installed.recipe = entry.recipe;
        installed.size = entry.size;
        installed.sha256.clear(); // taken again when it is sent somewhere
        ++result.packs;
        result.bytes += entry.size;
    }
    const PackManifest& bake = staged.value();
    manifest.bakedBy = bake.bakedBy;
    manifest.clientBuild = bake.clientBuild;
    manifest.clientLocale = bake.clientLocale;
    for (const std::string& locale : bake.overlays) {
        if (std::find(manifest.overlays.begin(), manifest.overlays.end(), locale) ==
            manifest.overlays.end()) {
            manifest.overlays.push_back(locale);
        }
    }
    std::sort(manifest.overlays.begin(), manifest.overlays.end());
    if (Result<void> saved = manifest.save(packsDir); !saved) {
        return saved.error();
    }
    std::filesystem::remove(stagingDir / kManifestFile, error);
    std::filesystem::remove_all(stagingDir / ".cache", error);
    return result;
}

} // namespace vd::pack
