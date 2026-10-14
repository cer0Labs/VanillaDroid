// Phase 11 (f11/interiors, 11.5): the terrain's and the water's light as 1.12 draws them (shaders terrain1..4[_s],
// ocean0_s): the baked shadow, the specular highlights of "Terrain Highlights" and the scrolling layers (MCLY).
// Included after frame.glsl.

// Specular exponent of the per-vertex highlight (the client's secondary color; its vertex programs are fixed
// function, so the power is our choice, kept wide like a per-vertex highlight looks).
const float kSurfaceShininess = 16.0;

// The highlight of the sun (or the moon) on a surface, per vertex: Blinn with the direct light's color. Zero with the
// option off.
mediump vec3 surfaceSpecular(mediump vec3 normal, highp vec3 cameraRelative) {
    if (frame.surfaceLight.y < 0.5) {
        return vec3(0.0);
    }
    mediump vec3 toEye = normalize(-cameraRelative);
    mediump vec3 halfway = normalize(frame.lightDirection.xyz + toEye);
    return frame.directColor.rgb * pow(max(dot(normal, halfway), 0.0), kSurfaceShininess);
}

// Terrain1..4: the baked shadow darkens the whole lit color to 70 % (shadow * 0.3 + 0.7, `shadow` 1 = in shadow
// here), and the highlight is the layers' blended alpha (their _s textures' mask) where no shadow falls.
mediump float terrainShade(mediump float shadow) {
    return 1.0 - 0.3 * shadow;
}
mediump vec3 terrainHighlight(mediump float mask, mediump float shadow, mediump vec3 specular) {
    return specular * (mask * (1.0 - shadow) * frame.surfaceLight.z);
}

// MCLY 0x80 "overbright": the layer glows on its own (the lava floors of the Burning Steppes, always animated too),
// unlit like the magma of the liquids. Its share of the blended color (layer i's flag in bit i of `overbright`).
mediump float terrainGlow(uint overbright, uvec4 layers, mediump vec3 alpha) {
    if (overbright == 0u) {
        return 0.0;
    }
    mediump float glow = (overbright & 1u) != 0u ? 1.0 : 0.0;
    if (layers.y != 255u) {
        glow = mix(glow, float((overbright >> 1u) & 1u), alpha.r);
    }
    if (layers.z != 255u) {
        glow = mix(glow, float((overbright >> 2u) & 1u), alpha.g);
    }
    if (layers.w != 255u) {
        glow = mix(glow, float((overbright >> 3u) & 1u), alpha.b);
    }
    return glow;
}

// MCLY texture animation (TerrainChunk::layerFlags, world/InteriorLighting.h terrainLayerOffset): the layer's UV
// offset in texture repeats.
highp vec2 terrainLayerOffset(uint flags) {
    if ((flags & 0x40u) == 0u) {
        return vec2(0.0);
    }
    const vec2 kDirections[8] = vec2[8](vec2(0.0, 1.0), vec2(-1.0, 1.0), vec2(-1.0, 0.0), vec2(-1.0, -1.0),
                                        vec2(0.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));
    highp float speed = float((flags >> 3u) & 7u);
    return kDirections[flags & 7u] * fract(frame.fogParams.z * speed / 11.2);
}
