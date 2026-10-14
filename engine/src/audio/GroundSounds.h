#pragma once

#include "core/Types.h"
#include "pack/AsyncReader.h"

#include <memory>
#include <string>
#include <string_view>

namespace vd::pack {
class PackReader;
}

namespace vd::audio {

// What the ground sounds like under the feet (pack/SoundFormat.h, "ground/<map>" in sound.vdpk): one map's table,
// read and decompressed on its own IO thread when the map changes. Until it arrives, and off the map's tiles,
// soundAt() says 0 (unknown). Game thread only.
class GroundSounds {
public:
    ~GroundSounds();

    // Asks for `map`'s table (Map.dbc directory, any case). `sounds` must stay open while it is read. The same map
    // again: nothing. Empty: forgets the table.
    void setMap(const pack::PackReader& sounds, std::string_view map);
    // Once per frame: takes the table when it has been read.
    void update();
    bool ready() const { return m_data != nullptr; }

    // TerrainType.dbc SoundID under world (x, y): 1 dirt, 2 metal, 3 stone, 4 snow, 5 wood, 6 grass, 7 leaves,
    // 8 sand, 9 soggy; 0 when unknown.
    u8 soundAt(f64 x, f64 y) const;

    // Checks a table blob (header, slots, size): the IO thread drops one that does not fit.
    static bool validate(const u8* data, u32 size);

private:
    pack::AsyncReader m_reader;
    bool m_started = false;
    std::string m_map; // lowercase, the one asked for last
    u64 m_request = 0; // tag of the read in flight (or done)
    std::unique_ptr<u8[]> m_data;
    u32 m_size = 0;
};

} // namespace vd::audio
