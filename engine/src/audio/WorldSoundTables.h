#pragma once

#include "core/Types.h"
#include "pack/AsyncReader.h"
#include "pack/PackReader.h"
#include "pack/WorldSoundFormat.h"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace vd::audio {

// Phase 14: the doodads' sounds and the terrain's emitters of a map, from worldsound.vdpk (pack/WorldSoundFormat.h),
// read and decompressed on their own IO thread when the map changes. Until they arrive (or without the pack) the
// lookups find nothing. Game thread only.
class WorldSoundTables {
public:
    ~WorldSoundTables();

    // Opens <packsDir>/worldsound.vdpk. False without the pack.
    bool init(const std::filesystem::path& packsDir);
    void shutdown();
    // The map whose tables to read (Map.dbc directory, any case; empty: none).
    void setMap(std::string_view map);
    // Once per frame: takes what the IO thread read.
    void update();
    // Changes whenever a map's tables arrive or go (what was found in the last ones is no longer valid).
    u32 generation() const { return m_generation; }

    // The map's doodad sounds (sorted by cell), and the indices of those within `radius` yards of (x, y) into `out`;
    // returns how many (at most out.size(): the first found).
    std::span<const pack::DoodadSound> doodadSounds() const;
    u32 doodadSoundsNear(f64 x, f64 y, f32 radius, std::span<u32> out) const;
    std::span<const pack::SoundEmitter> emitters() const;

    // Tests: the tables from blobs, checked as the reader does.
    bool loadDoodadSounds(std::unique_ptr<u8[]> data, u32 size);
    bool loadEmitters(std::unique_ptr<u8[]> data, u32 size);
    static bool validateDoodadSounds(const u8* data, u32 size);
    static bool validateEmitters(const u8* data, u32 size);

private:
    pack::PackReader m_pack;
    bool m_open = false;
    pack::AsyncReader m_reader;
    bool m_started = false;
    std::string m_map;
    u64 m_request = 0; // per map; the reads' tags are m_request * 2 (doodads) and m_request * 2 + 1 (emitters)
    u32 m_generation = 0;
    std::unique_ptr<u8[]> m_doodads;
    u32 m_doodadsSize = 0;
    std::unique_ptr<u8[]> m_emitters;
    u32 m_emittersSize = 0;
};

} // namespace vd::audio
