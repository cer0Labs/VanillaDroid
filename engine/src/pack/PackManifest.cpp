#include "pack/PackManifest.h"

#include "core/Json.h"
#include "vd_release_packs.gen.h" // kReleasePacksJson: release/packs.json at build time

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>

namespace vd::pack {

namespace {

constexpr u32 kFormat = 1;

bool startsWith(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

Result<std::string> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return Error{"Cannot open " + path.string()};
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

Result<void> writeFileAtomically(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::path temp = path;
    temp += ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            return Error{"Cannot write " + temp.string()};
        }
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file) {
            return Error{"Cannot write " + temp.string()};
        }
    }
    std::error_code error;
    std::filesystem::rename(temp, path, error);
    if (error) {
        std::filesystem::remove(temp, error);
        return Error{"Cannot replace " + path.string()};
    }
    return {};
}

} // namespace

BakeStep bakeStepOf(std::string_view packFile) {
    if (packFile.size() <= 5 || packFile.substr(packFile.size() - 5) != ".vdpk") {
        return {};
    }
    const std::string_view stem = packFile.substr(0, packFile.size() - 5);
    if (stem.find('.') != std::string_view::npos) {
        return {"locale", ""}; // db.esES, textures.esES, sound.esES (phase 21)
    }
    if (stem == "collision_objects") {
        return {"objects", ""};
    }
    if (startsWith(stem, "world_")) {
        return {"world", std::string(stem.substr(6))};
    }
    if (startsWith(stem, "collision_")) {
        return {"collision", std::string(stem.substr(10))};
    }
    if (stem == "liquids") {
        return {"world", ""}; // every world bake rewrites the shared liquids
    }
    static constexpr std::string_view kWholeSteps[] = {"db",         "textures", "models",     "actors",
                                                       "ui",         "sound",    "cinematics", "grass",
                                                       "worldsound", "bodyfx",   "app"};
    for (const std::string_view step : kWholeSteps) {
        if (stem == step) {
            return {std::string(step), ""};
        }
    }
    return {};
}

bool isClientPack(std::string_view packFile) {
    return packFile != kAppPackFile && !bakeStepOf(packFile).step.empty();
}

Result<std::vector<Recipe>> parseRecipes(std::string_view json) {
    Result<json::Value> document = json::parse(json);
    if (!document) {
        return document.error();
    }
    const json::Value& root = document.value();
    if (root.get("format").asInt() != kFormat) {
        return Error{"release/packs.json: unknown format"};
    }
    std::vector<Recipe> recipes;
    for (const json::Member& member : root.get("packs").members()) {
        recipes.push_back({member.key, std::string(member.value.asString())});
    }
    std::sort(recipes.begin(), recipes.end(),
              [](const Recipe& a, const Recipe& b) { return a.file < b.file; });
    return recipes;
}

std::string writeRecipes(const std::vector<Recipe>& recipes) {
    std::vector<Recipe> sorted = recipes;
    std::sort(sorted.begin(), sorted.end(), [](const Recipe& a, const Recipe& b) { return a.file < b.file; });
    json::Value root = json::Value::object();
    root.set("format", json::Value::number(kFormat));
    json::Value& packs = root.set("packs", json::Value::object());
    for (const Recipe& recipe : sorted) {
        packs.set(recipe.file, json::Value::string(recipe.recipe));
    }
    return json::write(root);
}

const std::vector<Recipe>& releaseRecipes() {
    static const std::vector<Recipe> recipes = [] {
        Result<std::vector<Recipe>> parsed = parseRecipes(kReleasePacksJson);
        return parsed ? std::move(parsed.value()) : std::vector<Recipe>{};
    }();
    return recipes;
}

std::string_view releaseRecipeOf(std::string_view packFile) {
    const std::vector<Recipe>& recipes = releaseRecipes();
    const auto it =
        std::lower_bound(recipes.begin(), recipes.end(), packFile,
                         [](const Recipe& recipe, std::string_view file) { return recipe.file < file; });
    return it != recipes.end() && it->file == packFile ? std::string_view(it->recipe) : std::string_view();
}

Result<PackManifest> PackManifest::load(const std::filesystem::path& packsDir) {
    const std::filesystem::path path = packsDir / kManifestFile;
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return PackManifest{};
    }
    Result<std::string> text = readFile(path);
    if (!text) {
        return text.error();
    }
    Result<json::Value> document = json::parse(text.value());
    if (!document) {
        return Error{path.string() + ": " + document.error().message};
    }
    const json::Value& root = document.value();
    if (root.get("format").asInt() != kFormat) {
        return Error{path.string() + ": unknown format"};
    }
    PackManifest manifest;
    manifest.bakedBy = root.get("bakedBy").asString();
    const json::Value& client = root.get("client");
    manifest.clientBuild = static_cast<u32>(client.get("build").asInt());
    manifest.clientLocale = client.get("locale").asString();
    for (const json::Value& overlay : client.get("overlays").items()) {
        manifest.overlays.emplace_back(overlay.asString());
    }
    for (const json::Member& member : root.get("packs").members()) {
        Entry& entry = manifest.record(member.key);
        entry.recipe = member.value.get("recipe").asString();
        entry.size = static_cast<u64>(member.value.get("size").asInt());
        entry.sha256 = member.value.get("sha256").asString();
    }
    return manifest;
}

Result<void> PackManifest::save(const std::filesystem::path& packsDir) const {
    json::Value root = json::Value::object();
    root.set("format", json::Value::number(kFormat));
    root.set("bakedBy", json::Value::string(bakedBy));
    json::Value& client = root.set("client", json::Value::object());
    client.set("build", json::Value::number(clientBuild));
    client.set("locale", json::Value::string(clientLocale));
    json::Value& overlayList = client.set("overlays", json::Value::array());
    for (const std::string& overlay : overlays) {
        overlayList.push(json::Value::string(overlay));
    }
    json::Value& packList = root.set("packs", json::Value::object());
    for (const Entry& entry : packs) {
        json::Value& value = packList.set(entry.file, json::Value::object());
        value.set("recipe", json::Value::string(entry.recipe));
        value.set("size", json::Value::number(static_cast<double>(entry.size)));
        if (!entry.sha256.empty()) {
            value.set("sha256", json::Value::string(entry.sha256));
        }
    }
    return writeFileAtomically(packsDir / kManifestFile, json::write(root));
}

const PackManifest::Entry* PackManifest::find(std::string_view file) const {
    const auto it =
        std::lower_bound(packs.begin(), packs.end(), file,
                         [](const Entry& entry, std::string_view name) { return entry.file < name; });
    return it != packs.end() && it->file == file ? &*it : nullptr;
}

PackManifest::Entry& PackManifest::record(std::string_view file) {
    const auto it =
        std::lower_bound(packs.begin(), packs.end(), file,
                         [](const Entry& entry, std::string_view name) { return entry.file < name; });
    if (it != packs.end() && it->file == file) {
        return *it;
    }
    Entry entry;
    entry.file = std::string(file);
    return *packs.insert(it, std::move(entry));
}

bool PackManifest::remove(std::string_view file) {
    const auto it =
        std::find_if(packs.begin(), packs.end(), [&](const Entry& entry) { return entry.file == file; });
    if (it == packs.end()) {
        return false;
    }
    packs.erase(it);
    return true;
}

std::vector<std::string> outdatedPacks(const PackManifest& manifest, const std::vector<Recipe>& recipes) {
    std::vector<std::string> outdated;
    for (const Recipe& recipe : recipes) {
        if (!isClientPack(recipe.file)) {
            continue;
        }
        const PackManifest::Entry* entry = manifest.find(recipe.file);
        if (!entry) {
            // An overlay ("db.esES.vdpk") is missing only when the folder was baked with that language.
            bool wanted = true;
            if (bakeStepOf(recipe.file).step == "locale") {
                const std::string_view file = recipe.file;
                const usize dot = file.find('.');
                const std::string_view locale = file.substr(dot + 1, file.size() - 5 - dot - 1);
                wanted = std::find(manifest.overlays.begin(), manifest.overlays.end(), locale) !=
                         manifest.overlays.end();
            }
            if (wanted) {
                outdated.push_back(recipe.file);
            }
            continue;
        }
        if (!entry->recipe.empty() && !recipe.recipe.empty() && entry->recipe != recipe.recipe) {
            outdated.push_back(recipe.file);
        }
    }
    return outdated;
}

} // namespace vd::pack
