#version 320 es
precision mediump float;

layout(set = 0, binding = 0) uniform mediump sampler2D uTexture;

layout(location = 0) in highp vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(uTexture, vUV);
}
