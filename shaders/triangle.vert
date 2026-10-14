#version 320 es

// Phase 0 smoke test: a swinging triangle, no vertex buffers.
// Vertex shaders keep the ES default (highp float) for positions; varyings can be mediump.

layout(push_constant) uniform PushConstants {
    vec4 preRotate;   // 2x2 column-major: swapchain pre-rotation (Android surface transform)
    vec2 aspectScale; // keeps the triangle undistorted in the logical (rotated) viewport
    float angle;      // swing angle in radians
} pc;

layout(location = 0) out mediump vec3 vColor;

const vec2 kPositions[3] = vec2[](vec2(0.0, -0.6), vec2(0.6, 0.45), vec2(-0.6, 0.45));
const vec3 kColors[3] = vec3[](vec3(1.0, 0.35, 0.2), vec3(0.2, 1.0, 0.35), vec3(0.25, 0.45, 1.0));

void main() {
    float c = cos(pc.angle);
    float s = sin(pc.angle);
    vec2 p = mat2(c, s, -s, c) * kPositions[gl_VertexIndex];
    p *= pc.aspectScale;
    gl_Position = vec4(mat2(pc.preRotate.xy, pc.preRotate.zw) * p, 0.5, 1.0);
    vColor = kColors[gl_VertexIndex];
}
