#version 320 es
#extension GL_GOOGLE_include_directive : require

// Rain (phase 11): three vertices per drop, no vertex buffer. Part 0 draws the drops as the client's Vertex_rain
// does: a triangle with two corners at the head, `rain.x` to each side across the view, and the third `rain.y`
// behind it along its fall (vtxCoordsY / vtxCoordsZ, texCoords (0,1) (1,1) (0.5,0) over RainDrop01), the offsets
// turned by the motion blur rotation. The head stops on the ground; the tail falls on into it until the drop is gone.
// Part 1 draws its splash as Vertex_patter does, once the drop is on the ground: RainDropSplash01's 4 frames at 16 a
// second, from the row that matches how steeply the camera looks down at it (row 0 seen from above, 3 from the
// side), centred on the impact the more the camera looks down. Here the triangle circumscribes the frame's square
// and the fragment shader cuts it to the frame.

#include "frame.glsl"
#include "weather.glsl"

layout(push_constant) uniform Push {
    uvec4 params; // x: 0 the drops, 1 their splashes
} pc;

layout(location = 0) out highp vec2 vUV;        // in the texture's cell (outside 0..1: cut)
layout(location = 1) flat out highp vec4 vCell; // xy: the cell's corner in the texture, zw: its size
layout(location = 2) out mediump vec4 vColor;
layout(location = 3) out mediump float vFog;
layout(location = 4) out highp vec3 vMask;

void hide() {
    gl_Position = kWeatherHidden;
    vUV = vec2(0.0);
    vCell = vec4(0.0);
    vColor = vec4(0.0);
    vFog = 0.0;
    vMask = vec3(0.0);
}

void main() {
    uint drop = uint(gl_VertexIndex) / 3u;
    uint corner = uint(gl_VertexIndex) % 3u;
    if (drop >= weather.particles.life.x) {
        hide();
        return;
    }
    WeatherParticle q = weatherParticle(weather.particles, drop);
    float fall = max(-q.velocity.z, 0.1);
    float since = (weatherGround(q.position.xy) - q.position.z) / fall; // since it reached the ground (< 0: falling)
    if (since > q.age) {
        hide(); // under the ground since its life began
        return;
    }
    vec3 position;
    if (pc.params.x == 0u) {
        float tail = weather.rain.y;
        if (since * fall >= tail) {
            hide();
            return;
        }
        vec3 head = since > 0.0 ? q.position - q.velocity * since : q.position;
        vec3 back = -normalize(q.velocity);
        vec3 side = cross(normalize(-head), back);
        float across = length(side);
        side = across > 1.0e-4 ? side / across : weather.cameraRight.xyz;
        vec3 offset;
        if (corner == 2u) {
            position = q.position;
            offset = back * tail;
            vUV = vec2(0.5, 0.0);
        } else {
            position = head;
            offset = side * (corner == 0u ? -weather.rain.x : weather.rain.x);
            vUV = vec2(corner == 0u ? 0.0 : 1.0, 1.0);
        }
        position += vec3(dot(weather.blur[0].xyz, offset), dot(weather.blur[1].xyz, offset),
                         dot(weather.blur[2].xyz, offset));
        vCell = vec4(0.0, 0.0, 1.0, 1.0);
    } else {
        if (since < 0.0 || since >= weather.rain.w) {
            hide();
            return;
        }
        vec3 at = q.position - q.velocity * since;
        float distance = max(length(at), 1.0e-3);
        float down = max(-at.z / distance, 0.0);
        float row = floor((1.0 - down) * 3.99);
        float column = min(floor(since * 15.96), 3.0);
        float halfSize = 0.5 * max(weather.rain.z, distance * weather.particles.shape.z);
        // The square of the frame is [-1, 1] x [0, 2] half sizes, standing on the impact; this triangle holds it.
        vec2 c = corner == 0u ? vec2(-2.0, 0.0) : (corner == 1u ? vec2(2.0, 0.0) : vec2(0.0, 4.0));
        vUV = corner == 0u ? vec2(-0.5, 1.0) : (corner == 1u ? vec2(1.5, 1.0) : vec2(0.5, -1.0));
        position = at + vec3(0.0, 0.0, down / 12.0) +
                   (weather.cameraRight.xyz * c.x + weather.cameraUp.xyz * (c.y - down)) * halfSize;
        vCell = vec4(column * 0.25, row * 0.25, 0.25, 0.25);
    }
    gl_Position = frame.viewProj * vec4(position, 1.0);
    vColor = vec4(weather.particles.color.rgb, pc.params.x == 0u ? weather.particles.shape.w : weather.cameraRight.w);
    vFog = fogFactor(position);
    vMask = vec3(0.0);
}
