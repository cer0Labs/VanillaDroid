#version 320 es
#extension GL_GOOGLE_include_directive : require

// Terrain splatting: up to 4 layers from the map's texture array, blended in order with the alphas of
// the tile's alpha texture (R, G, B = layers 1..3, A = baked shadow), vanilla-style lighting and fog.
precision mediump float;

#include "frame.glsl"
#include "surface.glsl"
#include "screenfx.glsl"

layout(set = 1, binding = 0) uniform mediump sampler2DArray uLayers;
layout(set = 2, binding = 0) uniform mediump sampler2D uAlpha;

layout(location = 0) in highp vec2 vChunkUV;
layout(location = 1) flat in highp vec2 vAlphaBase;
layout(location = 2) flat in uvec4 vLayers;
layout(location = 3) in vec3 vNormal;
layout(location = 4) in float vFog;
layout(location = 8) in vec3 vSpecular;               // phase 11
layout(location = 9) flat in highp vec4 vLayerOffset01;  // phase 11: MCLY scrolling of layers 0, 1
layout(location = 10) flat in highp vec4 vLayerOffset23; // and 2, 3
layout(location = 11) flat in uint vOverbright;          // phase 11: layers that glow (lava)

layout(location = 0) out vec4 outColor;

const float kTextureRepeat = 8.0; // layer textures repeat 8 times per chunk
const uint kNoLayer = 255u;

void main() {
    highp vec2 uv = vChunkUV * kTextureRepeat;
    // Stay inside this chunk's 64x64 alpha block: neighbours may use other layers.
    highp vec2 alphaUV = vAlphaBase + clamp(vChunkUV, 0.5 / 64.0, 63.5 / 64.0) * (1.0 / 16.0);
    vec4 alpha = texture(uAlpha, alphaUV);

    // RGB and, in alpha, the specular mask (phase 11: the client's _s textures), blended alike.
    vec4 color = vec4(0.35, 0.33, 0.3, 0.0); // chunks without layers (ocean floor)
    if (vLayers.x != kNoLayer) {
        color = texture(uLayers, vec3(uv + vLayerOffset01.xy, float(vLayers.x)));
    }
    if (vLayers.y != kNoLayer) {
        color = mix(color, texture(uLayers, vec3(uv + vLayerOffset01.zw, float(vLayers.y))), alpha.r);
    }
    if (vLayers.z != kNoLayer) {
        color = mix(color, texture(uLayers, vec3(uv + vLayerOffset23.xy, float(vLayers.z))), alpha.g);
    }
    if (vLayers.w != kNoLayer) {
        color = mix(color, texture(uLayers, vec3(uv + vLayerOffset23.zw, float(vLayers.w))), alpha.b);
    }

    vec3 normal = normalize(vNormal);
    float diffuse = max(dot(normal, frame.lightDirection.xyz), 0.0);
    // Saturated like the 1.12 client's vertex colors (an interpolated color never exceeds 1); phase 11: 1.12's shadow
    // (the whole light to 70 %), then the highlights (its secondary color, added after) and the glowing layers.
    vec3 lit = color.rgb * min(frame.ambientColor.rgb + frame.directColor.rgb * diffuse, vec3(1.0)) *
                   terrainShade(alpha.a) +
               terrainHighlight(color.a, alpha.a, vSpecular);
    lit = mix(lit, color.rgb, terrainGlow(vOverbright, vLayers, alpha.rgb));
    outColor = vec4(mix(lit, frame.fogColor.rgb, vFog), 1.0);
    outColor.rgb = screenFx(outColor.rgb); // phase 11: the client's full-screen effects
}
