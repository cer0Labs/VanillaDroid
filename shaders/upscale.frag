#version 320 es
precision mediump float;

// Dynamic resolution (phase 10): the scene stretched to the framebuffer with bilinear filtering, plus a light
// sharpening against the blur of the stretch (the centre against its four neighbours, kept inside their range so
// edges get no halos).

layout(set = 0, binding = 0) uniform mediump sampler2D uScene;

layout(push_constant) uniform Push {
    highp vec4 uvScale;
    highp vec4 texel;
} pc;

layout(location = 0) in highp vec2 vUV;
layout(location = 0) out vec4 outColor;

mediump vec3 scene(highp vec2 uv) {
    // Never past the scene's last texel: the rest of the image holds older frames.
    return texture(uScene, min(uv, pc.uvScale.zw)).rgb;
}

void main() {
    vec3 c = scene(vUV);
    if (pc.texel.z > 0.0) {
        vec3 n = scene(vUV - vec2(0.0, pc.texel.y));
        vec3 s = scene(vUV + vec2(0.0, pc.texel.y));
        vec3 w = scene(vUV - vec2(pc.texel.x, 0.0));
        vec3 e = scene(vUV + vec2(pc.texel.x, 0.0));
        vec3 lo = min(min(min(n, s), min(w, e)), c);
        vec3 hi = max(max(max(n, s), max(w, e)), c);
        c = clamp(c + (4.0 * c - (n + s + w + e)) * (0.25 * pc.texel.z), lo, hi);
    }
    outColor = vec4(c, 1.0);
}
