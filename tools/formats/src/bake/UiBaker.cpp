#include "bake/UiBaker.h"

#include "core/Hash.h"
#include "pack/PackFormat.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <numeric>

// stb_truetype v1.26, the copy Dear ImGui ships (pinned with it): signed distance fields straight from the outlines.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push, 0)
#pragma warning(disable : 4505) // unused static functions of the library
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include <imstb_truetype.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace vd::bake {
namespace {

// Shelves, tallest items first: good enough for a few hundred rectangles of similar heights.
class ShelfPacker {
public:
    explicit ShelfPacker(u32 size) : m_size(size) {}

    bool place(u32 width, u32 height, u32& x, u32& y) {
        for (Shelf& shelf : m_shelves) {
            if (height <= shelf.height && shelf.x + width <= m_size) {
                x = shelf.x;
                y = shelf.y;
                shelf.x += width;
                return true;
            }
        }
        if (m_nextY + height > m_size || width > m_size) {
            return false;
        }
        m_shelves.push_back({m_nextY, height, width});
        x = 0;
        y = m_nextY;
        m_nextY += height;
        return true;
    }

private:
    struct Shelf {
        u32 y;
        u32 height;
        u32 x;
    };
    u32 m_size;
    u32 m_nextY = 0;
    std::vector<Shelf> m_shelves;
};

constexpr u32 align4(u32 v) {
    return (v + 3u) & ~3u;
}

} // namespace

Result<UiAtlas> packUiAtlas(std::span<const UiSpriteInput> inputs, u32 pageSize) {
    constexpr u32 g = pack::kUiAtlasGutter;
    std::vector<u32> order(inputs.size());
    std::iota(order.begin(), order.end(), 0u);
    // Stable: equal sizes keep the inputs' order, the same atlas on every standard library (phase 22).
    std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        const formats::BlpImage& ia = *inputs[a].image;
        const formats::BlpImage& ib = *inputs[b].image;
        return ia.height != ib.height ? ia.height > ib.height : ia.width > ib.width;
    });

    UiAtlas atlas;
    atlas.pageSize = pageSize;
    std::vector<ShelfPacker> packers;
    u64 used = 0;
    for (u32 index : order) {
        const formats::BlpImage& image = *inputs[index].image;
        if (image.mips.empty() || image.width == 0 || image.height == 0) {
            return Error{"UI atlas: empty texture " + inputs[index].path};
        }
        const u32 slotW = align4(image.width) + 2 * g;
        const u32 slotH = align4(image.height) + 2 * g;
        if (slotW > pageSize || slotH > pageSize) {
            return Error{"UI atlas: texture too large for a page: " + inputs[index].path};
        }
        u32 page = 0, x = 0, y = 0;
        for (; page < packers.size(); ++page) {
            if (packers[page].place(slotW, slotH, x, y)) {
                break;
            }
        }
        if (page == packers.size()) {
            packers.emplace_back(pageSize);
            atlas.pages.emplace_back(usize{pageSize} * pageSize * 4, u8{0});
            if (!packers.back().place(slotW, slotH, x, y)) {
                return Error{"UI atlas: cannot place " + inputs[index].path};
            }
        }
        used += u64{slotW} * slotH;
        // The slot: the texture with its edges extended into the gutter and the 4-alignment padding.
        u8* dst = atlas.pages[page].data();
        const u8* src = image.mips[0].data();
        for (u32 sy = 0; sy < slotH; ++sy) {
            const u32 ty = static_cast<u32>(std::clamp<i32>(static_cast<i32>(sy) - static_cast<i32>(g), 0,
                                                            static_cast<i32>(image.height) - 1));
            for (u32 sx = 0; sx < slotW; ++sx) {
                const u32 tx = static_cast<u32>(std::clamp<i32>(static_cast<i32>(sx) - static_cast<i32>(g), 0,
                                                                static_cast<i32>(image.width) - 1));
                std::memcpy(dst + ((usize{y} + sy) * pageSize + x + sx) * 4, src + (usize{ty} * image.width + tx) * 4,
                            4);
            }
        }
        pack::UiSprite sprite{};
        sprite.pathHash = assetPathHash(inputs[index].path);
        sprite.page = static_cast<u16>(page);
        sprite.x = static_cast<u16>(x + g);
        sprite.y = static_cast<u16>(y + g);
        sprite.width = static_cast<u16>(image.width);
        sprite.height = static_cast<u16>(image.height);
        sprite.flags = image.alphaDepth > 0 ? pack::kTextureHasAlpha : 0;
        atlas.sprites.push_back(sprite);
    }
    std::sort(atlas.sprites.begin(), atlas.sprites.end(),
              [](const pack::UiSprite& a, const pack::UiSprite& b) { return a.pathHash < b.pathHash; });
    for (usize i = 1; i < atlas.sprites.size(); ++i) {
        if (atlas.sprites[i].pathHash == atlas.sprites[i - 1].pathHash) {
            return Error{"UI atlas: the same texture twice"};
        }
    }
    atlas.fill = atlas.pages.empty()
                     ? 0.0f
                     : static_cast<f32>(static_cast<f64>(used) /
                                        (static_cast<f64>(pageSize) * pageSize * static_cast<f64>(atlas.pages.size())));
    return atlas;
}

std::vector<u32> uiCharacterSet() {
    std::vector<u32> set;
    for (u32 c = 0x20; c <= 0x7E; ++c) {
        set.push_back(c);
    }
    for (u32 c = 0xA0; c <= 0x17F; ++c) { // Latin-1 supplement + Latin Extended-A
        set.push_back(c);
    }
    for (u32 c : {0x2013u, 0x2014u, 0x2018u, 0x2019u, 0x201Cu, 0x201Du, 0x2022u, 0x2026u, 0x20ACu, 0x2122u}) {
        set.push_back(c);
    }
    return set;
}

Result<UiFontBake> bakeUiFonts(std::span<const UiFontInput> fonts, std::span<const u32> charset) {
    constexpr u32 kSize = pack::kUiFontAtlasSize;
    UiFontBake bake;
    std::vector<ShelfPacker> packers;
    auto newLayer = [&] {
        packers.emplace_back(kSize);
        bake.layers.emplace_back(usize{kSize} * kSize, u8{0});
    };
    for (const UiFontInput& input : fonts) {
        stbtt_fontinfo info;
        if (input.ttf.empty() ||
            !stbtt_InitFont(&info, input.ttf.data(), stbtt_GetFontOffsetForIndex(input.ttf.data(), 0))) {
            return Error{"UI font: not a TrueType font: " + input.name};
        }
        const f32 scale = stbtt_ScaleForPixelHeight(&info, input.pixelHeight);
        i32 ascent = 0, descent = 0, lineGap = 0;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);

        struct Rendered {
            u32 codepoint;
            i32 glyph;
            i32 width, height, offsetX, offsetY;
            f32 advance;
            u8* sdf;
        };
        std::vector<Rendered> rendered;
        for (u32 codepoint : charset) {
            const i32 glyph = stbtt_FindGlyphIndex(&info, static_cast<i32>(codepoint));
            if (glyph == 0 && codepoint != ' ') {
                continue; // not in this font
            }
            i32 advance = 0, bearing = 0;
            stbtt_GetGlyphHMetrics(&info, glyph, &advance, &bearing);
            Rendered r{codepoint, glyph, 0, 0, 0, 0, static_cast<f32>(advance) * scale, nullptr};
            const i32 padding = static_cast<i32>(std::ceil(input.sdfRange));
            r.sdf = stbtt_GetGlyphSDF(&info, scale, glyph, padding, 128, 128.0f / input.sdfRange, &r.width, &r.height,
                                      &r.offsetX, &r.offsetY);
            if (!r.sdf) {
                r.width = r.height = 0; // blank (space)
            }
            rendered.push_back(r);
        }
        // Tallest first into the shared layers (a layer can hold glyphs of several fonts).
        std::vector<u32> order(rendered.size());
        std::iota(order.begin(), order.end(), 0u);
        // Stable (phase 22): the same layers on every standard library.
        std::stable_sort(order.begin(), order.end(),
                         [&](u32 a, u32 b) { return rendered[a].height > rendered[b].height; });
        std::vector<pack::UiGlyph> glyphs(rendered.size());
        for (u32 index : order) {
            const Rendered& r = rendered[index];
            pack::UiGlyph& glyph = glyphs[index];
            glyph.codepoint = r.codepoint;
            glyph.advance = r.advance;
            glyph.offsetX = static_cast<i16>(r.offsetX);
            glyph.offsetY = static_cast<i16>(r.offsetY);
            glyph.width = static_cast<u16>(r.width);
            glyph.height = static_cast<u16>(r.height);
            if (r.width == 0) {
                continue;
            }
            u32 layer = 0, x = 0, y = 0;
            for (; layer < packers.size(); ++layer) {
                if (packers[layer].place(static_cast<u32>(r.width) + 1, static_cast<u32>(r.height) + 1, x, y)) {
                    break;
                }
            }
            if (layer == packers.size()) {
                newLayer();
                if (!packers.back().place(static_cast<u32>(r.width) + 1, static_cast<u32>(r.height) + 1, x, y)) {
                    return Error{"UI font: glyph too large for the atlas"};
                }
            }
            u8* dst = bake.layers[layer].data();
            for (i32 row = 0; row < r.height; ++row) {
                std::memcpy(dst + (usize{y} + static_cast<usize>(row)) * kSize + x, r.sdf + row * r.width,
                            static_cast<usize>(r.width));
            }
            glyph.layer = static_cast<u16>(layer);
            glyph.x = static_cast<u16>(x);
            glyph.y = static_cast<u16>(y);
        }
        for (Rendered& r : rendered) {
            stbtt_FreeSDF(r.sdf, nullptr);
        }
        std::sort(glyphs.begin(), glyphs.end(),
                  [](const pack::UiGlyph& a, const pack::UiGlyph& b) { return a.codepoint < b.codepoint; });

        // Kerning of every pair of baked characters the font adjusts ('kern' or GPOS).
        std::vector<pack::UiKerning> kerning;
        for (const Rendered& left : rendered) {
            for (const Rendered& right : rendered) {
                const i32 adjust = stbtt_GetGlyphKernAdvance(&info, left.glyph, right.glyph);
                if (adjust != 0) {
                    kerning.push_back({(u64{left.codepoint} << 32) | right.codepoint,
                                       static_cast<f32>(adjust) * scale, 0});
                }
            }
        }
        std::sort(kerning.begin(), kerning.end(),
                  [](const pack::UiKerning& a, const pack::UiKerning& b) { return a.pair < b.pair; });

        pack::UiFontHeader header{};
        header.glyphCount = static_cast<u32>(glyphs.size());
        header.kerningCount = static_cast<u32>(kerning.size());
        header.pixelHeight = input.pixelHeight;
        header.ascent = static_cast<f32>(ascent) * scale;
        header.descent = static_cast<f32>(-descent) * scale;
        header.lineGap = static_cast<f32>(lineGap) * scale;
        header.sdfRange = input.sdfRange;
        header.fallback = '?';
        std::vector<u8> blob(sizeof(header) + glyphs.size() * sizeof(pack::UiGlyph) +
                             kerning.size() * sizeof(pack::UiKerning));
        std::memcpy(blob.data(), &header, sizeof(header));
        std::memcpy(blob.data() + sizeof(header), glyphs.data(), glyphs.size() * sizeof(pack::UiGlyph));
        if (!kerning.empty()) {
            std::memcpy(blob.data() + sizeof(header) + glyphs.size() * sizeof(pack::UiGlyph), kerning.data(),
                        kerning.size() * sizeof(pack::UiKerning));
        }
        bake.fonts.emplace_back("ui/font/" + input.name, std::move(blob));
    }
    return bake;
}

std::vector<u8> buildR8Array(const std::vector<std::vector<u8>>& layers, u32 size) {
    pack::TextureHeader header{};
    header.format = static_cast<u16>(pack::TextureFormat::R8);
    header.width = static_cast<u16>(size);
    header.height = static_cast<u16>(size);
    header.mipCount = 1;
    header.layers = static_cast<u16>(layers.size());
    const usize headerSize = (sizeof(header) + 15) & ~usize{15};
    header.mipOffsets[0] = static_cast<u32>(headerSize);
    header.mipSizes[0] = static_cast<u32>(usize{size} * size * layers.size());
    std::vector<u8> blob(headerSize + header.mipSizes[0], u8{0});
    std::memcpy(blob.data(), &header, sizeof(header));
    for (usize i = 0; i < layers.size(); ++i) {
        std::memcpy(blob.data() + headerSize + i * size * size, layers[i].data(), usize{size} * size);
    }
    return blob;
}

Result<std::vector<u8>> buildStringTable(std::span<const UiText> texts) {
    struct Sorted {
        u64 hash;
        const UiText* text;
    };
    std::vector<Sorted> sorted;
    sorted.reserve(texts.size());
    for (const UiText& t : texts) {
        sorted.push_back({stringHash(t.key), &t});
    }
    std::sort(sorted.begin(), sorted.end(), [](const Sorted& a, const Sorted& b) { return a.hash < b.hash; });
    for (usize i = 1; i < sorted.size(); ++i) {
        if (sorted[i].hash == sorted[i - 1].hash) {
            return Error{"text keys with the same hash: " + sorted[i - 1].text->key + " and " + sorted[i].text->key};
        }
    }
    pack::UiStringsHeader header{};
    header.count = static_cast<u32>(sorted.size());
    header.textOffset = static_cast<u32>(sizeof(header) + sorted.size() * sizeof(pack::UiString));
    std::vector<pack::UiString> entries;
    entries.reserve(sorted.size());
    std::string block;
    for (const Sorted& s : sorted) {
        entries.push_back({s.hash, static_cast<u32>(block.size()), static_cast<u32>(s.text->text.size())});
        block += s.text->text;
        block += '\0';
    }
    header.textSize = static_cast<u32>(block.size());
    std::vector<u8> blob(header.textOffset + block.size());
    std::memcpy(blob.data(), &header, sizeof(header));
    std::memcpy(blob.data() + sizeof(header), entries.data(), entries.size() * sizeof(pack::UiString));
    std::memcpy(blob.data() + header.textOffset, block.data(), block.size());
    return blob;
}

// --- Our own art (phase 20) ---------------------------------------------------------------------------------------

namespace {

// One element of an outline: STBTT_vmove / vline / vcurve (quadratic) / vcubic, in the SVG's own units.
struct SvgVertex {
    u8 type = STBTT_vmove;
    f32 x = 0.0f, y = 0.0f;   // the end point
    f32 cx = 0.0f, cy = 0.0f; // the (first) control point
    f32 cx1 = 0.0f, cy1 = 0.0f; // a cubic's second control point
};

// The numbers and commands of a path's "d" (SVG 1.1 §8.3.9: separators optional, "-4-33.61" and ".49.5" are two
// numbers each, an arc's flags may run into the next number).
class SvgPathReader {
public:
    explicit SvgPathReader(std::string_view d) : m_d(d) {}

    void skip() {
        while (m_i < m_d.size() && (std::isspace(static_cast<unsigned char>(m_d[m_i])) || m_d[m_i] == ',')) {
            ++m_i;
        }
    }
    bool end() {
        skip();
        return m_i >= m_d.size();
    }
    // The next command letter, if the next token is one.
    bool command(char& out) {
        skip();
        if (m_i < m_d.size() && std::isalpha(static_cast<unsigned char>(m_d[m_i])) && m_d[m_i] != 'e' &&
            m_d[m_i] != 'E') {
            out = m_d[m_i++];
            return true;
        }
        return false;
    }
    bool number(f32& out) {
        skip();
        const usize start = m_i;
        if (m_i < m_d.size() && (m_d[m_i] == '-' || m_d[m_i] == '+')) {
            ++m_i;
        }
        bool dot = false, digits = false;
        while (m_i < m_d.size()) {
            const char c = m_d[m_i];
            if (c >= '0' && c <= '9') {
                digits = true;
                ++m_i;
            } else if (c == '.' && !dot) {
                dot = true;
                ++m_i;
            } else {
                break;
            }
        }
        if (digits && m_i < m_d.size() && (m_d[m_i] == 'e' || m_d[m_i] == 'E')) {
            const usize mark = m_i++;
            if (m_i < m_d.size() && (m_d[m_i] == '-' || m_d[m_i] == '+')) {
                ++m_i;
            }
            if (m_i < m_d.size() && m_d[m_i] >= '0' && m_d[m_i] <= '9') {
                while (m_i < m_d.size() && m_d[m_i] >= '0' && m_d[m_i] <= '9') {
                    ++m_i;
                }
            } else {
                m_i = mark;
            }
        }
        if (!digits) {
            m_i = start;
            return false;
        }
        const std::string text(m_d.substr(start, m_i - start));
        out = std::strtof(text.c_str(), nullptr);
        return true;
    }
    bool flag(bool& out) {
        skip();
        if (m_i < m_d.size() && (m_d[m_i] == '0' || m_d[m_i] == '1')) {
            out = m_d[m_i++] == '1';
            return true;
        }
        return false;
    }
    bool numberNext() {
        skip();
        return m_i < m_d.size() &&
               ((m_d[m_i] >= '0' && m_d[m_i] <= '9') || m_d[m_i] == '-' || m_d[m_i] == '+' || m_d[m_i] == '.');
    }

private:
    std::string_view m_d;
    usize m_i = 0;
};

// An elliptical arc from (x1, y1) to (x2, y2) as cubics (SVG 1.1 F.6.5: the endpoint form to the center form, then
// quarter turns at most per cubic).
void svgArc(std::vector<SvgVertex>& out, f32 x1, f32 y1, f32 rx, f32 ry, f32 degrees, bool large, bool sweep, f32 x2,
            f32 y2) {
    rx = std::fabs(rx);
    ry = std::fabs(ry);
    if (rx < 1e-6f || ry < 1e-6f || (x1 == x2 && y1 == y2)) {
        out.push_back({STBTT_vline, x2, y2});
        return;
    }
    const f32 phi = degrees * 3.14159265f / 180.0f;
    const f32 cs = std::cos(phi), sn = std::sin(phi);
    const f32 dx = (x1 - x2) * 0.5f, dy = (y1 - y2) * 0.5f;
    const f32 x1p = cs * dx + sn * dy, y1p = -sn * dx + cs * dy;
    const f32 lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.0f) {
        rx *= std::sqrt(lambda);
        ry *= std::sqrt(lambda);
    }
    const f32 num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    const f32 den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    const f32 coef = (large == sweep ? -1.0f : 1.0f) * std::sqrt(std::max(0.0f, den > 0.0f ? num / den : 0.0f));
    const f32 cxp = coef * rx * y1p / ry, cyp = -coef * ry * x1p / rx;
    const f32 cx = cs * cxp - sn * cyp + (x1 + x2) * 0.5f, cy = sn * cxp + cs * cyp + (y1 + y2) * 0.5f;
    const auto angle = [](f32 ux, f32 uy, f32 vx, f32 vy) { return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy); };
    const f32 ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
    const f32 vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
    const f32 theta = angle(1.0f, 0.0f, ux, uy);
    f32 delta = angle(ux, uy, vx, vy);
    constexpr f32 kTurn = 2.0f * 3.14159265f;
    if (!sweep && delta > 0.0f) {
        delta -= kTurn;
    } else if (sweep && delta < 0.0f) {
        delta += kTurn;
    }
    const u32 segments = std::max(1u, static_cast<u32>(std::ceil(std::fabs(delta) / (kTurn * 0.25f) - 1e-4f)));
    const f32 step = delta / static_cast<f32>(segments);
    const f32 t = 4.0f / 3.0f * std::tan(step * 0.25f);
    const auto point = [&](f32 a, f32& px, f32& py) {
        px = cx + rx * std::cos(a) * cs - ry * std::sin(a) * sn;
        py = cy + rx * std::cos(a) * sn + ry * std::sin(a) * cs;
    };
    const auto tangent = [&](f32 a, f32& tx, f32& ty) {
        tx = -rx * std::sin(a) * cs - ry * std::cos(a) * sn;
        ty = -rx * std::sin(a) * sn + ry * std::cos(a) * cs;
    };
    for (u32 i = 0; i < segments; ++i) {
        const f32 a0 = theta + step * static_cast<f32>(i), a1 = a0 + step;
        f32 p0x, p0y, p1x, p1y, t0x, t0y, t1x, t1y;
        point(a0, p0x, p0y);
        point(a1, p1x, p1y);
        tangent(a0, t0x, t0y);
        tangent(a1, t1x, t1y);
        if (i + 1 == segments) {
            p1x = x2;
            p1y = y2;
        }
        out.push_back({STBTT_vcubic, p1x, p1y, p0x + t * t0x, p0y + t * t0y, p1x - t * t1x, p1y - t * t1y});
    }
}

// A path's "d" as outlines (every subpath closed by the rasterizer).
Result<void> svgPath(std::string_view d, std::vector<SvgVertex>& out) {
    SvgPathReader r(d);
    f32 x = 0.0f, y = 0.0f, startX = 0.0f, startY = 0.0f;
    f32 lastCx = 0.0f, lastCy = 0.0f; // the last control point (S and T reflect it)
    char last = 0;
    char cmd = 0;
    while (!r.end()) {
        char next = 0;
        if (r.command(next)) {
            cmd = next;
        } else if (cmd == 0 || !r.numberNext()) {
            return Error{"SVG path: unexpected text"};
        } else if (cmd == 'M') {
            cmd = 'L'; // more pairs after a moveto are linetos
        } else if (cmd == 'm') {
            cmd = 'l';
        }
        const bool rel = std::islower(static_cast<unsigned char>(cmd)) != 0;
        const f32 ox = rel ? x : 0.0f, oy = rel ? y : 0.0f;
        f32 a[7] = {};
        const auto read = [&](u32 n) {
            for (u32 i = 0; i < n; ++i) {
                if (!r.number(a[i])) {
                    return false;
                }
            }
            return true;
        };
        switch (std::toupper(static_cast<unsigned char>(cmd))) {
        case 'Z':
            x = startX;
            y = startY;
            last = 'Z';
            continue;
        case 'M':
            if (!read(2)) {
                return Error{"SVG path: M without its point"};
            }
            x = ox + a[0];
            y = oy + a[1];
            startX = x;
            startY = y;
            out.push_back({STBTT_vmove, x, y});
            break;
        case 'L':
            if (!read(2)) {
                return Error{"SVG path: L without its point"};
            }
            x = ox + a[0];
            y = oy + a[1];
            out.push_back({STBTT_vline, x, y});
            break;
        case 'H':
            if (!read(1)) {
                return Error{"SVG path: H without its x"};
            }
            x = ox + a[0];
            out.push_back({STBTT_vline, x, y});
            break;
        case 'V':
            if (!read(1)) {
                return Error{"SVG path: V without its y"};
            }
            y = oy + a[0];
            out.push_back({STBTT_vline, x, y});
            break;
        case 'C':
            if (!read(6)) {
                return Error{"SVG path: C without its points"};
            }
            out.push_back({STBTT_vcubic, ox + a[4], oy + a[5], ox + a[0], oy + a[1], ox + a[2], oy + a[3]});
            lastCx = ox + a[2];
            lastCy = oy + a[3];
            x = ox + a[4];
            y = oy + a[5];
            break;
        case 'S': {
            if (!read(4)) {
                return Error{"SVG path: S without its points"};
            }
            const bool cubicBefore = last == 'C' || last == 'S';
            const f32 c1x = cubicBefore ? 2.0f * x - lastCx : x, c1y = cubicBefore ? 2.0f * y - lastCy : y;
            out.push_back({STBTT_vcubic, ox + a[2], oy + a[3], c1x, c1y, ox + a[0], oy + a[1]});
            lastCx = ox + a[0];
            lastCy = oy + a[1];
            x = ox + a[2];
            y = oy + a[3];
            break;
        }
        case 'Q':
            if (!read(4)) {
                return Error{"SVG path: Q without its points"};
            }
            out.push_back({STBTT_vcurve, ox + a[2], oy + a[3], ox + a[0], oy + a[1]});
            lastCx = ox + a[0];
            lastCy = oy + a[1];
            x = ox + a[2];
            y = oy + a[3];
            break;
        case 'T': {
            if (!read(2)) {
                return Error{"SVG path: T without its point"};
            }
            const bool quadBefore = last == 'Q' || last == 'T';
            lastCx = quadBefore ? 2.0f * x - lastCx : x;
            lastCy = quadBefore ? 2.0f * y - lastCy : y;
            x = ox + a[0];
            y = oy + a[1];
            out.push_back({STBTT_vcurve, x, y, lastCx, lastCy});
            break;
        }
        case 'A': {
            bool large = false, sweep = false;
            if (!r.number(a[0]) || !r.number(a[1]) || !r.number(a[2]) || !r.flag(large) || !r.flag(sweep) ||
                !r.number(a[3]) || !r.number(a[4])) {
                return Error{"SVG path: A without its values"};
            }
            const f32 ex = ox + a[3], ey = oy + a[4];
            svgArc(out, x, y, a[0], a[1], a[2], large, sweep, ex, ey);
            x = ex;
            y = ey;
            break;
        }
        default: return Error{std::string("SVG path: unknown command ") + cmd};
        }
        last = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));
    }
    return {};
}

// The value of attribute `name` in a tag's text (`name="..."`, after a space), empty if none.
std::string_view svgAttribute(std::string_view tag, std::string_view name) {
    usize at = 0;
    while ((at = tag.find(name, at)) != std::string_view::npos) {
        const bool alone = at > 0 && std::isspace(static_cast<unsigned char>(tag[at - 1]));
        usize i = at + name.size();
        while (i < tag.size() && std::isspace(static_cast<unsigned char>(tag[i]))) {
            ++i;
        }
        if (alone && i < tag.size() && tag[i] == '=') {
            ++i;
            while (i < tag.size() && std::isspace(static_cast<unsigned char>(tag[i]))) {
                ++i;
            }
            if (i < tag.size() && (tag[i] == '"' || tag[i] == '\'')) {
                const usize end = tag.find(tag[i], i + 1);
                if (end != std::string_view::npos) {
                    return tag.substr(i + 1, end - i - 1);
                }
            }
        }
        at += name.size();
    }
    return {};
}

} // namespace

Result<formats::BlpImage> rasterizeSvgIcon(std::string_view svg, u32 size, f32 margin) {
    if (size == 0 || size > 1024) {
        return Error{"SVG icon: size out of range"};
    }
    if (svg.find("transform") != std::string_view::npos) {
        return Error{"SVG icon: transforms are not supported"};
    }
    const usize svgTag = svg.find("<svg");
    if (svgTag == std::string_view::npos) {
        return Error{"SVG icon: no <svg> element"};
    }
    const std::string_view root = svg.substr(svgTag, svg.find('>', svgTag) - svgTag);
    f32 box[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (const std::string_view viewBox = svgAttribute(root, "viewBox"); !viewBox.empty()) {
        SvgPathReader r(viewBox);
        for (f32& v : box) {
            r.number(v);
        }
    } else {
        SvgPathReader w(svgAttribute(root, "width")), h(svgAttribute(root, "height"));
        w.number(box[2]);
        h.number(box[3]);
    }
    if (box[2] <= 0.0f || box[3] <= 0.0f) {
        return Error{"SVG icon: no viewBox nor size"};
    }
    // Every <path>'s outlines.
    std::vector<SvgVertex> outline;
    for (usize at = svg.find("<path"); at != std::string_view::npos; at = svg.find("<path", at + 5)) {
        const std::string_view tag = svg.substr(at, svg.find('>', at) - at);
        if (svgAttribute(tag, "fill") == "none") {
            continue;
        }
        if (Result<void> path = svgPath(svgAttribute(tag, "d"), outline); !path) {
            return path.error();
        }
    }
    if (outline.empty()) {
        return Error{"SVG icon: no path to fill"};
    }
    for (const char* shape : {"<circle", "<rect", "<polygon", "<ellipse", "<line", "<polyline", "stroke="}) {
        if (svg.find(shape) != std::string_view::npos) {
            return Error{std::string("SVG icon: not supported: ") + shape};
        }
    }
    // The viewBox centered in the square, in sixteenths of a pixel (stb_truetype's vertices are 16-bit integers).
    constexpr f32 kSub = 16.0f;
    const f32 scale = static_cast<f32>(size) * (1.0f - 2.0f * margin) / std::max(box[2], box[3]);
    const f32 offX = (static_cast<f32>(size) - box[2] * scale) * 0.5f - box[0] * scale;
    const f32 offY = (static_cast<f32>(size) - box[3] * scale) * 0.5f - box[1] * scale;
    const auto unit = [&](f32 v, f32 off) {
        return static_cast<stbtt_vertex_type>(std::lround(std::clamp((v * scale + off) * kSub, -32000.0f, 32000.0f)));
    };
    std::vector<stbtt_vertex> vertices(outline.size());
    for (usize i = 0; i < outline.size(); ++i) {
        const SvgVertex& v = outline[i];
        stbtt_vertex& s = vertices[i];
        std::memset(&s, 0, sizeof(s));
        s.type = v.type;
        s.x = unit(v.x, offX);
        s.y = unit(v.y, offY);
        s.cx = unit(v.cx, offX);
        s.cy = unit(v.cy, offY);
        s.cx1 = unit(v.cx1, offX);
        s.cy1 = unit(v.cy1, offY);
    }
    std::vector<u8> coverage(static_cast<usize>(size) * size, 0);
    stbtt__bitmap bitmap;
    bitmap.w = static_cast<int>(size);
    bitmap.h = static_cast<int>(size);
    bitmap.stride = static_cast<int>(size);
    bitmap.pixels = coverage.data();
    stbtt_Rasterize(&bitmap, 0.25f, vertices.data(), static_cast<int>(vertices.size()), 1.0f / kSub, 1.0f / kSub,
                    0.0f, 0.0f, 0, 0, 0, nullptr);
    formats::BlpImage image;
    image.width = size;
    image.height = size;
    image.alphaDepth = 8;
    image.encoding = formats::BlpEncoding::Argb8888;
    std::vector<u8> rgba(coverage.size() * 4);
    for (usize i = 0; i < coverage.size(); ++i) {
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = coverage[i];
    }
    image.mips.push_back(std::move(rgba));
    return image;
}

} // namespace vd::bake
