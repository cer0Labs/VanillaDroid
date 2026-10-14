#version 320 es
#extension GL_GOOGLE_include_directive : require

// Particles and ribbons (phase 9): quads the CPU built camera-relative (fx::ParticleSystem), with their color and
// texture cell. Vertex layout: ParticleVertex (render/Renderer.h).

#include "frame.glsl"

layout(push_constant) uniform Push {
    uvec4 params; // x: blend (pack::ModelBlend), y: kParticleDraw* flags
} pc;

layout(location = 0) in vec3 inPosition; // camera-relative
layout(location = 1) in vec2 inUV;       // unorm16
layout(location = 2) in vec4 inColor;    // unorm8, alpha = opacity

layout(location = 0) out highp vec2 vUV;
layout(location = 1) out mediump vec4 vColor;
layout(location = 2) out mediump float vFog;

const uint kUnfogged = 1u;
const uint kLit = 2u;

void main() {
    gl_Position = frame.viewProj * vec4(inPosition, 1.0);
    vUV = inUV;
    vec4 color = inColor;
    if ((pc.params.y & kLit) != 0u) {
        // Smoke and dust darken with the scene's light (no normal: half the sun).
        color.rgb *= min(frame.ambientColor.rgb + frame.directColor.rgb * 0.5, vec3(1.0));
    }
    vColor = color;
    vFog = (pc.params.y & kUnfogged) != 0u ? 0.0 : fogFactor(inPosition);
}
