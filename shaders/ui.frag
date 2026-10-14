#version 320 es
precision mediump float;

// Touch UI (phase 7): one pipeline for every quad; UiVertex::params picks the source (render/Renderer.h UiMode).

layout(set = 0, binding = 0) uniform mediump sampler2DArray uAtlas; // interface sprites, one page per layer
layout(set = 0, binding = 1) uniform mediump sampler2DArray uIcons; // icon cache, one icon per layer
layout(set = 0, binding = 2) uniform mediump sampler2DArray uFonts; // glyph distance fields
layout(set = 1, binding = 0) uniform mediump sampler2D uTexture;    // the draw command's texture

layout(push_constant) uniform Push {
    vec4 scaleOffset;
    vec4 rotation;
    vec4 circle;
} pc;

layout(location = 0) in highp vec2 vUV;
layout(location = 1) in mediump vec4 vColor;
layout(location = 2) flat in highp uint vParams;
layout(location = 3) in highp vec2 vPosition;

layout(location = 0) out vec4 outColor;

void main() {
    uint mode = (vParams >> 12u) & 15u;
    float layer = float(vParams & 4095u);
    vec4 color;
    if (mode == 0u) {
        color = texture(uAtlas, vec3(vUV, layer)) * vColor;
    } else if (mode == 1u) {
        color = texture(uIcons, vec3(vUV, layer)) * vColor;
    } else if (mode == 2u) {
        // Distance field: 0.5 on the outline. The smoothing width follows the on-screen scale of the field.
        float distance = texture(uFonts, vec3(vUV, layer)).r;
        float width = max(fwidth(distance), 1.0 / 255.0) * 0.75;
        float fill = smoothstep(0.5 - width, 0.5 + width, distance);
        float outline = float((vParams >> 16u) & 255u) / 510.0; // up to half the encoded range
        if (outline > 0.0) {
            float edge = smoothstep(0.5 - outline - width, 0.5 - outline + width, distance);
            color = vec4(vColor.rgb * fill, vColor.a * edge);
        } else {
            color = vec4(vColor.rgb, vColor.a * fill);
        }
    } else if (mode == 3u) {
        color = texture(uTexture, vUV) * vColor;
    } else if (mode == 4u) {
        color = texture(uTexture, vUV) * vColor;
        float d = distance(vPosition, pc.circle.xy);
        color.a *= clamp(pc.circle.z - d + 0.5, 0.0, 1.0); // antialiased rim
    } else if (mode == 6u) {
        // Phase 20: an icon inside a circle (the touch buttons); its uv is centered on 0.5, the radius in bits 16-23.
        color = texture(uIcons, vec3(vUV, layer)) * vColor;
        float radius = float((vParams >> 16u) & 255u) / 510.0;
        float d = distance(vUV, vec2(0.5));
        color.a *= clamp((radius - d) / max(fwidth(d), 1.0e-4) + 0.5, 0.0, 1.0);
    } else if (mode == 7u) {
        // Phase 20: a disc or ring over the quad (uv 0..1), the hole's share of the radius in bits 25-31; only the last
        // share of the turn in bits 16-23 (clockwise from twelve o'clock) when below 255: a cooldown's sweep.
        color = vColor;
        vec2 p = vUV * 2.0 - 1.0;
        float d = length(p);
        float w = max(fwidth(d), 1.0e-4);
        float a = clamp((1.0 - d) / w + 0.5, 0.0, 1.0);
        float hole = float((vParams >> 25u) & 127u) / 127.0;
        if (hole > 0.0) {
            a *= clamp((d - hole) / w + 0.5, 0.0, 1.0);
        }
        uint turn = (vParams >> 16u) & 255u;
        if (turn < 255u) {
            // Clockwise from twelve o'clock (screen y grows down): 0 at the top, 0.25 at three o'clock.
            float t = fract(atan(p.x, -p.y) / 6.2831853 + 1.0);
            float edge = clamp(fwidth(t), 1.0e-4, 0.02); // the seam at twelve jumps from 1 to 0: keep it sharp
            a *= clamp((t - (1.0 - float(turn) / 255.0)) / edge + 0.5, 0.0, 1.0);
        }
        color.a *= a;
    } else if (mode == 14u || mode == 15u) {
        // Retouches (windows): a sprite of the atlas (14) or an icon of the icon cache (15) desaturated as
        // the client's SetDesaturated draws it (Shaders\Pixel\Desaturate.bls): the texel's luminance (0.299,
        // 0.587, 0.114) and its alpha times the vertex's; the vertex's colour does not reach the texel there.
        vec4 texel = mode == 14u ? texture(uAtlas, vec3(vUV, layer)) : texture(uIcons, vec3(vUV, layer));
        color = vec4(vec3(dot(texel.rgb, vec3(0.299, 0.587, 0.114))), texel.a * vColor.a);
    } else {
        color = vColor;
    }
    // Premultiplied: additive quads (bit 24, the client's alphaMode="ADD") add their light and cover nothing.
    // No discard: blending on the tile is cheap, discard would disable fast paths.
    bool additive = ((vParams >> 24u) & 1u) != 0u;
    outColor = vec4(color.rgb * color.a, additive ? 0.0 : color.a);
}
