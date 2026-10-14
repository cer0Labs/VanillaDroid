#version 320 es

// Texture composition (character skins): one layer as a rectangle of the target, in pixels.

layout(push_constant) uniform Push {
    vec4 rect;   // x, y, width, height in pixels
    vec2 target; // target size in pixels
} pc;

layout(location = 0) out highp vec2 vUV;

void main() {
    // Two triangles from the vertex index: (0,0) (1,0) (0,1) / (0,1) (1,0) (1,1).
    const vec2 corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, 1.0), vec2(1.0, 0.0),
                                    vec2(1.0, 1.0));
    vec2 uv = corners[gl_VertexIndex];
    vec2 pixel = pc.rect.xy + uv * pc.rect.zw;
    gl_Position = vec4(pixel / pc.target * 2.0 - 1.0, 0.0, 1.0);
    vUV = uv;
}
