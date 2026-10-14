#pragma once

#include "core/Clock.h"
#include "core/Types.h"
#include "formats/Adt.h"
#include "formats/MpqArchiveSet.h"
#include "pack/PackFormat.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vd::baker {

// Bump when baked output changes for the same input (forces consumers to re-bake).
constexpr u32 kBakerVersion = 1;

using Options = std::map<std::string, std::string>;

// Value of --name, or `fallback`.
std::string option(const Options& options, const char* name, const std::string& fallback = {});

// --wow-dir, then $VD_WOW_DIR, then the path configured in CMake.
std::filesystem::path wowDir(const Options& options);
bool openArchives(const Options& options, formats::MpqArchiveSet& mpq);

bool matchesFilter(const std::string& canonicalPath, const std::string& filter);

// The bake caches' keys: the input bytes and the settings that change the output (textures: textures.vdpk and the
// language overlays; sounds: sound.vdpk and the overlays).
u64 textureCacheKey(const std::vector<u8>& blpBytes, pack::TextureFormat format);
u64 soundCacheKey(const std::vector<u8>& bytes, bool mp3);

// Bake cache: one file per baked asset, keyed by a hash of (input bytes, settings, encoder version).
// A re-bake only redoes the assets whose input or settings changed.
class BakeCache {
public:
    explicit BakeCache(std::filesystem::path dir);

    bool load(u64 key, std::vector<u8>& out) const;
    void store(u64 key, const std::vector<u8>& blob) const;

private:
    std::filesystem::path pathFor(u64 key) const;

    std::filesystem::path m_dir;
};

// World commands (WorldCommands.cpp).
struct MapInfo {
    std::string directory; // as spelled in the MPQ, e.g. "Azeroth"
    formats::Wdt wdt;
};
// Every map with a WDT, sorted by name. Maps without terrain tiles (WMO-only instances) included.
std::vector<MapInfo> listMaps(const formats::MpqArchiveSet& mpq);
// `filter`: comma-separated map directories, case-insensitive; empty or "all" selects every map.
bool mapSelected(const std::string& directory, const std::string& filter);
int commandScanTerrain(const Options& options);
int commandScanMaps(const Options& options);
int commandScanModels(const Options& options);
// Every WMO and every M2 the world places -> <outPath> (models.vdpk).
int bakeModels(const formats::MpqArchiveSet& mpq, const std::filesystem::path& outPath);
// Terrain of every map in `mapFilter` (comma-separated directory names, empty or "all" = every map)
// into <outDir>/world_<map>.vdpk.
int bakeWorld(const formats::MpqArchiveSet& mpq, const std::filesystem::path& outDir, const std::string& mapFilter);

// Collision of every map in `mapFilter` into <outDir>/collision_<map>.vdpk (CollisionCommands.cpp).
int bakeCollision(const formats::MpqArchiveSet& mpq, const std::filesystem::path& outDir, const std::string& mapFilter);
// Phase 8: collision_objects.vdpk, the collision of game object models in their own space (transports).
int bakeObjectCollision(const formats::MpqArchiveSet& mpq, const std::filesystem::path& outDir);
// Compares the baked ground under a list of points (x y z per line, e.g. creature spawns) with their height.
int commandCheckCollision(const Options& options);
// Walks the character controller from --from to --to over the baked collision and explains where it stops.
int commandProbeWalk(const Options& options);

// Actor commands (ActorCommands.cpp).
int commandScanActors(const Options& options);
// Phase 14: census of the M2 events (ids, timed or always on, data words).
int commandScanEvents(const Options& options);
// Race, creature and item models, animated -> <outPath> (actors.vdpk).
int bakeActors(const formats::MpqArchiveSet& mpq, const std::filesystem::path& outPath);

// Phase 14 (CinematicCommands.cpp): the cinematics' cameras -> <outPath> (cinematics.vdpk).
int bakeCinematics(const formats::MpqArchiveSet& mpq, const std::filesystem::path& outPath);
// Phase 18: the glue scenes' cameras, lights and sequences (scan-glue).
int commandScanGlue(const Options& options);

// Effects (EffectCommands.cpp): particle emitters, ribbons and texture transforms of every M2.
int commandScanParticles(const Options& options);

// Phase 14 (bodyfx, BodyFxCommands.cpp): the sequence boxes of the units' models -> <outPath> (bodyfx.vdpk).
int bakeBodyFx(const formats::MpqArchiveSet& mpq, const std::filesystem::path& outPath);
// Phase 14 (DoodadCommands.cpp): placed doodads that move or turn to the camera, and their instances in the cities.
int commandScanDoodadAnim(const Options& options);

// Phase 21 (LocaleCommands.cpp): clients of the same build in other languages.
struct LocaleClient {
    std::string locale; // its language, from its DBC texts: "esES"
    std::filesystem::path dir;
    std::unique_ptr<formats::MpqArchiveSet> mpq;
};
// The language of a client's texts (which of the 8 columns of a DBC text it fills), empty when none is found.
std::string detectClientLocale(const formats::MpqArchiveSet& mpq);
// The clients in --locale-clients, else $VD_WOW_LOCALE_DIRS, else CMake's VD_WOW_LOCALE_DIRS (';'-separated), opened
// with their language; a folder that does not exist is skipped, as is a client in `baseLocale` or a repeated one.
std::vector<LocaleClient> openLocaleClients(const Options& options, const std::string& baseLocale);
// The language overlays of `client` over the base client `base`: <outDir>/db.<locale>.vdpk, textures.<locale>.vdpk,
// sound.<locale>.vdpk.
int bakeLocale(const formats::MpqArchiveSet& base, const LocaleClient& client, const std::filesystem::path& outDir);

// Touch UI (UiCommands.cpp): interface texture atlas, fonts as distance fields, minimap tables -> <outPath> (ui.vdpk).
// The client's texts (GlobalStrings.lua, GlueStrings.lua) come from mpq, in `baseLocale` (the pack's header), and
// phase 21: from each of `localeClients` for its language. Phase 22: only what comes from the clients (ours: bakeApp).
int bakeUi(const formats::MpqArchiveSet& mpq, const std::filesystem::path& outPath, const std::string& baseLocale,
           const std::vector<LocaleClient>& localeClients);

// Sound (SoundCommands.cpp): every file SoundEntries.dbc names -> <outPath> (sound.vdpk); a census of the client's
// WAV and MP3 files.
int bakeSound(const formats::MpqArchiveSet& mpq, const std::filesystem::path& outPath);
// Every file SoundEntries.dbc names (and the login music), as spelled in the client, one per canonical path;
// `references`: how many times they are named.
std::vector<std::string> soundPaths(const formats::MpqArchiveSet& mpq, u32& references);
int commandScanSounds(const Options& options);
// The footstep surface the sound bake finds at a point: the chunk's layers, their ground effects, the cells.
int commandProbeGround(const Options& options);

// Phase 14 (GrassCommands.cpp): the terrain's detail doodads -> <outPath> (grass.vdpk), and their census per map.
int bakeGrass(const formats::MpqArchiveSet& mpq, const std::filesystem::path& outPath);
int commandScanGrass(const Options& options);
// Phase 14 (WorldSoundCommands.cpp): the doodads' sound events, the buildings' rooms (WMOAreaTable) and the terrain's
// sound emitters -> <outPath> (worldsound.vdpk); a census of the emitters (ADT MCSE).
int bakeWorldSound(const formats::MpqArchiveSet& mpq, const std::filesystem::path& outPath);
int commandScanEmitters(const Options& options);

// Phase 22 (DistributionCommands.cpp, ADR-0005): `digest` writes the recipe of every client pack of a folder
// (release/packs.json); after a bake, recordBake() notes in <outDir>/manifest.json the packs it wrote since `since`,
// with this program's recipe for each (pack/PackManifest.h), the client's build and languages.
int commandDigest(const Options& options);
void recordBake(const std::filesystem::path& outDir, std::filesystem::file_time_type since,
                const std::string& baseLocale, const std::vector<std::string>& overlayLocales);

// phase 22 (wave 1) declarations here
// Phase 22 (apppack, AppCommands.cpp): app.vdpk, ours, without the WoW client: our texts (`localeDir`: locale/), our
// art and font (`artDir`: art/) -> <outPath>.
int bakeApp(const std::filesystem::path& outPath, const std::filesystem::path& localeDir,
            const std::filesystem::path& artDir);
// Phase 22 (setup): the archives left out of a client: --skip-archives for the base client, --locale-skip-archives for
// each --locale-clients client; names separated by ';' or '|', any case. VanillaDroid Setup leaves out the archives
// that are not official (a private server's patch-3.MPQ, patch-A.MPQ...) unless the player includes them.
std::vector<std::string> skipArchives(const Options& options, const char* name);
// Phase 22 (phonebaker, RunOptions.cpp): options of every command, for baking on a phone. --jobs N: the threads that
// bake (the calling one and N - 1 workers in every JobSystem; default: every core but one); --cache <dir>|off: the
// bake caches in <dir> instead of <out>/.cache, or none (everything baked again, nothing written). False, with an
// error logged, on a bad value.
bool applyRunOptions(const Options& options);
// A bake cache's folder (<out>/.cache/<name>) after --cache: itself, <dir>/<name>, or empty (no cache).
std::filesystem::path bakeCacheDir(const std::filesystem::path& dir);
// One log line with what the process has cost since `start`: time, CPU time of every thread, peak memory.
void logRunCost(const char* what, Clock::time_point start);
// Phase 22 (phonebaker, AssetDigest.cpp): "<digest> <type> <raw size> <name>" for every asset of --pack, by name.
int commandDigestAssets(const Options& options);
// Phase 22 (phonebake, PhoneBakeCommands.cpp): install --staging <dir> --packs <dir>: the packs a step baked
// into a staging folder (its manifest.json says which) into the packs folder, their recipes into its
// manifest.json.
int commandInstall(const Options& options);

} // namespace vd::baker
