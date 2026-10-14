#version 320 es
#extension GL_GOOGLE_include_directive : require

// Horizon silhouettes take the fog color, like the original client's distant mountains.
precision mediump float;

#include "frame.glsl"
#include "screenfx.glsl"

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(frame.fogColor.rgb, 1.0);
    outColor.rgb = screenFx(outColor.rgb); // phase 11: the client's full-screen effects
}
