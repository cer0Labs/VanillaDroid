#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vd::formats {

// World Map Object (WMO v17, 1.12): a root file (materials, groups, portals, doodad sets) plus one file
// per group (<root>_NNN.wmo) with the geometry. PC only, turned into baked models by the baker.
//
// Coordinates are model space with Z up, like M2 models. Instances are placed by ADT MODF / WDT MODF
// entries (see formats/Adt.h, placement axes).

// MOMT flags.
constexpr u32 kWmoMaterialUnlit = 0x1;
constexpr u32 kWmoMaterialUnfogged = 0x2;
constexpr u32 kWmoMaterialTwoSided = 0x4;
constexpr u32 kWmoMaterialExteriorLight = 0x8;
constexpr u32 kWmoMaterialNightGlow = 0x10; // "SIDN": emissive at night
constexpr u32 kWmoMaterialWindow = 0x20;
constexpr u32 kWmoMaterialClampS = 0x40;
constexpr u32 kWmoMaterialClampT = 0x80;

// MOGP / MOGI flags.
constexpr u32 kWmoGroupHasBsp = 0x1;
constexpr u32 kWmoGroupHasVertexColors = 0x4;
constexpr u32 kWmoGroupExterior = 0x8;
constexpr u32 kWmoGroupExteriorLit = 0x40; // lit like the outdoors: the streets of a city WMO
constexpr u32 kWmoGroupUnreachable = 0x80; // no portal leads to it (Stormwind keeps three rough cathedral drafts so)
constexpr u32 kWmoGroupHasLights = 0x200;
constexpr u32 kWmoGroupHasDoodads = 0x800;
constexpr u32 kWmoGroupHasLiquid = 0x1000;
constexpr u32 kWmoGroupInterior = 0x2000;
constexpr u32 kWmoGroupAlwaysDraw = 0x8000; // drawn from outside without portals (a city's streets)

constexpr u32 kWmoGroupOcean = 0x80000; // its water is ocean

// MOHD flags.
constexpr u32 kWmoRootLiquidTypeIsDbcId = 0x4; // MOGP liquid is a LiquidType id (not in 1.12 data)

// MOPY triangle flags.
constexpr u8 kWmoTriangleNoCollision = 0x04;
constexpr u8 kWmoTriangleCollisionOnly = 0xFF; // material id: invisible collision triangle

struct WmoMaterial {
    u32 flags = 0;
    u32 shader = 0;    // 0 diffuse, 1 specular, 2 metal, 3 environment...
    u32 blendMode = 0; // 0 opaque, 1 alpha key, 2 alpha, 3 additive...
    std::string texture1;
    std::string texture2; // environment map for shader 3, often empty
    u32 emissiveColor = 0; // BGRA (SIDN)
    u32 diffuseColor = 0;  // BGRA
};

struct WmoGroupInfo {
    u32 flags = 0;
    std::array<f32, 3> boundsMin{};
    std::array<f32, 3> boundsMax{};
};

struct WmoPortal {
    std::vector<std::array<f32, 3>> vertices;
    std::array<f32, 4> plane{}; // normal xyz, distance
};

struct WmoPortalRef {
    u16 portal = 0;
    u16 group = 0; // the group on the other side
    i16 side = 0;  // which side of the portal plane the owning group is on
};

struct WmoDoodadSet {
    std::string name;
    u32 first = 0;
    u32 count = 0;
};

struct WmoDoodad {
    std::string model; // M2 path (the file names say .mdl/.mdx; the client loads .m2)
    u8 flags = 0;
    std::array<f32, 3> position{};
    std::array<f32, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f}; // quaternion x, y, z, w
    f32 scale = 1.0f;
    u32 color = 0; // BGRA
};

// Phase 11: MOLT, the lights the level designers placed (lanterns, candles, forges). 1.12 bakes them into the
// vertex colors (MOCV) and the doodad colors (MODD); the client also lights models with the nearest ones.
enum class WmoLightType : u8 { Omni = 0, Spot = 1, Directional = 2, Ambient = 3 };

struct WmoLight {
    WmoLightType type = WmoLightType::Omni;
    bool attenuated = false; // useAtten: fades from attenStart to attenEnd (else reaches everywhere)
    u32 color = 0;           // BGRA
    std::array<f32, 3> position{};
    f32 intensity = 1.0f;
    f32 attenStart = 0.0f; // yards
    f32 attenEnd = 0.0f;
};

// Phase 11: MFOG, a fog sphere: inside `innerRadius` of `position` its fog replaces the outdoor one, fading out at
// `outerRadius` (flag kWmoFogInfinite: everywhere in the groups that list it). One set above water, one below.
constexpr u32 kWmoFogInfinite = 0x1;

struct WmoFog {
    u32 flags = 0;
    std::array<f32, 3> position{};
    f32 innerRadius = 0.0f;
    f32 outerRadius = 0.0f;
    f32 end = 0.0f;        // yards
    f32 startScale = 0.25f; // start = end * startScale
    u32 color = 0;          // BGRA
    f32 underwaterEnd = 0.0f;
    f32 underwaterStartScale = 0.0f;
    u32 underwaterColor = 0;
};

struct WmoRoot {
    u32 flags = 0;
    u32 ambientColor = 0; // BGRA
    u32 areaTableId = 0;  // WMOAreaTable
    std::array<f32, 3> boundsMin{};
    std::array<f32, 3> boundsMax{};
    std::vector<WmoMaterial> materials;
    std::vector<WmoGroupInfo> groups;
    std::vector<WmoPortal> portals;
    std::vector<WmoPortalRef> portalRefs;
    std::vector<WmoDoodadSet> doodadSets;
    std::vector<WmoDoodad> doodads;
    std::string skybox;
    std::vector<WmoLight> lights; // phase 11: MOLT
    std::vector<WmoFog> fogs;     // phase 11: MFOG
};

struct WmoBatch {
    u32 firstIndex = 0;
    u32 indexCount = 0;
    u16 minVertex = 0;
    u16 maxVertex = 0;
    u8 flags = 0;
    u8 material = 0;
};

// MLIQ: a height grid of (tilesX + 1) x (tilesY + 1) vertices from `corner` (group space), tiles of
// kWmoLiquidTileSize yards; a tile whose flags & 0xF == 0xF has no liquid.
constexpr f32 kWmoLiquidTileSize = 1600.0f / 3.0f / 128.0f;
constexpr u8 kWmoLiquidTileEmpty = 0x0F;

struct WmoLiquid {
    u32 vertsX = 0;
    u32 vertsY = 0;
    u32 tilesX = 0;
    u32 tilesY = 0;
    std::array<f32, 3> corner{};
    std::vector<f32> heights; // vertsX * vertsY, row-major (y rows of x)
    std::vector<u8> tiles;    // tilesX * tilesY flags
};

// Phase 18 (world): MOBN, a node of the group's BSP tree (the client's CAaBspNode).
struct WmoBspNode {
    u16 flags = 0;          // 0..2: the plane's axis; 0x4: a leaf
    u16 negChild = 0xFFFF;  // below the plane (0xFFFF: none)
    u16 posChild = 0xFFFF;  // above it
    u16 faceCount = 0;      // a leaf's MOBR entries
    u32 firstFace = 0;
    f32 planeDist = 0.0f;
};

struct WmoGroup {
    u32 flags = 0;
    std::array<f32, 3> boundsMin{};
    std::array<f32, 3> boundsMax{};
    u16 portalStart = 0; // into WmoRoot::portalRefs
    u16 portalCount = 0;
    u16 transBatchCount = 0; // batches are ordered: trans, interior, exterior
    u16 interiorBatchCount = 0;
    u16 exteriorBatchCount = 0;
    u32 groupId = 0; // WMOAreaTable
    u32 liquidType = 0; // MOGP: see wmoLiquidKind
    std::vector<u8> triangleFlags;     // MOPY, per triangle
    std::vector<u8> triangleMaterials; // MOPY, per triangle (0xFF = collision only)
    std::vector<u16> indices;          // MOVI
    std::vector<std::array<f32, 3>> positions; // MOVT
    std::vector<std::array<f32, 3>> normals;   // MONR
    std::vector<std::array<f32, 2>> uvs;       // MOTV
    std::vector<u32> colors;                   // MOCV (BGRA), empty if absent
    std::vector<WmoBatch> batches;             // MOBA
    std::vector<u16> doodadRefs;               // MODR: into WmoRoot::doodads
    WmoLiquid liquid;                          // MLIQ, tilesX == 0 if absent
    // Phase 11: the fogs of the group (MOGP, into WmoRoot::fogs; 0 is also the first fog) and its lights (MOLR,
    // into WmoRoot::lights).
    std::array<u8, 4> fogIds{};
    std::vector<u16> lightRefs;
    // Phase 18 (world): the BSP tree (MOBN) and its leaves' faces (MOBR: MOVI triangle indices).
    std::vector<WmoBspNode> bspNodes;
    std::vector<u16> bspFaces;
};

enum class WmoLiquidKind : u8 { Water = 0, Ocean = 1, Magma = 2, Slime = 3, None = 0xFF };

// The group's liquid, resolved like vMaNGOS' vmap extractor (contrib/vmap_extractor/vmapextract/wmo.cpp)
// resolves it for the server's own water checks.
WmoLiquidKind wmoLiquidKind(const WmoRoot& root, const WmoGroup& group);

// A group the 1.12 client never draws: unreachable through the portals and neither outdoors nor "always drawn"
// (the client walks the portals from the outdoor groups and from the group the camera is in).
bool wmoGroupNeverDrawn(u32 groupFlags);

Result<WmoRoot> parseWmoRoot(std::span<const u8> data);
Result<WmoGroup> parseWmoGroup(std::span<const u8> data);

// "X.wmo" -> "X_007.wmo".
std::string wmoGroupPath(std::string_view rootPath, u32 group);
// The client's M2 name for a WMO/ADT model reference (.mdx / .mdl -> .m2).
std::string m2Path(std::string_view reference);

} // namespace vd::formats
