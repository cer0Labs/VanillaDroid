#version 320 es

// Touch UI (phase 7): quads in logical pixels (as the player sees the screen) -> clip space with the swapchain
// pre-rotation. Vertex layout: UiVertex (render/Renderer.h).

layout(push_constant) uniform Push {
    vec4 scaleOffset; // logical pixels -> [-1, 1]: xy scale, zw offset
    vec4 rotation;    // pre-rotation (2x2, column major)
    vec4 circle;      // UiMode::TextureCircle: center (logical px), radius
} pc;

layout(location = 0) in highp vec2 inPosition;
layout(location = 1) in highp vec2 inUV;
layout(location = 2) in mediump vec4 inColor;
layout(location = 3) in highp uint inParams;

layout(location = 0) out highp vec2 vUV;
layout(location = 1) out mediump vec4 vColor;
layout(location = 2) flat out highp uint vParams;
layout(location = 3) out highp vec2 vPosition;

void main() {
    vec2 ndc = inPosition * pc.scaleOffset.xy + pc.scaleOffset.zw;
    gl_Position = vec4(mat2(pc.rotation.xy, pc.rotation.zw) * ndc, 0.0, 1.0);
    vUV = inUV;
    vColor = inColor;
    vParams = inParams;
    vPosition = inPosition;
}
