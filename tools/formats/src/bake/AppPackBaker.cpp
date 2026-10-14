#include "bake/AppPackBaker.h"

#include "bake/PackWriter.h"
#include "bake/TextureBaker.h"
#include "bake/UiBaker.h"
#include "bake/UiStrings.h"
#include "core/Log.h"
#include "pack/UiFormat.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace vd::bake {
namespace {

// Phase 20: the touch cluster's glyphs (docs/MOBILE_CONTROLS.md). One line per icon.
constexpr OwnArt kOwnArt[] = {
    {"touch/jump.svg", "Interface\\VanillaDroid\\Touch-Jump.blp", 128},
    {"touch/mount.svg", "Interface\\VanillaDroid\\Touch-Mount.blp", 128},
    {"touch/target.svg", "Interface\\VanillaDroid\\Touch-Target.blp", 128},
};

// DroidSans (Apache 2.0, art/fonts/LICENSE-DroidSans.txt): Latin, Latin-1 and Latin Extended-A like the
// client's.
constexpr OwnFont kOwnFonts[] = {
    {"fonts/DroidSans.ttf", "droidsans", 40.0f},
};

std::vector<u8> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

std::span<const OwnArt> ownArt() {
    return kOwnArt;
}

std::span<const OwnFont> ownFonts() {
    return kOwnFonts;
}

Result<AppPackStats> bakeAppPack(const std::filesystem::path& outPath, const std::filesystem::path& localeDir,
                                 const std::filesystem::path& artDir, u32 bakerVersion) {
    AppPackStats stats;
    PackWriter writer;
    VD_TRY(writer.create(outPath, bakerVersion));
    auto add = [&](std::string_view path, pack::AssetType type, std::span<const u8> data, bool compress) {
        if (Result<void> added = writer.add(path, type, data, compress); !added) {
            VD_LOG_ERROR("%s", added.error().message.c_str());
            ++stats.failures;
        }
    };

    // Texts.
    TextTables tables;
    if (!ownTextTables(localeDir, tables)) {
        ++stats.failures;
    }
    std::vector<std::pair<std::string, std::vector<u8>>> blobs;
    if (!stringTableBlobs(tables, blobs)) {
        ++stats.failures;
    }
    for (const auto& [locale, texts] : tables) {
        stats.texts += texts.size();
        VD_LOG_INFO("app: texts %s: %zu", locale.c_str(), texts.size());
    }
    for (const auto& [name, blob] : blobs) {
        add(name, pack::AssetType::UiStrings, blob, false);
    }
    stats.languages = blobs.size();

    // Art: an atlas of its own, with pages of ui.vdpk's size (the runtime appends them to its pages).
    std::vector<formats::BlpImage> images;
    images.reserve(std::size(kOwnArt)); // the inputs point into it: never reallocated
    std::vector<UiSpriteInput> inputs;
    for (const OwnArt& art : kOwnArt) {
        const std::filesystem::path file = artDir / art.file;
        const std::vector<u8> bytes = readFile(file);
        const std::string_view svg(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        Result<formats::BlpImage> image =
            svg.empty() ? Result<formats::BlpImage>(Error{"missing"}) : rasterizeSvgIcon(svg, art.size);
        if (!image) {
            VD_LOG_ERROR("Own art %s: %s", file.string().c_str(), image.error().message.c_str());
            ++stats.failures;
            continue;
        }
        images.push_back(std::move(image.value()));
        inputs.push_back({art.path, &images.back()});
    }
    if (!inputs.empty()) {
        Result<UiAtlas> atlas = packUiAtlas(inputs);
        if (!atlas) {
            return atlas.error();
        }
        std::vector<std::vector<u8>> pageBlobs;
        for (std::vector<u8>& page : atlas.value().pages) {
            formats::BlpImage image;
            image.width = atlas.value().pageSize;
            image.height = atlas.value().pageSize;
            image.alphaDepth = 8;
            image.encoding = formats::BlpEncoding::Argb8888;
            image.mips.push_back(std::move(page));
            Result<std::vector<u8>> blob = bakeTexture(image, pack::TextureFormat::Astc4x4);
            if (!blob) {
                return Error{"app atlas page: " + blob.error().message};
            }
            pageBlobs.push_back(std::move(blob.value()));
        }
        Result<std::vector<u8>> atlasBlob = buildTextureArray(pageBlobs);
        if (!atlasBlob) {
            return Error{"app atlas: " + atlasBlob.error().message};
        }
        const std::vector<pack::UiSprite>& sprites = atlas.value().sprites;
        std::vector<u8> spriteTable(sizeof(pack::UiSpritesHeader) + sprites.size() * sizeof(pack::UiSprite));
        const pack::UiSpritesHeader header{static_cast<u32>(sprites.size()), 0};
        std::memcpy(spriteTable.data(), &header, sizeof(header));
        std::memcpy(spriteTable.data() + sizeof(header), sprites.data(),
                    sprites.size() * sizeof(pack::UiSprite));
        // Mostly empty pages: LZ4 keeps the APK small (decompressed once at load).
        add(pack::kUiAtlasAsset, pack::AssetType::Texture, atlasBlob.value(), true);
        add(pack::kUiSpritesAsset, pack::AssetType::UiSprites, spriteTable, false);
        stats.sprites = sprites.size();
        stats.atlasPages = atlas.value().pages.size();
    }

    // Font.
    std::vector<std::vector<u8>> ttfs;
    std::vector<UiFontInput> fontInputs;
    for (const OwnFont& font : kOwnFonts) {
        std::vector<u8> ttf = readFile(artDir / font.file);
        if (ttf.empty()) {
            VD_LOG_ERROR("Own font %s: missing", (artDir / font.file).string().c_str());
            ++stats.failures;
            continue;
        }
        ttfs.push_back(std::move(ttf));
        UiFontInput input;
        input.name = font.name;
        input.pixelHeight = font.pixelHeight;
        fontInputs.push_back(std::move(input));
    }
    for (usize i = 0; i < fontInputs.size(); ++i) {
        fontInputs[i].ttf = ttfs[i];
    }
    if (!fontInputs.empty()) {
        const std::vector<u32> charset = uiCharacterSet();
        Result<UiFontBake> fonts = bakeUiFonts(fontInputs, charset);
        if (!fonts) {
            return Error{"app fonts: " + fonts.error().message};
        }
        add(pack::kUiFontAtlasAsset, pack::AssetType::Texture,
            buildR8Array(fonts.value().layers, pack::kUiFontAtlasSize), true);
        for (const auto& [name, blob] : fonts.value().fonts) {
            add(name, pack::AssetType::UiFont, blob, false);
            pack::UiFontHeader header{};
            std::memcpy(&header, blob.data(), sizeof(header));
            stats.glyphs += header.glyphCount;
        }
        stats.fontLayers = fonts.value().layers.size();
    }

    VD_TRY(writer.finish());
    stats.bytes = writer.bytesWritten();
    return stats;
}

} // namespace vd::bake
