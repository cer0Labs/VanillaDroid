#pragma once

#include "core/Types.h"
#include "render/Renderer.h"

#include <string>

namespace vd {

namespace pack {
class PackReader;
}

// Debug window to browse a baked texture pack on the device: checks the bake output by eye and
// measures what a texture costs to load (page-in from the mapped pack + GPU upload).
class TextureBrowser {
public:
    void draw(Renderer& renderer, const pack::PackReader& pack);
    void release(Renderer& renderer);

private:
    void load(Renderer& renderer, const pack::PackReader& pack, int index);
    // Loads kBenchmarkSamples textures spread over the pack, twice (cold: pages read from storage;
    // warm: already in the page cache), and reports main-thread load time percentiles.
    void runBenchmark(Renderer& renderer, const pack::PackReader& pack);

    int m_index = 0;
    int m_loadedIndex = -1;
    TextureHandle m_texture;
    TextureInfo m_info;
    u16 m_format = 0;
    u16 m_flags = 0;
    std::string m_name;
    std::string m_error;
    std::string m_benchmark;
};

} // namespace vd
