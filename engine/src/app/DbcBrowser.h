#pragma once

namespace vd {

namespace pack {
class PackReader;
}

// Debug window listing the DBC tables of a baked pack, with a raw record grid. Each cell is shown as
// string, float or integer by heuristic (or forced), which is how unknown field layouts get decoded.
class DbcBrowser {
public:
    void draw(const pack::PackReader& dbPack);

private:
    int m_table = 0;
    int m_firstRow = 0;
    int m_mode = 0; // 0 auto, 1 integers, 2 floats
};

} // namespace vd
