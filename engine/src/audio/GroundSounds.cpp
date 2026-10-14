#include "audio/GroundSounds.h"

#include "core/Hash.h"
#include "core/Log.h"
#include "pack/PackReader.h"
#include "pack/SoundFormat.h"
#include "pack/TerrainFormat.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vd::audio {

GroundSounds::~GroundSounds() {
    m_reader.stop();
}

bool GroundSounds::validate(const u8* data, u32 size) {
    if (!data || size < sizeof(pack::GroundSoundsHeader)) {
        return false;
    }
    pack::GroundSoundsHeader header;
    std::memcpy(&header, data, sizeof(header));
    if (header.version != pack::kGroundSoundsVersion || header.tileCount > 64 * 64 ||
        size != sizeof(header) + static_cast<u64>(header.tileCount) * pack::kGroundTileBytes) {
        return false;
    }
    for (const u16 slot : header.tiles) {
        if (slot != pack::kNoGroundTile && slot >= header.tileCount) {
            return false;
        }
    }
    return true;
}

void GroundSounds::setMap(const pack::PackReader& sounds, std::string_view map) {
    // Called every frame: the same map costs a comparison, nothing else.
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    if (map.size() == m_map.size() &&
        std::equal(map.begin(), map.end(), m_map.begin(), [&](char a, char b) { return lower(a) == b; })) {
        return;
    }
    m_map.assign(map);
    std::transform(m_map.begin(), m_map.end(), m_map.begin(), lower);
    m_data.reset();
    m_size = 0;
    ++m_request; // a read still in flight for the last map is dropped when it arrives
    if (m_map.empty()) {
        return;
    }
    const std::optional<pack::PackAsset> asset = sounds.find("ground/" + m_map);
    if (!asset || asset->type != pack::AssetType::GroundSounds) {
        return; // a map without terrain (instances made of one WMO), or an old pack
    }
    if (!m_started) {
        m_reader.start("vd-ground");
        m_started = true;
    }
    if (!m_reader.request(m_request, *asset, &GroundSounds::validate)) {
        VD_LOG_WARN("Ground sounds of %s: the reader is busy", m_map.c_str());
    }
}

void GroundSounds::update() {
    if (!m_started) {
        return;
    }
    pack::AsyncReader::Done done;
    while (m_reader.poll(done)) {
        if (done.tag == m_request && done.data) {
            m_data.reset(done.data);
            m_size = done.size;
        } else {
            delete[] done.data;
        }
    }
}

u8 GroundSounds::soundAt(f64 x, f64 y) const {
    if (!m_data) {
        return 0;
    }
    // The terrain's addressing (world/Terrain): tile from the map's corner, then (s, t) inside it in vertex units,
    // 8 per chunk; a cell is 8 / kGroundCells of them.
    const f64 fx = 32.0 - y / pack::kTileSize;
    const f64 fy = 32.0 - x / pack::kTileSize;
    if (!(fx >= 0.0 && fy >= 0.0 && fx < 64.0 && fy < 64.0)) {
        return 0;
    }
    const u32 tx = static_cast<u32>(fx);
    const u32 ty = static_cast<u32>(fy);
    pack::GroundSoundsHeader header;
    std::memcpy(&header, m_data.get(), sizeof(header));
    const u16 slot = header.tiles[ty * 64 + tx];
    if (slot == pack::kNoGroundTile) {
        return 0;
    }
    const f64 s = std::clamp((pack::tileCornerY(tx) - y) / pack::kUnitSize, 0.0, 127.999);
    const f64 t = std::clamp((pack::tileCornerX(ty) - x) / pack::kUnitSize, 0.0, 127.999);
    const u32 column = static_cast<u32>(s) / 8;
    const u32 row = static_cast<u32>(t) / 8;
    constexpr f64 kCellUnits = 8.0 / pack::kGroundCells;
    const u32 cellColumn = static_cast<u32>(std::fmod(s, 8.0) / kCellUnits);
    const u32 cellRow = static_cast<u32>(std::fmod(t, 8.0) / kCellUnits);
    const usize offset = sizeof(header) + static_cast<usize>(slot) * pack::kGroundTileBytes +
                         (row * 16 + column) * pack::kGroundCells * pack::kGroundCells +
                         std::min(cellRow, pack::kGroundCells - 1) * pack::kGroundCells +
                         std::min(cellColumn, pack::kGroundCells - 1);
    return offset < m_size ? m_data[offset] : 0;
}

} // namespace vd::audio
