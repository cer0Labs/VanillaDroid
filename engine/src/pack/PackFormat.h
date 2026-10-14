#pragma once

#include "core/Types.h"

namespace vd::pack {

// On-disk layout of a VDPK pack (little-endian, shared by the baker and the runtime).
//
//   PackHeader (64 bytes)
//   asset blobs, each aligned to kBlobAlignment
//   PackEntry[entryCount], sorted by pathHash (binary search)
//   name table: NUL-terminated canonical paths (debug/tooling; may be stripped later)
//
// Bump kPackVersion on any layout change: the runtime refuses packs with another version.

constexpr char kPackMagic[4] = {'V', 'D', 'P', 'K'};
constexpr u32 kPackVersion = 1;
constexpr u32 kClientBuild = 5875;
constexpr u64 kBlobAlignment = 64;
constexpr u32 kNoName = 0xFFFFFFFFu;

enum class AssetType : u16 {
    Dbc = 1,         // raw client table (WDBC), read in place
    Texture = 2,     // TextureHeader + mips
    TerrainMap = 3,  // TerrainMapHeader (pack/TerrainFormat.h)
    TerrainTile = 4, // TerrainTileHeader (pack/TerrainFormat.h)
    TerrainHorizon = 5, // HorizonHeader (pack/TerrainFormat.h)
    Model = 6,          // ModelHeader (pack/ModelFormat.h): baked M2 or WMO
    ActorModel = 7,     // ActorModelHeader (pack/ActorFormat.h): animated M2 (races, creatures, items)
    CollisionTile = 8,  // CollisionTileHeader (pack/CollisionFormat.h): what a tile collides with
    UiSprites = 9,      // UiSpritesHeader (pack/UiFormat.h): the interface textures in the UI atlas
    UiFont = 10,        // UiFontHeader (pack/UiFormat.h): glyph metrics of a font
    UiMinimap = 11,     // UiMinimapHeader (pack/UiFormat.h): minimap textures of a map
    UiStrings = 12,     // UiStringsHeader (pack/UiFormat.h): the interface's texts in one language
    Sound = 13,         // SoundHeader (pack/SoundFormat.h): a sound effect, voice, ambience loop or music track
    GroundSounds = 14,  // GroundSoundsHeader (pack/SoundFormat.h): the footstep surface of a map's terrain
    SkyModel = 15,      // SkyModelHeader (pack/SkyFormat.h): stars and skyboxes, drawn around the camera
    TerrainObjects = 16, // TerrainTileHeader with placements only (pack/TerrainFormat.h): a map's global WMO
    CinematicCamera = 17, // CinematicCameraHeader (pack/CinematicFormat.h): a cinematic's flying camera
    GrassModels = 18,    // phase 14: GrassModelsHeader (pack/GrassFormat.h): detail doodad models and ground effects
    GrassTile = 23,      // phase 14: GrassTileHeader (pack/GrassFormat.h): a terrain tile's ground effect cells
    // Phase 14 (sound): worldsound.vdpk (pack/WorldSoundFormat.h).
    ModelSounds = 19,   // DoodadSoundsHeader: a map's doodads' sound events
    WmoAreas = 20,      // WmoAreasHeader: a map's buildings, their WMOAreaTable ids and floors
    SoundEmitters = 24, // SoundEmittersHeader: a map's terrain sound emitters (MCSE)
    BodyBounds = 22,     // phase 14 (bodyfx): BodyBoundsHeader (pack/BodyFxFormat.h): the units' sequence boxes
    UiMacroIcons = 25,   // phase 15 (options): UiMacroIconsHeader (pack/UiFormat.h): the macro icons
    GlueScene = 26,      // phase 18: GlueSceneHeader (pack/GlueFormat.h): a glue screen's camera and lights
    UiModels = 27,       // retouches (ui models): UiModelsHeader (pack/UiModelFormat.h): the interface's models
};

enum class Compression : u16 {
    None = 0,
    Lz4 = 1, // LZ4 block; PackEntry::rawSize is the decompressed size
};

struct PackHeader {
    char magic[4];
    u32 version;
    u32 bakerVersion;
    u32 clientBuild;
    u64 entriesOffset;
    u32 entryCount;
    u32 flags;
    u64 namesOffset;
    u64 namesSize;
    // Phase 21 (languages): the language of the client data a language-specific pack was baked from ("esES": db.vdpk
    // of a Spanish client, the overlays db.esES.vdpk...), zeros when the pack does not depend on it (older packs too).
    char locale[4];
    u8 reserved[12];
};
static_assert(sizeof(PackHeader) == 64, "PackHeader layout");

struct PackEntry {
    u64 pathHash; // assetPathHash() of the canonical path
    u64 offset;   // from the start of the file
    u32 storedSize;
    u32 rawSize;    // size after decompression (== storedSize when uncompressed)
    u32 nameOffset; // into the name table, or kNoName
    u16 type;       // AssetType
    u16 compression;
};
static_assert(sizeof(PackEntry) == 32, "PackEntry layout");

// --- Texture blob ---------------------------------------------------------------------------------

enum class TextureFormat : u16 {
    Rgba8 = 0,
    Astc4x4 = 1,
    Astc5x5 = 2,
    Astc6x6 = 3,
    Astc8x8 = 4,
    R8 = 5, // one channel (UI font distance fields)
};

constexpr u16 kTextureHasAlpha = 1u << 0;    // alpha channel carries information
constexpr u16 kTextureAlphaCutout = 1u << 1; // 1-bit alpha in the source: alpha-test material
// Phase 11: a terrain texture array whose alpha is the specular mask ("Terrain Highlights": the client's _s textures,
// 0 in the layers without one).
constexpr u16 kTextureSpecularMask = 1u << 2;

constexpr u32 kMaxMips = 16;

struct TextureHeader {
    u16 format; // TextureFormat
    u16 flags;
    u16 width;
    u16 height;
    u16 mipCount;
    u16 layers; // 0 = plain 2D texture; N >= 1 = 2D array of N layers
    // From the start of the blob; 16-byte aligned. For arrays, a mip holds every layer back to back
    // (the layout vkCmdCopyBufferToImage expects) and mipSizes covers all of them.
    u32 mipOffsets[kMaxMips];
    u32 mipSizes[kMaxMips];
};
static_assert(sizeof(TextureHeader) == 140, "TextureHeader layout");

constexpr u32 astcBlockSize(TextureFormat format) {
    switch (format) {
    case TextureFormat::Astc4x4: return 4;
    case TextureFormat::Astc5x5: return 5;
    case TextureFormat::Astc6x6: return 6;
    case TextureFormat::Astc8x8: return 8;
    default: return 0;
    }
}

} // namespace vd::pack
