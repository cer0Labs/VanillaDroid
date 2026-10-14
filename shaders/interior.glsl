// Phase 11 (f11/interiors): the light of interiors and of the night (world/InteriorLighting.h). Included by the
// model and actor shaders after frame.glsl.

// An instance's packed light (ModelInstanceData::light, ActorInstanceData::light): an ambient and a directional
// color with an octahedral direction, (0, 0) = lit like the outdoors. Evaluated per vertex, like 1.12 lights its
// models. rgb: the light; a: 1 when it replaces the frame's light.
mediump vec4 instanceLight(uvec2 packed, mediump vec3 normal) {
    if (packed.x == 0u && packed.y == 0u) {
        return vec4(0.0);
    }
    mediump vec3 ambient = vec3(uvec3(packed.x, packed.x >> 8u, packed.x >> 16u) & 0xFFu) * (1.0 / 255.0);
    mediump vec3 direct = vec3(uvec3(packed.y, packed.y >> 8u, packed.y >> 16u) & 0xFFu) * (1.0 / 255.0);
    mediump vec2 e = max(vec2(bitfieldExtract(int(packed.x), 24, 8), bitfieldExtract(int(packed.y), 24, 8)) / 127.0,
                         vec2(-1.0));
    mediump vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return vec4(min(ambient + direct * max(dot(normal, normalize(n)), 0.0), vec3(1.0)), 1.0); // saturated (Model2)
}

// An M2's own point light (ModelDraw::selfLight: a brazier's or a campfire's fire, reaching a yard or two) on its own
// vertices, model space: full up to the attenuation start, none from its end.
mediump vec3 selfLight(highp vec3 position, mediump vec3 normal, highp vec4 light, mediump vec4 color) {
    if (light.w <= 0.0) {
        return vec3(0.0);
    }
    highp vec3 toLight = light.xyz - position;
    highp float distance = length(toLight);
    mediump float attenuation = clamp((light.w - distance) / max(light.w - color.w, 1e-3), 0.0, 1.0);
    mediump float facing = max(dot(normalize(normal), toLight / max(distance, 1e-3)), 0.0);
    return color.rgb * (facing * attenuation);
}

// A WMO surface (pack::ModelVertex::color, v6): the light baked into its vertex colors (halved, 1.12's overbright
// included) over the building's ambient (MOHD), and the outdoor light by the vertex alpha: interior batches take the
// first, exterior ones the second (dark at night), transition batches at doors a blend of both.
mediump vec3 vertexColorLight(mediump vec4 vertexColor, mediump vec3 ambient, mediump vec3 normal) {
    mediump vec3 inside = vertexColor.rgb * 2.0 + ambient;
    if (vertexColor.a < 0.004) {
        return inside;
    }
    mediump vec3 outdoor = min(
        frame.ambientColor.rgb + frame.directColor.rgb * max(dot(normalize(normal), frame.lightDirection.xyz), 0.0),
        vec3(1.0));
    return mix(inside, outdoor, vertexColor.a);
}

// The fragment's light: the instance's own (an interior) replaces the frame's, an M2's own light adds to it; unlit
// materials keep theirs. The inner faces of windows (pack::kModelMaterialExteriorLight) take the outdoor light that
// comes through them: bright by day, dark at night, whichever way they face.
mediump vec3 applyInstanceLight(mediump vec3 light, mediump vec4 instance, uint materialFlags) {
    if ((materialFlags & 1u) != 0u) {
        return light;
    }
    if ((materialFlags & 128u) != 0u) {
        return min(frame.ambientColor.rgb + frame.directColor.rgb, vec3(1.0));
    }
    return instance.a > 0.5 ? instance.rgb : min(light + instance.rgb, vec3(1.0));
}

// Night glow: the WMO materials flagged SIDN (pack::kModelMaterialNightGlow: windows, lamp glass) add their emissive
// color as the night falls (frame.surfaceLight.x), after the saturated light like 1.12's emissive term.
mediump vec3 nightGlow(mediump vec3 texel, uint materialFlags, mediump vec3 emissive) {
    if ((materialFlags & 32u) == 0u) {
        return vec3(0.0);
    }
    return texel * emissive * frame.surfaceLight.x;
}
