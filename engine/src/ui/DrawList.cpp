#include "ui/DrawList.h"

#include "core/Hash.h"

#include <algorithm>
#include <cmath>

namespace vd::ui {
namespace {

constexpr u32 params(u32 layer, UiMode mode, u32 extra = 0) {
    return (layer & 4095u) | (static_cast<u32>(mode) << 12) | ((extra & 255u) << 16);
}

// Printable characters of interface text, with the client's escape sequences applied: |cAARRGGBB..|r colors,
// |H..|h[..]|h hyperlinks (their text shown), || a bar; "\n" and $B (quest text) break lines.
struct TextCursor {
    std::string_view s;
    usize i = 0;
    u32 color = 0;
    u32 base = 0;

    // False at the end. `cp` = '\n' for line breaks.
    bool next(u32& cp) {
        while (i < s.size()) {
            const char c = s[i];
            if (c == '|' && i + 1 < s.size()) {
                const char code = s[i + 1];
                if (code == 'c' && i + 10 <= s.size()) {
                    u32 value = 0;
                    bool valid = true;
                    for (usize k = i + 2; k < i + 10; ++k) {
                        const char h = s[k];
                        const u32 digit = h >= '0' && h <= '9'   ? static_cast<u32>(h - '0')
                                          : h >= 'a' && h <= 'f' ? static_cast<u32>(h - 'a' + 10)
                                          : h >= 'A' && h <= 'F' ? static_cast<u32>(h - 'A' + 10)
                                                                 : 99u;
                        valid &= digit < 16;
                        value = (value << 4) | (digit & 15u);
                    }
                    if (valid) { // AARRGGBB -> RGBA8 (the text's alpha is kept)
                        color = rgba(static_cast<u8>(value >> 16), static_cast<u8>(value >> 8), static_cast<u8>(value),
                                     static_cast<u8>(base >> 24));
                        i += 10;
                        continue;
                    }
                } else if (code == 'r') {
                    color = base;
                    i += 2;
                    continue;
                } else if (code == 'H') { // |H<link>|h
                    const usize end = s.find("|h", i + 2);
                    i = end == std::string_view::npos ? s.size() : end + 2;
                    continue;
                } else if (code == 'h') {
                    i += 2;
                    continue;
                } else if (code == '|') {
                    i += 2;
                    cp = '|';
                    return true;
                } else if (code == 'n') {
                    i += 2;
                    cp = '\n';
                    return true;
                }
            }
            if (c == '$' && i + 1 < s.size() && (s[i + 1] == 'B' || s[i + 1] == 'b')) {
                i += 2;
                cp = '\n';
                return true;
            }
            if (c == '\r') {
                ++i;
                continue;
            }
            cp = decodeUtf8(s, i);
            return true;
        }
        return false;
    }
};

} // namespace

u32 decodeUtf8(std::string_view s, usize& i) {
    const u8 c = static_cast<u8>(s[i]);
    if (c < 0x80) {
        ++i;
        return c;
    }
    u32 needed = 0;
    u32 cp = 0;
    if ((c & 0xE0) == 0xC0) {
        needed = 1;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        needed = 2;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        needed = 3;
        cp = c & 0x07;
    } else {
        ++i;
        return 0xFFFD;
    }
    if (i + needed >= s.size()) { // truncated sequence
        i = s.size();
        return 0xFFFD;
    }
    for (u32 k = 1; k <= needed; ++k) {
        const u8 next = static_cast<u8>(s[i + k]);
        if ((next & 0xC0) != 0x80) {
            i += k;
            return 0xFFFD;
        }
        cp = (cp << 6) | (next & 0x3F);
    }
    i += needed + 1;
    return cp;
}

void DrawList::init(const Assets* assets, u32 maxQuads) {
    m_assets = assets;
    m_models.bind(assets); // retouches (ui models): read once the pack is open
    m_maxQuads = maxQuads;
    m_vertices.resize(usize{maxQuads} * 4);
    m_commands.reserve(512);
    reset();
}

void DrawList::reset() {
    m_quadCount = 0;
    m_commands.clear();
    m_clipDepth = 0;
    m_clipChanged = true;
}

void DrawList::pushClip(const Rect& clip) {
    Rect r = clip;
    if (m_clipDepth > 0) {
        const Rect& outer = m_clips[m_clipDepth - 1];
        const f32 x0 = std::max(r.x, outer.x), y0 = std::max(r.y, outer.y);
        const f32 x1 = std::min(r.right(), outer.right()), y1 = std::min(r.bottom(), outer.bottom());
        r = {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
    }
    if (m_clipDepth < m_clips.size()) {
        m_clips[m_clipDepth++] = r;
    }
    m_clipChanged = true;
}

void DrawList::popClip() {
    if (m_clipDepth > 0) {
        --m_clipDepth;
    }
    m_clipChanged = true;
}

UiVertex* DrawList::addQuad(TextureHandle texture, f32 circleX, f32 circleY, f32 circleRadius) {
    if (m_quadCount >= m_maxQuads) {
        return nullptr;
    }
    const bool wantsTexture = texture.valid();
    bool join = !m_commands.empty() && !m_clipChanged;
    if (join) {
        UiDrawCmd& last = m_commands.back();
        if (wantsTexture) {
            join = !last.texture.valid() || last.texture.index == texture.index;
        }
        if (join && circleRadius > 0.0f) {
            join = last.circleRadius == 0.0f ||
                   (last.circleX == circleX && last.circleY == circleY && last.circleRadius == circleRadius);
        }
    }
    if (!join) {
        UiDrawCmd command;
        command.firstQuad = m_quadCount;
        if (m_clipDepth > 0) {
            const Rect& clip = m_clips[m_clipDepth - 1];
            command.clipX = clip.x;
            command.clipY = clip.y;
            command.clipWidth = std::max(clip.w, 0.001f);
            command.clipHeight = std::max(clip.h, 0.001f);
        }
        m_commands.push_back(command);
        m_clipChanged = false;
    }
    UiDrawCmd& command = m_commands.back();
    if (wantsTexture) {
        command.texture = texture;
    }
    if (circleRadius > 0.0f) {
        command.circleX = circleX;
        command.circleY = circleY;
        command.circleRadius = circleRadius;
    }
    ++command.quadCount;
    return &m_vertices[usize{m_quadCount++} * 4];
}

void DrawList::quad(const Rect& r, TexCoords uv, u32 color, u32 p, TextureHandle texture) {
    UiVertex* v = addQuad(texture);
    if (!v) {
        return;
    }
    v[0] = {r.x, r.y, uv.u0, uv.v0, color, p};
    v[1] = {r.x + r.w, r.y, uv.u1, uv.v0, color, p};
    v[2] = {r.x + r.w, r.y + r.h, uv.u1, uv.v1, color, p};
    v[3] = {r.x, r.y + r.h, uv.u0, uv.v1, color, p};
}

void DrawList::solid(const Rect& r, u32 color) {
    quad(r, {}, color, params(0, UiMode::Solid));
}

void DrawList::sprite(const Rect& r, const Sprite& s, u32 color, TexCoords uv) {
    const f32 inv = 1.0f / m_assets->atlasSize();
    const TexCoords atlas{(s.x + uv.u0 * s.width) * inv, (s.y + uv.v0 * s.height) * inv,
                          (s.x + uv.u1 * s.width) * inv, (s.y + uv.v1 * s.height) * inv};
    quad(r, atlas, color, params(s.page, UiMode::Atlas));
}

void DrawList::sprite(const Rect& r, u64 pathHash, u32 color, TexCoords uv) {
    if (const std::optional<Sprite> s = m_assets->sprite(pathHash)) {
        sprite(r, *s, color, uv);
    }
}

void DrawList::spriteAdditive(const Rect& r, u64 pathHash, u32 color, TexCoords uv) {
    const std::optional<Sprite> found = m_assets->sprite(pathHash);
    if (!found) {
        return;
    }
    const Sprite& s = *found;
    const f32 inv = 1.0f / m_assets->atlasSize();
    const TexCoords atlas{(s.x + uv.u0 * s.width) * inv, (s.y + uv.v0 * s.height) * inv,
                          (s.x + uv.u1 * s.width) * inv, (s.y + uv.v1 * s.height) * inv};
    quad(r, atlas, color, params(s.page, UiMode::Atlas) | (1u << 24));
}

void DrawList::icon(const Rect& r, u32 slot, u32 color, TexCoords uv) {
    quad(r, uv, color, params(slot, UiMode::Icon));
}

void DrawList::texture(const Rect& r, TextureHandle texture, u32 color, TexCoords uv) {
    quad(r, uv, color, params(0, UiMode::Texture), texture);
}

void DrawList::textureInCircle(const Rect& r, TextureHandle texture, f32 cx, f32 cy, f32 radius, TexCoords uv,
                               u32 color) {
    UiVertex* v = addQuad(texture, cx, cy, radius);
    if (!v) {
        return;
    }
    const u32 p = params(0, UiMode::TextureCircle);
    v[0] = {r.x, r.y, uv.u0, uv.v0, color, p};
    v[1] = {r.x + r.w, r.y, uv.u1, uv.v0, color, p};
    v[2] = {r.x + r.w, r.y + r.h, uv.u1, uv.v1, color, p};
    v[3] = {r.x, r.y + r.h, uv.u0, uv.v1, color, p};
}

void DrawList::customQuad(const UiVertex (&corners)[4], TextureHandle texture, f32 circleX, f32 circleY,
                          f32 circleRadius) { // retouches (ui models)
    if (UiVertex* v = addQuad(texture, circleX, circleY, circleRadius)) {
        for (u32 i = 0; i < 4; ++i) {
            v[i] = corners[i];
        }
    }
}

void DrawList::textureQuadInCircle(const f32 corners[8], TextureHandle texture, f32 cx, f32 cy, f32 radius,
                                   TexCoords uv, u32 color) {
    UiVertex* v = addQuad(texture, cx, cy, radius);
    if (!v) {
        return;
    }
    const u32 p = params(0, UiMode::TextureCircle);
    v[0] = {corners[0], corners[1], uv.u0, uv.v0, color, p};
    v[1] = {corners[2], corners[3], uv.u1, uv.v0, color, p};
    v[2] = {corners[4], corners[5], uv.u1, uv.v1, color, p};
    v[3] = {corners[6], corners[7], uv.u0, uv.v1, color, p};
}

void DrawList::portrait(const Rect& r, TextureHandle portrait, u64 fallback, u32 color) {
    if (!portrait.valid()) {
        sprite(r, fallback, color);
        return;
    }
    textureInCircle(r, portrait, r.centerX(), r.centerY(), std::min(r.w, r.h) * 0.5f, {}, color);
}

void DrawList::spriteQuad(const f32 corners[8], const Sprite& s, u32 color, TexCoords uv, bool additive) {
    UiVertex* v = addQuad({});
    if (!v) {
        return;
    }
    const f32 inv = 1.0f / m_assets->atlasSize();
    const f32 u0 = (s.x + uv.u0 * s.width) * inv, v0 = (s.y + uv.v0 * s.height) * inv;
    const f32 u1 = (s.x + uv.u1 * s.width) * inv, v1 = (s.y + uv.v1 * s.height) * inv;
    const u32 p = params(s.page, UiMode::Atlas) | (additive ? (1u << 24) : 0u);
    v[0] = {corners[0], corners[1], u0, v0, color, p};
    v[1] = {corners[2], corners[3], u1, v0, color, p};
    v[2] = {corners[4], corners[5], u1, v1, color, p};
    v[3] = {corners[6], corners[7], u0, v1, color, p};
}

void DrawList::backdropBorder(const Rect& r, u64 borderHash, f32 edge, u32 color) {
    const std::optional<Sprite> found = m_assets->sprite(borderHash);
    if (!found) {
        return;
    }
    const Sprite& s = *found;
    const f32 inv = 1.0f / m_assets->atlasSize();
    // Cell i of the 8 vertical strips: u across the strip, v along it.
    auto cell = [&](u32 i, f32& u0, f32& u1, f32& v0, f32& v1) {
        u0 = (s.x + s.width * (static_cast<f32>(i) / 8.0f)) * inv;
        u1 = (s.x + s.width * (static_cast<f32>(i + 1) / 8.0f)) * inv;
        v0 = s.y * inv;
        v1 = (s.y + s.height) * inv;
    };
    const u32 p = params(s.page, UiMode::Atlas);
    auto put = [&](f32 x0, f32 y0, f32 x1, f32 y1, u32 i, bool rotate) {
        UiVertex* v = addQuad({});
        if (!v) {
            return;
        }
        f32 u0, u1, v0, v1;
        cell(i, u0, u1, v0, v1);
        if (!rotate) {
            v[0] = {x0, y0, u0, v0, color, p};
            v[1] = {x1, y0, u1, v0, color, p};
            v[2] = {x1, y1, u1, v1, color, p};
            v[3] = {x0, y1, u0, v1, color, p};
        } else { // the strip laid horizontally: its length (v) runs along x
            v[0] = {x0, y0, u0, v0, color, p};
            v[1] = {x1, y0, u0, v1, color, p};
            v[2] = {x1, y1, u1, v1, color, p};
            v[3] = {x0, y1, u1, v0, color, p};
        }
    };
    const f32 x0 = r.x, y0 = r.y, x1 = r.right(), y1 = r.bottom();
    put(x0, y0 + edge, x0 + edge, y1 - edge, 0, false); // left
    put(x1 - edge, y0 + edge, x1, y1 - edge, 1, false); // right
    put(x0 + edge, y0, x1 - edge, y0 + edge, 2, true);  // top
    put(x0 + edge, y1 - edge, x1 - edge, y1, 3, true);  // bottom
    put(x0, y0, x0 + edge, y0 + edge, 4, false);        // corners
    put(x1 - edge, y0, x1, y0 + edge, 5, false);
    put(x0, y1 - edge, x0 + edge, y1, 6, false);
    put(x1 - edge, y1 - edge, x1, y1, 7, false);
}

void DrawList::spriteDesaturated(const Rect& r, u64 pathHash, u32 color, TexCoords uv) {
    const std::optional<Sprite> found = m_assets->sprite(pathHash);
    if (!found) {
        return;
    }
    const Sprite& s = *found;
    const f32 inv = 1.0f / m_assets->atlasSize();
    const TexCoords atlas{(s.x + uv.u0 * s.width) * inv, (s.y + uv.v0 * s.height) * inv,
                          (s.x + uv.u1 * s.width) * inv, (s.y + uv.v1 * s.height) * inv};
    quad(r, atlas, color, params(s.page, UiMode::AtlasDesaturated));
}

void DrawList::iconDesaturated(const Rect& r, u32 slot, u32 color, TexCoords uv) {
    quad(r, uv, color, params(slot, UiMode::IconDesaturated));
}

f32 DrawList::lineHeight(FontId font, f32 size) const {
    const Font& f = m_assets->font(font);
    return size * (f.pixelHeight() + f.lineGap()) / f.pixelHeight();
}

f32 DrawList::ascent(FontId font, f32 size) const {
    const Font& f = m_assets->font(font);
    return size * f.ascent() / f.pixelHeight();
}

f32 DrawList::measure(std::string_view utf8, FontId font, f32 size) const {
    const Font& f = m_assets->font(font);
    const f32 scale = size / f.pixelHeight();
    TextCursor cursor{utf8, 0, 0, 0};
    f32 width = 0.0f, best = 0.0f;
    u32 previous = 0;
    u32 cp = 0;
    while (cursor.next(cp)) {
        if (cp == '\n') {
            best = std::max(best, width);
            width = 0.0f;
            previous = 0;
            continue;
        }
        width += (f.glyph(cp).advance + (previous ? f.kerning(previous, cp) : 0.0f)) * scale;
        previous = cp;
    }
    return std::max(best, width);
}

void DrawList::glyphs(f32 x, f32 baselineY, std::string_view utf8, const TextStyle& style, u32 color,
                      bool colorCodes, const QuarterTurn* turn) {
    const Font& f = m_assets->font(style.font);
    const f32 scale = style.size / f.pixelHeight();
    const f32 invAtlas = 1.0f / m_assets->fontAtlasSize();
    const u32 outline =
        style.outline > 0.0f ? static_cast<u32>(std::min(255.0f, style.outline / (scale * f.sdfRange()) * 255.0f)) : 0;
    TextCursor cursor{utf8, 0, color, color};
    u32 previous = 0;
    u32 cp = 0;
    f32 pen = x;
    while (cursor.next(cp)) {
        if (cp == '\n') {
            break;
        }
        const pack::UiGlyph& g = f.glyph(cp);
        if (previous) {
            pen += f.kerning(previous, cp) * scale;
        }
        previous = cp;
        if (g.width > 0) {
            const Rect r{pen + g.offsetX * scale, baselineY + g.offsetY * scale, g.width * scale, g.height * scale};
            const TexCoords uv{g.x * invAtlas, g.y * invAtlas, (g.x + g.width) * invAtlas,
                               (g.y + g.height) * invAtlas};
            const u32 c = colorCodes ? cursor.color : color;
            const u32 p = params(g.layer + f.layerBase(), UiMode::Text, outline); // phase 22: app.vdpk's font
            if (!turn) {
                quad(r, uv, c, p);
            } else if (UiVertex* v = addQuad({})) {
                // The glyph's corners (clockwise from its top left) turned: its top edge runs upwards on the left.
                const f32 left = turn->x + r.y, right = turn->x + r.y + r.h;
                const f32 bottom = turn->y + turn->length - r.x, top = bottom - r.w;
                v[0] = {left, bottom, uv.u0, uv.v0, c, p};
                v[1] = {left, top, uv.u1, uv.v0, c, p};
                v[2] = {right, top, uv.u1, uv.v1, c, p};
                v[3] = {right, bottom, uv.u0, uv.v1, c, p};
            }
        }
        pen += g.advance * scale;
    }
}

f32 DrawList::text(f32 x, f32 baselineY, std::string_view utf8, const TextStyle& style) {
    const f32 width = measure(utf8, style.font, style.size);
    const f32 left = style.align == Align::Left ? x : style.align == Align::Center ? x - width * 0.5f : x - width;
    if (style.shadow > 0.0f) {
        glyphs(left + style.shadow, baselineY + style.shadow, utf8, style, withAlpha(kBlack, static_cast<f32>(style.color >> 24) / 255.0f), false);
    }
    glyphs(left, baselineY, utf8, style, style.color, true);
    return width;
}

void DrawList::textTurned(const Rect& r, std::string_view utf8, const TextStyle& style) {
    // Laid out along the box's height (its length) and across its width, then turned.
    const QuarterTurn turn{r.x, r.y, r.h};
    const f32 left = (r.h - measure(utf8, style.font, style.size)) * 0.5f;
    const f32 baseline = r.w * 0.5f + ascent(style.font, style.size) - style.size * 0.5f;
    if (style.shadow > 0.0f) {
        glyphs(left + style.shadow, baseline + style.shadow, utf8, style,
               withAlpha(kBlack, static_cast<f32>(style.color >> 24) / 255.0f), false, &turn);
    }
    glyphs(left, baseline, utf8, style, style.color, true, &turn);
}

f32 DrawList::wrappedText(const Rect& r, std::string_view utf8, const TextStyle& style) {
    const Font& f = m_assets->font(style.font);
    const f32 scale = style.size / f.pixelHeight();
    const f32 line = lineHeight(style.font, style.size);
    f32 baseline = r.y + ascent(style.font, style.size);
    // Greedy: the longest run of words that fits, broken at the last space (or anywhere in a word too long).
    TextCursor cursor{utf8, 0, style.color, style.color};
    usize lineStart = 0;
    u32 lineColor = style.color;
    f32 width = 0.0f;
    usize lastSpace = std::string_view::npos; // byte index of the space
    u32 lastSpaceColor = style.color;
    u32 previous = 0;
    u32 cp = 0;
    auto emit = [&](usize end, u32 color) {
        TextStyle lineStyle = style;
        lineStyle.color = color;
        const std::string_view text = utf8.substr(lineStart, end - lineStart);
        const f32 lineWidth = measure(text, style.font, style.size);
        const f32 x = style.align == Align::Left ? r.x : style.align == Align::Center ? r.centerX() - lineWidth * 0.5f
                                                                                       : r.right() - lineWidth;
        if (style.shadow > 0.0f) { // as faded as the text (the chat's lines fade out)
            glyphs(x + style.shadow, baseline + style.shadow, text, lineStyle,
                   withAlpha(kBlack, static_cast<f32>(style.color >> 24) / 255.0f), false);
        }
        glyphs(x, baseline, text, lineStyle, color, true);
        baseline += line;
    };
    while (true) {
        const usize at = cursor.i;
        const u32 colorAt = cursor.color;
        if (!cursor.next(cp)) {
            if (cursor.i > lineStart) {
                emit(cursor.i, lineColor);
            }
            break;
        }
        if (cp == '\n') {
            emit(at, lineColor);
            lineStart = cursor.i;
            lineColor = cursor.color;
            width = 0.0f;
            lastSpace = std::string_view::npos;
            previous = 0;
            continue;
        }
        const f32 advance = (f.glyph(cp).advance + (previous ? f.kerning(previous, cp) : 0.0f)) * scale;
        if (cp == ' ') {
            lastSpace = cursor.i - 1; // the space itself, after any escape before it ("NOT|r limited")
            lastSpaceColor = cursor.color;
        }
        if (width + advance > r.w && at > lineStart && cp != ' ') {
            if (lastSpace != std::string_view::npos && lastSpace > lineStart) {
                emit(lastSpace, lineColor);
                lineStart = lastSpace + 1;
                lineColor = lastSpaceColor;
            } else {
                emit(at, lineColor);
                lineStart = at;
                lineColor = colorAt;
            }
            // Measure again from the new line start up to here.
            cursor.i = lineStart;
            cursor.color = lineColor;
            width = 0.0f;
            lastSpace = std::string_view::npos;
            previous = 0;
            continue;
        }
        width += advance;
        previous = cp;
    }
    return baseline - line + (line - ascent(style.font, style.size)) - r.y;
}

void DrawList::triangle(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2, u32 color) {
    UiVertex* v = addQuad({});
    if (!v) {
        return;
    }
    const u32 p = params(0, UiMode::Solid);
    v[0] = {x0, y0, 0.0f, 0.0f, color, p};
    v[1] = {x1, y1, 0.0f, 0.0f, color, p};
    v[2] = {x2, y2, 0.0f, 0.0f, color, p};
    v[3] = {x2, y2, 0.0f, 0.0f, color, p}; // the second triangle (0, 2, 3) is empty
}

void DrawList::iconCircle(const Rect& r, u32 slot, u32 color, f32 crop) {
    const f32 c = std::clamp(crop, 0.0f, 0.45f);
    const u32 radius = static_cast<u32>((0.5f - c) * 510.0f + 0.5f); // in the icon's uv, 255 = 0.5
    quad(r, {c, c, 1.0f - c, 1.0f - c}, color, params(slot, UiMode::IconCircle, radius));
}

void DrawList::ring(f32 cx, f32 cy, f32 radius, f32 inner, u32 color, f32 arc, bool additive) {
    const u32 turn = static_cast<u32>(std::clamp(arc, 0.0f, 1.0f) * 255.0f + 0.5f);
    if (radius <= 0.0f || turn == 0) {
        return;
    }
    const u32 hole = static_cast<u32>(std::clamp(inner, 0.0f, 1.0f) * 127.0f + 0.5f);
    const u32 p = params(0, UiMode::Ring, turn) | (hole << 25) | (additive ? (1u << 24) : 0u);
    quad({cx - radius, cy - radius, 2.0f * radius, 2.0f * radius}, {}, color, p);
}

f32 DrawList::measureWrapped(f32 width, std::string_view utf8, FontId font, f32 size) const {
    // The same greedy breaking as wrappedText(), counting lines instead of drawing them.
    const Font& f = m_assets->font(font);
    const f32 scale = size / f.pixelHeight();
    TextCursor cursor{utf8, 0, 0, 0};
    usize lineStart = 0;
    f32 lineWidth = 0.0f;
    usize lastSpace = std::string_view::npos;
    u32 previous = 0;
    u32 cp = 0;
    u32 lines = 0;
    while (true) {
        const usize at = cursor.i;
        if (!cursor.next(cp)) {
            if (cursor.i > lineStart) {
                ++lines;
            }
            break;
        }
        if (cp == '\n') {
            ++lines;
            lineStart = cursor.i;
            lineWidth = 0.0f;
            lastSpace = std::string_view::npos;
            previous = 0;
            continue;
        }
        const f32 advance = (f.glyph(cp).advance + (previous ? f.kerning(previous, cp) : 0.0f)) * scale;
        if (cp == ' ') {
            lastSpace = cursor.i - 1; // the space itself, after any escape before it ("NOT|r limited")
        }
        if (lineWidth + advance > width && at > lineStart && cp != ' ') {
            ++lines;
            lineStart = lastSpace != std::string_view::npos && lastSpace > lineStart ? lastSpace + 1 : at;
            cursor.i = lineStart;
            lineWidth = 0.0f;
            lastSpace = std::string_view::npos;
            previous = 0;
            continue;
        }
        lineWidth += advance;
        previous = cp;
    }
    return static_cast<f32>(lines) * lineHeight(font, size);
}

u32 DrawList::wrappedSpan(f32 width, std::string_view utf8, FontId font, f32 size, usize first, usize last,
                          std::span<Rect> out) const {
    // The same greedy breaking as wrappedText(): each line's bytes, then the span's part of them measured from the
    // line's start.
    if (first >= last) {
        return 0;
    }
    const Font& f = m_assets->font(font);
    const f32 scale = size / f.pixelHeight();
    const f32 lineH = lineHeight(font, size);
    TextCursor cursor{utf8, 0, 0, 0};
    usize lineStart = 0;
    f32 lineWidth = 0.0f;
    usize lastSpace = std::string_view::npos;
    u32 previous = 0;
    u32 cp = 0;
    u32 line = 0;
    u32 written = 0;
    const auto place = [&](usize begin, usize end) {
        if (first < end && last > begin && written < out.size()) {
            const usize from = std::max(first, begin), to = std::min(last, end);
            const f32 x0 = measure(utf8.substr(begin, from - begin), font, size);
            const f32 x1 = measure(utf8.substr(begin, to - begin), font, size);
            out[written++] = {x0, static_cast<f32>(line) * lineH, x1 - x0, lineH};
        }
        ++line;
    };
    while (true) {
        const usize at = cursor.i;
        if (!cursor.next(cp)) {
            if (cursor.i > lineStart) {
                place(lineStart, cursor.i);
            }
            break;
        }
        if (cp == '\n') {
            place(lineStart, at);
            lineStart = cursor.i;
            lineWidth = 0.0f;
            lastSpace = std::string_view::npos;
            previous = 0;
            continue;
        }
        const f32 advance = (f.glyph(cp).advance + (previous ? f.kerning(previous, cp) : 0.0f)) * scale;
        if (cp == ' ') {
            lastSpace = cursor.i - 1; // the space itself, after any escape before it ("NOT|r limited")
        }
        if (lineWidth + advance > width && at > lineStart && cp != ' ') {
            const bool atSpace = lastSpace != std::string_view::npos && lastSpace > lineStart;
            place(lineStart, atSpace ? lastSpace : at);
            lineStart = atSpace ? lastSpace + 1 : at;
            cursor.i = lineStart;
            lineWidth = 0.0f;
            lastSpace = std::string_view::npos;
            previous = 0;
            continue;
        }
        lineWidth += advance;
        previous = cp;
    }
    return written;
}

} // namespace vd::ui
