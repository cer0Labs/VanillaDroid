#version 320 es

// Project convention: GLSL ES. Fragment shaders default to mediump (RelaxedPrecision in SPIR-V,
// FP16 on Adreno/Mali); use highp explicitly only where precision matters.
precision mediump float;

layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(vColor, 1.0);
}
