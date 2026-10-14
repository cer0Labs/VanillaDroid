// Per-draw constants of the model and actor pipelines (vertex and fragment stages). Must match ModelPush in
// render/ModelRenderer.cpp.
layout(push_constant) uniform Push {
    uvec4 flags;  // x: material flags (pack::kModelMaterial*), y: 1 = WMO (vertex-color light), z: blend (pack::ModelBlend)
    vec4 ambient; // WMO interior ambient (MOHD)
    vec4 color;   // material color multiplier (animated on actors), white elsewhere
    highp vec4 uvRow0; // texture animation (phase 9): u' = dot(uvRow0.xyz, (u, v, 1)), identity when static
    highp vec4 uvRow1; // v' = dot(uvRow1.xyz, (u, v, 1))
    highp vec4 selfLight;      // phase 11: an M2's own point light: model-space position, w: attenuation end
    mediump vec4 selfLightColor; // color times intensity, w: attenuation start (zeros: none); WMO: night glow color
} pc;

highp vec2 animatedUV(highp vec2 uv) {
    highp vec3 p = vec3(uv, 1.0);
    return vec2(dot(pc.uvRow0.xyz, p), dot(pc.uvRow1.xyz, p));
}
