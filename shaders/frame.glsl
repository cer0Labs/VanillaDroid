// Per-frame scene constants (set 0, binding 0). Must match FrameUniforms in render/Renderer.cpp (std140).
// Colors come from Light.dbc and are gamma-space, like the original client.
layout(std140, set = 0, binding = 0) uniform Frame {
    highp mat4 viewProj;   // camera-relative view * projection * pre-rotation
    vec4 lightDirection;   // xyz: unit vector towards the sun (or the moon at night)
    vec4 directColor;
    vec4 ambientColor;
    vec4 fogColor;
    highp vec4 fogParams;  // x = start, y = 1 / (end - start), z = time (s), w = end
    vec4 sky[5];           // zenith -> horizon
    vec4 riverClose;
    vec4 riverFar;
    vec4 oceanClose;
    vec4 oceanFar;
    vec4 waterAlphas;      // river shallow, river deep, ocean shallow, ocean deep
    // Phase 11: the rest of Light.dbc's bands and the conditions that selected them.
    vec4 shadowColor;
    vec4 sunColor;         // w: celestial glow
    vec4 sunHalo;          // w: cloud density
    vec4 cloudEmissive;    // w: LightParams.glow
    vec4 cloudLayer1;      // w: weather intensity
    vec4 cloudLayer2;      // w: time of day, 0..1 from midnight
    vec4 conditions;       // x: under water, y: ghost (0 or 1); z: phase 15, a model frame (coverage alpha); w: free
    vec4 screenFx;         // phase 11 (screenfx.glsl): x = full-screen glow weight, y = the ghost's grading (0 or 1)
    vec4 surfaceLight;     // phase 11 (f11/interiors): x: night glow 0..1, y: highlights (TERRAIN_HIGHLIGHTS) on,
                           // z: the terrain textures carry specular masks; w: free
} frame;

highp float fogFactor(highp vec3 cameraRelative) {
    return clamp((length(cameraRelative) - frame.fogParams.x) * frame.fogParams.y, 0.0, 1.0);
}
