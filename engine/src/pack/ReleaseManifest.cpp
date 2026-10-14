#include "pack/ReleaseManifest.h"

#include "core/Json.h"
#include "core/ReleaseSignature.h"

#include <algorithm>
#include <set>

namespace vd::pack {
namespace {

// The order of a full bake (tools/setup/BakeRunner's), so messages list the steps as the baker runs them.
constexpr std::string_view kStepOrder[] = {"db",        "textures",   "models", "actors", "world",
                                           "collision", "objects",    "ui",     "sound",  "cinematics",
                                           "grass",     "worldsound", "bodyfx", "locale"};

bool isHexDigest(std::string_view text) {
    if (text.size() != 64) {
        return false;
    }
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool isWebUrl(std::string_view url) {
    return url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0;
}

// A file entry: absent (no key) is an empty file; present, it must be whole.
Result<ReleaseFile> readFile(const json::Value& value, std::string_view name) {
    ReleaseFile file;
    if (value.isNull()) {
        return file;
    }
    file.url = std::string(value.get("url").asString());
    file.sha256 = std::string(value.get("sha256").asString());
    const double size = value.get("size").asNumber(-1.0);
    if (!isWebUrl(file.url)) {
        return Error{std::string(name) + ": no http(s) URL"};
    }
    if (!isHexDigest(file.sha256)) {
        return Error{std::string(name) + ": the SHA-256 is not 64 lowercase hex digits"};
    }
    if (size < 0.0) {
        return Error{std::string(name) + ": no size"};
    }
    file.size = static_cast<u64>(size);
    return file;
}

// The overlay's language of a pack file ("db.esES.vdpk" -> "esES"), empty for a base pack.
std::string_view overlayLocaleOf(std::string_view file) {
    if (file.size() <= 5 || file.substr(file.size() - 5) != ".vdpk") {
        return {};
    }
    const std::string_view stem = file.substr(0, file.size() - 5);
    const usize dot = stem.find('.');
    return dot == std::string_view::npos ? std::string_view() : stem.substr(dot + 1);
}

std::string_view recipeIn(std::span<const Recipe> recipes, std::string_view file) {
    const auto it =
        std::lower_bound(recipes.begin(), recipes.end(), file,
                         [](const Recipe& recipe, std::string_view name) { return recipe.file < name; });
    return it != recipes.end() && it->file == file ? std::string_view(it->recipe) : std::string_view();
}

bool listedIn(std::span<const Recipe> recipes, std::string_view file) {
    const auto it =
        std::lower_bound(recipes.begin(), recipes.end(), file,
                         [](const Recipe& recipe, std::string_view name) { return recipe.file < name; });
    return it != recipes.end() && it->file == file;
}

} // namespace

const ReleaseFile& ReleaseManifest::setupForThisSystem() const {
#if defined(_WIN32)
    return setupWindows;
#elif defined(__APPLE__)
    return setupMacos;
#else
    static const ReleaseFile kNone;
    return kNone;
#endif
}

u32 versionCodeOf(std::string_view version) {
    u32 parts[3] = {};
    usize part = 0;
    usize digits = 0;
    for (const char c : version) {
        if (c == '.') {
            if (digits == 0 || ++part > 2) {
                return 0;
            }
            digits = 0;
        } else if (c >= '0' && c <= '9' && digits < 3) {
            parts[part] = parts[part] * 10 + static_cast<u32>(c - '0');
            ++digits;
        } else {
            return 0;
        }
    }
    if (part != 2 || digits == 0) {
        return 0;
    }
    return parts[0] * 1000000u + parts[1] * 1000u + parts[2];
}

Result<ReleaseManifest> parseReleaseManifest(std::string_view text) {
    Result<json::Value> document = json::parse(text);
    if (!document) {
        return Error{"manifest.json: " + document.error().message};
    }
    const json::Value& root = document.value();
    if (!root.isObject()) {
        return Error{"manifest.json: not an object"};
    }
    ReleaseManifest manifest;
    manifest.format = root.get("format").asInt(0);
    manifest.version = std::string(root.get("version").asString());
    manifest.notes = std::string(root.get("notes").asString());
    if (manifest.tooNew()) {
        // A newer format may have changed anything else: only what tells the player where to look.
        manifest.versionCode = versionCodeOf(manifest.version);
        return manifest;
    }
    const u32 expected = versionCodeOf(manifest.version);
    if (expected == 0) {
        return Error{"manifest.json: the version is not X.Y.Z"};
    }
    if (root.get("versionCode").asInt(-1) != static_cast<i64>(expected)) {
        return Error{"manifest.json: versionCode does not match version " + manifest.version};
    }
    manifest.versionCode = expected;
    manifest.date = std::string(root.get("date").asString());
    if (!manifest.notes.empty() && !isWebUrl(manifest.notes)) {
        return Error{"manifest.json: notes is not an http(s) URL"};
    }
    Result<ReleaseFile> apk = readFile(root.get("apk"), "apk");
    if (!apk) {
        return apk.error();
    }
    manifest.apk = std::move(apk.value());
    const json::Value& setup = root.get("setup");
    Result<ReleaseFile> windows = readFile(setup.get("windows"), "setup.windows");
    if (!windows) {
        return windows.error();
    }
    manifest.setupWindows = std::move(windows.value());
    Result<ReleaseFile> macos = readFile(setup.get("macos"), "setup.macos");
    if (!macos) {
        return macos.error();
    }
    manifest.setupMacos = std::move(macos.value());
    for (const json::Member& member : root.get("packs").members()) {
        manifest.packs.push_back({member.key, std::string(member.value.asString())});
    }
    std::sort(manifest.packs.begin(), manifest.packs.end(),
              [](const Recipe& a, const Recipe& b) { return a.file < b.file; });
    return manifest;
}

SignedRelease readSignedRelease(std::string_view json, std::string_view signatureBase64) {
    return readSignedRelease(json, signatureBase64, releasePublicKey());
}

SignedRelease readSignedRelease(std::string_view json, std::string_view signatureBase64,
                                std::span<const u8> publicKey) {
    SignedRelease out;
    const std::span<const u8> bytes(reinterpret_cast<const u8*>(json.data()), json.size());
    if (!verifyRelease(bytes, signatureBase64, publicKey)) {
        out.status = SignedRelease::Status::BadSignature;
        return out;
    }
    Result<ReleaseManifest> parsed = parseReleaseManifest(json);
    if (!parsed) {
        out.status = SignedRelease::Status::Damaged;
        out.error = parsed.error().message;
        return out;
    }
    out.manifest = std::move(parsed.value());
    out.status = out.manifest.tooNew() ? SignedRelease::Status::TooNew : SignedRelease::Status::Ok;
    return out;
}

std::vector<StepPlan> bakeStepsFor(std::span<const std::string> packFiles) {
    std::vector<StepPlan> steps;
    for (const std::string_view name : kStepOrder) {
        std::set<std::string> maps;
        bool whole = false;
        bool any = false;
        for (const std::string& file : packFiles) {
            const BakeStep step = bakeStepOf(file);
            if (step.step != name) {
                continue;
            }
            any = true;
            if (step.map.empty()) {
                whole = true;
            } else {
                maps.insert(step.map);
            }
        }
        if (!any) {
            continue;
        }
        StepPlan plan;
        plan.step = std::string(name);
        if (!whole) {
            plan.maps.assign(maps.begin(), maps.end());
        }
        steps.push_back(std::move(plan));
    }
    return steps;
}

std::string describeSteps(std::span<const StepPlan> steps) {
    std::string text;
    for (const StepPlan& step : steps) {
        if (!text.empty()) {
            text += ", ";
        }
        text += step.step;
        if (!step.maps.empty()) {
            text += " (";
            for (usize i = 0; i < step.maps.size(); ++i) {
                text += (i ? ", " : "") + step.maps[i];
            }
            text += ")";
        }
    }
    return text;
}

std::vector<std::string> releaseDataChanges(const PackManifest& folder, std::span<const Recipe> release,
                                            std::span<const Recipe> current) {
    std::vector<std::string> changed;
    // A manifest that records no client pack (only app.vdpk, say) tells nothing about them.
    const bool hasManifest =
        std::any_of(folder.packs.begin(), folder.packs.end(),
                    [](const PackManifest::Entry& entry) { return isClientPack(entry.file); });
    for (const Recipe& recipe : release) {
        if (!isClientPack(recipe.file)) {
            continue;
        }
        const std::string_view locale = overlayLocaleOf(recipe.file);
        if (!locale.empty() && (!hasManifest || std::find(folder.overlays.begin(), folder.overlays.end(),
                                                          locale) == folder.overlays.end())) {
            continue; // a language this folder was not baked with
        }
        const PackManifest::Entry* entry = hasManifest ? folder.find(recipe.file) : nullptr;
        std::string_view before;
        if (entry) {
            before = entry->recipe;
        } else if (listedIn(current, recipe.file)) {
            if (hasManifest) {
                continue; // the folder lacked it already: not this release's doing
            }
            before = recipeIn(current, recipe.file);
        } else {
            changed.push_back(recipe.file); // new in this release
            continue;
        }
        if (!before.empty() && !recipe.recipe.empty() && before != recipe.recipe) {
            changed.push_back(recipe.file);
        }
    }
    std::sort(changed.begin(), changed.end());
    return changed;
}

} // namespace vd::pack
