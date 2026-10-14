#include "audio/WorldSoundTables.h"

#include "core/Log.h"
#include "core/Profile.h"

#include <algorithm>
#include <cstring>

namespace vd::audio {

WorldSoundTables::~WorldSoundTables() {
    shutdown();
}

bool WorldSoundTables::init(const std::filesystem::path& packsDir) {
    shutdown();
    if (Result<void> opened = m_pack.open(packsDir / pack::kWorldSoundPack); !opened) {
        VD_LOG_INFO("No %s (vd_baker bake --only worldsound): doodads and the terrain make no sounds",
                    pack::kWorldSoundPack);
        return false;
    }
    m_open = true;
    return true;
}

void WorldSoundTables::shutdown() {
    if (m_started) {
        m_reader.stop();
        m_started = false;
    }
    m_doodads.reset();
    m_emitters.reset();
    m_doodadsSize = m_emittersSize = 0;
    m_map.clear();
    if (m_open) {
        m_pack.close();
        m_open = false;
    }
}

bool WorldSoundTables::validateDoodadSounds(const u8* data, u32 size) {
    if (!data || size < sizeof(pack::DoodadSoundsHeader)) {
        return false;
    }
    pack::DoodadSoundsHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.version != pack::kWorldSoundVersion || size != sizeof(h) + u64{h.count} * sizeof(pack::DoodadSound)) {
        return false;
    }
    const auto* records = reinterpret_cast<const pack::DoodadSound*>(data + sizeof(h));
    for (u32 i = 1; i < h.count; ++i) {
        if (records[i - 1].cell > records[i].cell) {
            return false; // not sorted by cell: the queries would miss
        }
    }
    return true;
}

bool WorldSoundTables::validateEmitters(const u8* data, u32 size) {
    if (!data || size < sizeof(pack::SoundEmittersHeader)) {
        return false;
    }
    pack::SoundEmittersHeader h;
    std::memcpy(&h, data, sizeof(h));
    return h.version == pack::kWorldSoundVersion && size == sizeof(h) + u64{h.count} * sizeof(pack::SoundEmitter);
}

bool WorldSoundTables::loadDoodadSounds(std::unique_ptr<u8[]> data, u32 size) {
    if (!validateDoodadSounds(data.get(), size)) {
        return false;
    }
    m_doodads = std::move(data);
    m_doodadsSize = size;
    ++m_generation;
    return true;
}

bool WorldSoundTables::loadEmitters(std::unique_ptr<u8[]> data, u32 size) {
    if (!validateEmitters(data.get(), size)) {
        return false;
    }
    m_emitters = std::move(data);
    m_emittersSize = size;
    ++m_generation;
    return true;
}

void WorldSoundTables::setMap(std::string_view map) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    if (map.size() == m_map.size() &&
        std::equal(map.begin(), map.end(), m_map.begin(), [&](char a, char b) { return lower(a) == b; })) {
        return;
    }
    m_map.assign(map);
    std::transform(m_map.begin(), m_map.end(), m_map.begin(), lower);
    m_doodads.reset();
    m_emitters.reset();
    m_doodadsSize = m_emittersSize = 0;
    ++m_generation;
    ++m_request; // reads in flight for the last map are dropped when they arrive
    if (m_map.empty() || !m_open) {
        return;
    }
    const std::optional<pack::PackAsset> doodads = m_pack.find(pack::modelSoundsPath(m_map));
    const std::optional<pack::PackAsset> emitters = m_pack.find(pack::soundEmittersPath(m_map));
    const bool hasDoodads = doodads && doodads->type == pack::AssetType::ModelSounds;
    const bool hasEmitters = emitters && emitters->type == pack::AssetType::SoundEmitters;
    if (!hasDoodads && !hasEmitters) {
        return; // a map without sounding doodads nor emitters
    }
    if (!m_started) {
        m_reader.start("vd-worldsound");
        m_started = true;
    }
    if (hasDoodads && !m_reader.request(m_request * 2, *doodads, &WorldSoundTables::validateDoodadSounds)) {
        VD_LOG_WARN("Doodad sounds of %s: the reader is busy", m_map.c_str());
    }
    if (hasEmitters && !m_reader.request(m_request * 2 + 1, *emitters, &WorldSoundTables::validateEmitters)) {
        VD_LOG_WARN("Sound emitters of %s: the reader is busy", m_map.c_str());
    }
}

void WorldSoundTables::update() {
    if (!m_started) {
        return;
    }
    pack::AsyncReader::Done done;
    while (m_reader.poll(done)) {
        if (done.tag == m_request * 2 && done.data) {
            m_doodads.reset(done.data);
            m_doodadsSize = done.size;
            ++m_generation;
        } else if (done.tag == m_request * 2 + 1 && done.data) {
            m_emitters.reset(done.data);
            m_emittersSize = done.size;
            ++m_generation;
        } else {
            delete[] done.data;
        }
    }
}

std::span<const pack::DoodadSound> WorldSoundTables::doodadSounds() const {
    if (!m_doodads) {
        return {};
    }
    pack::DoodadSoundsHeader h;
    std::memcpy(&h, m_doodads.get(), sizeof(h));
    return {reinterpret_cast<const pack::DoodadSound*>(m_doodads.get() + sizeof(h)), h.count};
}

u32 WorldSoundTables::doodadSoundsNear(f64 x, f64 y, f32 radius, std::span<u32> out) const {
    const std::span<const pack::DoodadSound> records = doodadSounds();
    if (records.empty() || out.empty()) {
        return 0;
    }
    VD_PROFILE_ZONE("WorldSoundTables::doodadSoundsNear");
    // The cells of the square around the point (doodadSoundCell's axes: rows from X, columns from Y), each row's run
    // found by binary search in the sorted records.
    const u32 first = pack::doodadSoundCell(x + radius, y + radius);
    const u32 last = pack::doodadSoundCell(x - radius, y - radius);
    const u32 row0 = first / pack::kDoodadSoundCells, row1 = last / pack::kDoodadSoundCells;
    const u32 column0 = first % pack::kDoodadSoundCells, column1 = last % pack::kDoodadSoundCells;
    const f64 radius2 = static_cast<f64>(radius) * radius;
    u32 count = 0;
    for (u32 row = row0; row <= row1; ++row) {
        const u32 from = row * pack::kDoodadSoundCells + column0;
        const u32 to = row * pack::kDoodadSoundCells + column1;
        const auto begin = std::lower_bound(records.begin(), records.end(), from,
                                            [](const pack::DoodadSound& r, u32 cell) { return r.cell < cell; });
        for (auto it = begin; it != records.end() && it->cell <= to; ++it) {
            const f64 dx = static_cast<f64>(it->position[0]) - x, dy = static_cast<f64>(it->position[1]) - y;
            if (dx * dx + dy * dy <= radius2) {
                out[count++] = static_cast<u32>(it - records.begin());
                if (count == out.size()) {
                    return count;
                }
            }
        }
    }
    return count;
}

std::span<const pack::SoundEmitter> WorldSoundTables::emitters() const {
    if (!m_emitters) {
        return {};
    }
    pack::SoundEmittersHeader h;
    std::memcpy(&h, m_emitters.get(), sizeof(h));
    return {reinterpret_cast<const pack::SoundEmitter*>(m_emitters.get() + sizeof(h)), h.count};
}

} // namespace vd::audio
