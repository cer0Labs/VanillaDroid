#pragma once

#include "core/Types.h"
#include "render/Renderer.h"
#include "ui/UiAssets.h"
#include "ui/UiModels.h" // retouches (ui models)

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace vd::ui {

// A rectangle in logical pixels (y down).
struct Rect {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 w = 0.0f;
    f32 h = 0.0f;

    f32 right() const { return x + w; }
    f32 bottom() const { return y + h; }
    f32 centerX() const { return x + w * 0.5f; }
    f32 centerY() const { return y + h * 0.5f; }
    bool contains(f32 px, f32 py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    Rect inset(f32 d) const { return {x + d, y + d, w - 2.0f * d, h - 2.0f * d}; }
    Rect inset(f32 dx, f32 dy) const { return {x + dx, y + dy, w - 2.0f * dx, h - 2.0f * dy}; }
};

// RGBA8 as the vertex expects it (red in the low byte).
constexpr u32 rgba(u8 r, u8 g, u8 b, u8 a = 255) {
    return u32{r} | (u32{g} << 8) | (u32{b} << 16) | (u32{a} << 24);
}
constexpr u32 withAlpha(u32 color, f32 alpha) {
    const f32 a = static_cast<f32>(color >> 24) * (alpha < 0.0f ? 0.0f : alpha > 1.0f ? 1.0f : alpha);
    return (color & 0x00FFFFFFu) | (static_cast<u32>(a + 0.5f) << 24);
}
constexpr u32 kWhite = rgba(255, 255, 255);
constexpr u32 kBlack = rgba(0, 0, 0);
// The interface's standard text colors.
constexpr u32 kGold = rgba(255, 209, 0);      // NORMAL_FONT_COLOR
constexpr u32 kHighlight = rgba(255, 255, 255);
constexpr u32 kGray = rgba(128, 128, 128);
constexpr u32 kRed = rgba(255, 26, 26);
constexpr u32 kGreen = rgba(26, 255, 26);

// Texture coordinates inside a texture, 0..1 (the part of a client texture a frame shows, as SetTexCoord).
struct TexCoords {
    f32 u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
};

enum class Align : u8 { Left, Center, Right };

struct TextStyle {
    FontId font = FontId::Friz;
    f32 size = 16.0f;      // line height (ascent + descent), logical pixels
    u32 color = kGold;
    f32 outline = 0.0f;    // black outline, logical pixels
    f32 shadow = 0.0f;     // black copy offset down-right by this many pixels (0 = none)
    Align align = Align::Left;
};

// The UI of one frame as quads for the renderer, in drawing order. Consecutive quads with the same clip rectangle and
// external texture share a draw command. Fixed capacity: nothing allocates while drawing.
class DrawList {
public:
    void init(const Assets* assets, u32 maxQuads);
    void reset();

    void pushClip(const Rect& clip); // intersected with the current one
    void popClip();

    void solid(const Rect& r, u32 color);
    // A client interface texture (Assets::sprite) or part of it, stretched over `r`.
    void sprite(const Rect& r, const Sprite& sprite, u32 color = kWhite, TexCoords uv = {});
    void sprite(const Rect& r, u64 pathHash, u32 color = kWhite, TexCoords uv = {});
    // Adds its light instead of covering (the client's alphaMode="ADD": sparks, flashes, glows).
    void spriteAdditive(const Rect& r, u64 pathHash, u32 color = kWhite, TexCoords uv = {});
    // An icon of the icon cache (slot). `uv` crops the 64x64 icon (the client shows 0.08..0.92 inside buttons).
    void icon(const Rect& r, u32 slot, u32 color = kWhite, TexCoords uv = {0.08f, 0.08f, 0.92f, 0.92f});
    void texture(const Rect& r, TextureHandle texture, u32 color = kWhite, TexCoords uv = {});
    // A texture drawn only inside a circle (the minimap): the quad is clipped to (cx, cy, radius) per pixel.
    void textureInCircle(const Rect& r, TextureHandle texture, f32 cx, f32 cy, f32 radius, TexCoords uv = {},
                         u32 color = kWhite);
    // A unit's portrait (Renderer::portraitTexture) in the circle inscribed in `r`, as the unit frames show
    // the client's SetPortraitTexture; while `portrait` is invalid (not rendered yet), the `fallback` sprite
    // (the frame's TemporaryPortrait art) stretched over `r`. `color` tints either (a dead target's gray).
    void portrait(const Rect& r, TextureHandle portrait, u64 fallback, u32 color = kWhite);
    // Four corner points (clockwise from top-left), for rotated sprites. Retouches (friends frame): `additive` as
    // spriteAdditive (a turned tab's highlight).
    void spriteQuad(const f32 corners[8], const Sprite& sprite, u32 color = kWhite, TexCoords uv = {},
                    bool additive = false);
    // Phase 15: a texture over four corner points (clockwise from its top-left), clipped to a circle (the minimap's
    // turned building tiles).
    void textureQuadInCircle(const f32 corners[8], TextureHandle texture, f32 cx, f32 cy, f32 radius,
                             TexCoords uv = {}, u32 color = kWhite);
    // A frame border like the client's backdrops: `sprite` split in 8 cells of `edge` pixels (left, right, top,
    // bottom, then the 4 corners), edges stretched along the rectangle.
    void backdropBorder(const Rect& r, u64 borderHash, f32 edge, u32 color = kWhite); // phase 15: `color` tints it
    // Retouches (windows): sprite() and icon() desaturated as the client's SetDesaturated
    // (UiMode::*Desaturated): only the alpha of `color` applies, as in Desaturate.bls.
    void spriteDesaturated(const Rect& r, u64 pathHash, u32 color = kWhite, TexCoords uv = {});
    void iconDesaturated(const Rect& r, u32 slot, u32 color = kWhite,
                         TexCoords uv = {0.08f, 0.08f, 0.92f, 0.92f});

    // Text. Returns the width drawn. `x` is the left edge (Align::Left), center or right edge.
    f32 text(f32 x, f32 baselineY, std::string_view utf8, const TextStyle& style);
    // Friends frame: one line turned a quarter counter-clockwise (it reads upwards, the letters' tops to the left),
    // centered in `r`: the text of a frame's tabs turned to stand along its right edge.
    void textTurned(const Rect& r, std::string_view utf8, const TextStyle& style);
    // Width of a single line (color codes skipped).
    f32 measure(std::string_view utf8, FontId font, f32 size) const;
    // Word-wrapped text inside `r` from its top; returns the height used. "\n" and "$B" break lines.
    f32 wrappedText(const Rect& r, std::string_view utf8, const TextStyle& style);
    f32 lineHeight(FontId font, f32 size) const;
    f32 ascent(FontId font, f32 size) const;

    // A solid triangle (a quad with two corners on the same spot): cooldown sweeps, arrows.
    void triangle(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2, u32 color);
    // Phase 20 (the touch cluster's round buttons): an icon of the icon cache inside the circle inscribed in `r`, its
    // own square border cropped away (`crop` of each side, as the client's 0.08 inside buttons), antialiased.
    void iconCircle(const Rect& r, u32 slot, u32 color = kWhite, f32 crop = 0.08f);
    // A disc (`inner` 0) or a ring around (cx, cy): `inner` the hole's share of the radius, `arc` the share of the turn
    // drawn, ending at twelve o'clock clockwise (a cooldown's part still to come; 1: the whole circle). Antialiased.
    void ring(f32 cx, f32 cy, f32 radius, f32 inner, u32 color, f32 arc = 1.0f, bool additive = false);
    // Height wrappedText() uses for `utf8` in a box `width` wide, without drawing it.
    f32 measureWrapped(f32 width, std::string_view utf8, FontId font, f32 size) const;
    // Where the bytes [first, last) of `utf8` are drawn when wrappedText() lays it out left-aligned in a box `width`
    // wide from (0, 0): one rectangle per line they reach (the chat's links). Returns how many were written to `out`.
    u32 wrappedSpan(f32 width, std::string_view utf8, FontId font, f32 size, usize first, usize last,
                    std::span<Rect> out) const;
    // The clip rectangle in effect (w <= 0: none, the whole screen).
    Rect clip() const { return m_clipDepth > 0 ? m_clips[m_clipDepth - 1] : Rect{}; }

    std::span<const UiVertex> vertices() const { return {m_vertices.data(), m_quadCount * 4}; }
    std::span<const UiDrawCmd> commands() const { return m_commands; }
    u32 quadCount() const { return m_quadCount; }
    bool full() const { return m_quadCount >= m_maxQuads; }
    const Assets* assets() const { return m_assets; }

    // Retouches (ui models): the interface's models (cooldowns, autocast, the minimap's ping) drawn with this list,
    // and a quad of four given corners (any UiMode; `texture` / the circle for UiMode::Texture / TextureCircle).
    UiModels& models() { return m_models; }
    void customQuad(const UiVertex (&corners)[4], TextureHandle texture = {}, f32 circleX = 0.0f,
                    f32 circleY = 0.0f, f32 circleRadius = 0.0f);

private:
    UiVertex* addQuad(TextureHandle texture, f32 circleX = 0.0f, f32 circleY = 0.0f, f32 circleRadius = 0.0f);
    void quad(const Rect& r, TexCoords uv, u32 color, u32 params, TextureHandle texture = {});
    // A quarter turn counter-clockwise of a line laid out in a box `length` long: its (lx, ly) drawn at
    // (x + ly, y + length - lx).
    struct QuarterTurn {
        f32 x = 0.0f, y = 0.0f, length = 0.0f;
    };
    void glyphs(f32 x, f32 baselineY, std::string_view utf8, const TextStyle& style, u32 color, bool colorCodes,
                const QuarterTurn* turn = nullptr);

    const Assets* m_assets = nullptr;
    std::vector<UiVertex> m_vertices;
    std::vector<UiDrawCmd> m_commands;
    u32 m_quadCount = 0;
    u32 m_maxQuads = 0;
    std::array<Rect, 16> m_clips{};
    u32 m_clipDepth = 0;
    bool m_clipChanged = true;
    UiModels m_models; // retouches (ui models)
};

// UTF-8 -> codepoints (invalid bytes become U+FFFD). Advances `i`.
u32 decodeUtf8(std::string_view s, usize& i);

} // namespace vd::ui
