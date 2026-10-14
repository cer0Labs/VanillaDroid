#version 320 es
#extension GL_GOOGLE_include_directive : require

// Sky dome around the camera, generated from gl_VertexIndex (no buffers): the 1.12 client's rings (sky_dome.glsl)
// colored with the 5 Light.dbc sky bands from the top down to 1.8 degrees, the fog color at the horizon and below.
// Drawn after the opaque world at depth 1.0, so only pixels nothing else covered pass the depth test.

#include "frame.glsl"
#include "sky_dome.glsl"

layout(location = 0) out mediump vec3 vColor;

void main() {
    int ring;
    vec3 position = skyDomeVertex(gl_VertexIndex, ring);
    vec4 clip = frame.viewProj * vec4(position * 10.0, 1.0);
    gl_Position = clip.xyww; // on the far plane

    vColor = ring < 5 ? frame.sky[ring].rgb : frame.fogColor.rgb;
}
