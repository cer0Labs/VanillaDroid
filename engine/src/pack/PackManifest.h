#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Phase 22 (distribution, ADR-0005 §3): which packs have to be baked again after an update.
//
// A pack's RECIPE is a digest of its content (vd_baker digest: its assets' paths, types and bytes, not their
// order in the file) as the reference clients bake it. release/packs.json lists the recipe of every client
// pack of a release and is built into the program (releaseRecipes()). Each bake leaves packs/manifest.json in
// its folder with the recipe each pack was baked to. A pack whose recipe differs from the program's was baked
// by an older baker: the bake step that writes it (bakeStepOf) runs again. app.vdpk (our own texts and art)
// is not a client pack: it travels inside the APK.
namespace vd::pack {

inline constexpr std::string_view kManifestFile = "manifest.json"; // inside the packs folder
inline constexpr std::string_view kAppPackFile = "app.vdpk";

// The bake step (vd_baker bake --only <step>) that writes a pack file, and the --maps argument of the per-map
// steps: "world_azeroth.vdpk" -> {"world", "azeroth"}, "collision_objects.vdpk" -> {"objects", ""},
// "db.esES.vdpk" -> {"locale", ""}. An empty step for a name no step writes.
struct BakeStep {
    std::string step;
    std::string map;
};
BakeStep bakeStepOf(std::string_view packFile);
// Packs baked from the client's data: every *.vdpk but app.vdpk.
bool isClientPack(std::string_view packFile);

// release/packs.json: {"format": 1, "packs": {"models.vdpk": "<recipe>", ...}}, sorted by file name.
struct Recipe {
    std::string file;
    std::string recipe;
};
Result<std::vector<Recipe>> parseRecipes(std::string_view json);
std::string writeRecipes(const std::vector<Recipe>& recipes);
// The recipes of this program's release (release/packs.json when it was built); empty in a development build
// made before any `vd_baker digest`.
const std::vector<Recipe>& releaseRecipes();
std::string_view releaseRecipeOf(std::string_view packFile); // empty when the release does not list it

// packs/manifest.json: what the bakes left in a packs folder.
struct PackManifest {
    struct Entry {
        std::string file;
        // releaseRecipeOf() of the baker that wrote it; empty if its release did not know it.
        std::string recipe;
        u64 size = 0;
        std::string sha256; // of the file, filled when it is sent to a phone (empty until then)
    };

    std::string bakedBy;      // version of the program that baked the latest of them (core/Version.h)
    u32 clientBuild = 0;      // 5875
    std::string clientLocale; // the base client's language ("enUS", "esES"...)
    std::vector<std::string> overlays; // the languages of the overlays db.<locale>.vdpk...
    std::vector<Entry> packs;          // sorted by file name

    // A missing file is an empty manifest; a damaged one is an error.
    static Result<PackManifest> load(const std::filesystem::path& packsDir);
    // Written to a temporary name, then renamed.
    Result<void> save(const std::filesystem::path& packsDir) const;

    const Entry* find(std::string_view file) const;
    Entry& record(std::string_view file); // the entry, added in its sorted place if it is new
    bool remove(std::string_view file);
};

// The client packs of the manifest whose recipe is not the one of `recipes` (an empty recorded or expected
// recipe counts as up to date: nothing to compare), and the release's packs the folder lacks.
std::vector<std::string> outdatedPacks(const PackManifest& manifest, const std::vector<Recipe>& recipes);

} // namespace vd::pack
