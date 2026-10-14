#pragma once

#include "core/Hash.h"
#include "core/Types.h"

#include <string>
#include <string_view>

namespace vd::pack {

// Baked collision of game object models (phase 8: transports — ships, zeppelins, elevators — that move with
// their passengers on them): collision_objects.vdpk holds, for every model of GameObjectDisplayInfo.dbc that
// has something to collide with, one CollisionTileHeader blob (pack/CollisionFormat.h, without terrain, tileX
// = tileY = 0) in the model's own space, under the model's path plus ".col":
//   world/wmo/transports/transport_zeppelin/transport_zeppelin.wmo.col
//   world/kalimdor/mulgore/passivedoodads/thunderbluffelevator/elevatorcar.m2.col
// WMOs bring their default doodad set; M2s their bounding triangles (the vmap extractor's rule, as the map
// collision).
constexpr const char* kObjectCollisionPack = "collision_objects.vdpk";

// The model path a display names, as baked: ".mdx" (the client's name) becomes ".m2".
inline std::string objectModelPath(std::string_view displayModel) {
    std::string path = normalizeAssetPath(displayModel);
    const usize dot = path.find_last_of('.');
    if (dot != std::string::npos && path.compare(dot, std::string::npos, ".mdx") == 0) {
        path.replace(dot, std::string::npos, ".m2");
    }
    return path;
}

// Asset path of a model's collision.
inline std::string objectCollisionPath(std::string_view displayModel) {
    return objectModelPath(displayModel) + ".col";
}

} // namespace vd::pack
