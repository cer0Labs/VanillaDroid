// Fragment body shared by model.frag (no discard: keeps early depth rejection) and model_alphakey.frag.
precision mediump float;

#include "frame.glsl"
#include "screenfx.glsl"

layout(set = 1, binding = 0) uniform mediump sampler2D uTexture;

#include "model_push.glsl"
#include "interior.glsl"

// Phase 18 (world): centroid: with multisampling a pixel on a triangle's edge runs with the UV of its centre even when
// only some samples are inside, and outside the triangle that UV leaves the texture's island (trees' trunks showed
// their ring seams as dashed lines: the transparent texels next to the bark failed the test). One sample: the same.
layout(location = 0) centroid in highp vec2 vUV;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vColor;
layout(location = 3) in float vFog;
layout(location = 6) in vec4 vInstanceLight;

layout(location = 0) out vec4 outColor;

const uint kUnlit = 1u;
const uint kUnfogged = 2u;
const uint kExteriorLight = 128u;
const uint kBlendMod = 4u;
const uint kBlendMod2x = 5u;
const float kAlphaRef = 0.5;
// pc.flags.w (phase 11): an opaque part of a translucent unit. A macro: a constant equal to kUnlit would share its id
// and be decorated RelaxedPrecision twice (invalid SPIR-V).
#define VD_FLAG_FADED 1u
// Phase 14 (doodads): this draw's instances fade with the distance and an M2's vertex alpha (vColor.a, white in M2s)
// carries the fade: opaque and alpha-tested parts (in this shader's ALPHA_KEY variant) dither out, blended ones scale
// their opacity. VD_FLAG_NO_ALPHA_TEST: an opaque material drawn with the alpha-key variant for that dither.
#define VD_FLAG_DOODAD_FADE 2u
#define VD_FLAG_NO_ALPHA_TEST 4u

// An ordered 4x4 (Bayer) threshold for the screen-door fade: 1 / 32 .. 31 / 32.
const float kDither[16] = float[16](0.5, 8.5, 2.5, 10.5, 12.5, 4.5, 14.5, 6.5, 3.5, 11.5, 1.5, 9.5, 15.5, 7.5, 13.5, 5.5);

// Phase 18 (world): multisampling with alpha to coverage (model_alphakey_a2c.frag): the alpha test becomes the pixel's
// coverage, ramping over one pixel around the reference, so foliage's cut-out edges are smoothed like the geometry's.
// Below the ramp the fragment still goes (no samples covered). Two compiled variants, not a specialization constant:
// the Mi 11X's Adreno 650 driver (Vulkan 1.1.128) dropped the alpha test's discard of the unspecialized one (foliage
// drawn as solid quads).
#ifdef ALPHA_TO_COVERAGE
const bool kAlphaToCoverage = true;
#else
const bool kAlphaToCoverage = false;
#endif

void main() {
    vec4 texel = texture(uTexture, vUV);
#ifdef ALPHA_KEY
    float coverage = 1.0;
    if ((pc.flags.w & VD_FLAG_NO_ALPHA_TEST) == 0u) {
        if (kAlphaToCoverage) {
            coverage = clamp((texel.a - kAlphaRef) / max(fwidth(texel.a), 1.0 / 255.0) + 0.5, 0.0, 1.0);
            if (coverage <= 0.0) {
                discard;
            }
        } else if (texel.a < kAlphaRef) {
            discard;
        }
    }
    if ((pc.flags.w & VD_FLAG_DOODAD_FADE) != 0u) {
        uvec2 cell = uvec2(gl_FragCoord.xy) & 3u;
        if (vColor.a * 16.0 <= kDither[cell.y * 4u + cell.x]) {
            discard;
        }
    }
#endif
    vec3 light;
    if ((pc.flags.x & kUnlit) != 0u) {
        light = vec3(1.0);
    } else if (pc.flags.y != 0u && (pc.flags.x & kExteriorLight) == 0u) { // phase 11: inner window faces: outdoors
        light = vertexColorLight(vColor, pc.ambient.rgb, vNormal); // phase 11: baked vertex lighting and outdoors
    } else {
        light = frame.ambientColor.rgb + frame.directColor.rgb * max(dot(normalize(vNormal), frame.lightDirection.xyz), 0.0);
        light = min(light, vec3(1.0)); // saturated like the 1.12 client's (ADD_SAT in Model2.bls)
    }
    light = applyInstanceLight(light, vInstanceLight, pc.flags.x); // phase 11: interiors
    texel *= pc.color;
    vec3 color = texel.rgb * light + nightGlow(texel.rgb, pc.flags.x, pc.selfLightColor.rgb); // phase 11: lit windows
    if ((pc.flags.x & kUnfogged) == 0u) {
        // Multiplicative blends fade to "no change" in the fog, the rest to the fog color.
        vec3 fogTarget = pc.flags.z == kBlendMod ? vec3(1.0) : (pc.flags.z == kBlendMod2x ? vec3(0.5) : frame.fogColor.rgb);
        color = mix(color, fogTarget, vFog);
    }
    outColor = vec4(color, texel.a);
    outColor.rgb = screenFxBlend(outColor.rgb, pc.flags.z); // phase 11: the client's full-screen effects
    if ((pc.flags.w & VD_FLAG_FADED) != 0u) {
        outColor.a = pc.color.a; // its fade, not the texture's alpha (which an opaque material does not use)
    }
    // Phase 15: a model frame of the interface (app/ModelViews) keeps a real alpha over its transparent background: its
    // opaque and alpha-tested surfaces (blend modes 0 and 1) cover what is behind them, whatever their texture's alpha.
    if (frame.conditions.z > 0.5 && pc.flags.z <= 1u) {
        outColor.a = 1.0;
    }
#ifdef ALPHA_KEY
    if (kAlphaToCoverage) {
        outColor.a = coverage; // phase 18 (world): what alpha to coverage reads (an opaque pass: nothing blends it)
    }
#endif
#ifndef ALPHA_KEY
    if ((pc.flags.w & VD_FLAG_DOODAD_FADE) != 0u) { // phase 14: a blended part of a doodad fading with the distance
        if (pc.flags.z == kBlendMod || pc.flags.z == kBlendMod2x) {
            outColor.rgb = mix(pc.flags.z == kBlendMod ? vec3(1.0) : vec3(0.5), outColor.rgb, vColor.a);
        } else {
            outColor.a *= vColor.a;
        }
    }
#endif
}
