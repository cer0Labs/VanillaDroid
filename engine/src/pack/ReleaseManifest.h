#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "pack/PackManifest.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

// Phase 22 (distribution, ADR-0005 §6-§7): reading a public release. Its manifest.json (written and signed by
// the release workflow, scripts/release-manifest.ps1) says what the latest release is: its version, the APK
// and the Setup of each system (URL, size, SHA-256) and the recipes of its packs. VanillaDroid Setup (vdsetup
// update) and the app read it to tell the player about an update and to fetch it. Nothing from it is used
// unless its signature verifies (core/ReleaseSignature) and each file's SHA-256 is the manifest's. The
// comparisons here say what is new for this program (its versionCode) and which bake steps a packs folder
// needs again (its recipes against the release's).
namespace vd::pack {

inline constexpr i64 kReleaseFormat = 1;
// The latest release's manifest: GitHub serves the latest release's asset at this fixed URL. Its signature is
// the same URL + kReleaseSignatureSuffix.
inline constexpr std::string_view kReleaseManifestUrl =
    "https://github.com/cer0Labs/VanillaDroid/releases/latest/download/manifest.json";
inline constexpr std::string_view kReleaseSignatureSuffix = ".sig";
// The page of the latest release, for what a program cannot do by itself (a manifest too new for it).
inline constexpr std::string_view kReleasePageUrl =
    "https://github.com/cer0Labs/VanillaDroid/releases/latest";
// Bounds for the downloads of the manifest and its signature (a few KB each).
inline constexpr usize kMaxReleaseManifestBytes = 1u << 20;
inline constexpr usize kMaxReleaseSignatureBytes = 4096;

struct ReleaseFile {
    std::string url;
    u64 size = 0;
    std::string sha256; // lowercase hex

    bool present() const { return !url.empty() && sha256.size() == 64; }
};

struct ReleaseManifest {
    i64 format = 0;
    std::string version; // "0.2.0"
    u32 versionCode = 0; // X * 1,000,000 + Y * 1,000 + Z
    std::string date;    // "2026-10-20"
    std::string notes;   // the release's page: what is new
    ReleaseFile apk;
    ReleaseFile setupWindows;
    ReleaseFile setupMacos;
    std::vector<Recipe> packs; // sorted by file name

    // A format this program does not know: a newer release than it can read (the player updates by hand).
    bool tooNew() const { return format != kReleaseFormat; }
    // The Setup for the system this program was built for (an empty file on others).
    const ReleaseFile& setupForThisSystem() const;
};

// "1.2.3" -> 1002003 (core/Version.h's rule); 0 when it is not X.Y.Z with parts of at most three digits.
u32 versionCodeOf(std::string_view version);

// Parses manifest.json (not its signature). One of another format parses with only `format`, `version` and
// `notes` (tooNew()). Errors (English, for logs): not JSON, a version or versionCode missing or not matching,
// a file entry without URL or with a malformed SHA-256.
Result<ReleaseManifest> parseReleaseManifest(std::string_view json);

// A downloaded manifest and its signature, checked.
struct SignedRelease {
    enum class Status : u8 {
        Ok,           // verified and parsed: `manifest`
        BadSignature, // the signature does not verify: nothing from it may be used
        Damaged,      // verified, but not a manifest this program can read (`error`)
        TooNew,       // verified, of a newer format: `manifest` has its version and notes
    };
    Status status = Status::BadSignature;
    ReleaseManifest manifest;
    std::string error;
};
// The signature over the exact bytes of `json` against the release key built into this program, then the
// parse.
SignedRelease readSignedRelease(std::string_view json, std::string_view signatureBase64);
// The same against another key (tests).
SignedRelease readSignedRelease(std::string_view json, std::string_view signatureBase64,
                                std::span<const u8> publicKey);

// A bake step with the maps it covers (world and collision: some maps; empty: the whole step).
struct StepPlan {
    std::string step;
    std::vector<std::string> maps;
};
// The steps that write these pack files, in the order of a full bake, maps merged ("world_azeroth.vdpk",
// "world_kalimdor.vdpk" -> world (azeroth, kalimdor)). app.vdpk and unknown names are left out; liquids.vdpk
// is the whole world step.
std::vector<StepPlan> bakeStepsFor(std::span<const std::string> packFiles);
// "world (azeroth, kalimdor), models": for the player's messages.
std::string describeSteps(std::span<const StepPlan> steps);

// The client packs `release` bakes differently from what a packs folder holds, sorted: by the folder's
// manifest when it records client packs (a pack it records with another recipe, or a pack new in the release
// that `current` did not list either), else by `current` (the folder is taken as baked by this program: its
// recipes, normally releaseRecipes()). Packs the folder already lacked before the release are not its doing:
// left out. Language overlays only when the folder has that language.
std::vector<std::string> releaseDataChanges(const PackManifest& folder, std::span<const Recipe> release,
                                            std::span<const Recipe> current);

} // namespace vd::pack
