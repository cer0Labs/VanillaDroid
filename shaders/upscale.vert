#version 320 es

// Dynamic resolution (phase 10): one triangle over the whole framebuffer that samples the scene, drawn into the
// top-left part of its image. Both images are in the display's native orientation: no pre-rotation here.

layout(push_constant) uniform Push {
    vec4 uvScale; // xy: the scene's share of its image; zw: the largest coordinate sampled (half a texel inside)
    vec4 texel;   // xy: one texel of the scene image; z: sharpening (0 = plain bilinear)
} pc;

layout(location = 0) out highp vec2 vUV;

void main() {
    // (0, 0), (2, 0), (0, 2): covers the framebuffer with one triangle.
    vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
    vUV = corner * pc.uvScale.xy;
}
