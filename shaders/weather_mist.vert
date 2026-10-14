#version 320 es
#extension GL_GOOGLE_include_directive : require

// Weather mists (phase 11, the client's Mists): a few big soft sprites facing the camera, six vertices each, no vertex
// buffer, drifting with the wind a little over the ground and fading in and out over their life; turned at random so
// the same blob does not repeat, and faded as the camera comes into them so none fills the screen.

#include "frame.glsl"
#include "weather.glsl"

layout(push_constant) uniform Push {
    uvec4 params; // unused
} pc;

layout(location = 0) out highp vec2 vUV;
layout(location = 1) flat out highp vec4 vCell;
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
    uint sprite = uint(gl_VertexIndex) / 6u;
    uint corner = uint(gl_VertexIndex) % 6u;
    if (sprite >= weather.mist.life.x) {
        hide();
        return;
    }
    WeatherParticle q = weatherParticle(weather.mist, sprite);
    vec4 fades = weather.mist.fades;
    float ground = weatherGround(q.position.xy);
    float lift = weatherUnit(weatherHash(q.seed + 7u));
    float halfSize = weather.mist.shape.y * (0.75 + 0.5 * weatherUnit(weatherHash(q.seed + 8u)));
    // Its rounded-off edge just over the ground: where a sprite goes under the ground a hard line shows.
    vec3 center = vec3(q.position.xy, (ground > -9000.0 ? ground : -2.0) + 0.85 * halfSize + fades.w * (1.0 + lift));
    float distance = length(center);
    float alpha = weather.mist.shape.w * clamp(q.age / max(fades.x, 1.0e-3), 0.0, 1.0) *
                  clamp((q.life - q.age) / max(fades.y, 1.0e-3), 0.0, 1.0) *
                  smoothstep(0.4 * halfSize, 1.2 * halfSize, distance);
    if (alpha <= 0.002) {
        hide();
        return;
    }
    // Two triangles: (-1,-1) (1,-1) (1,1), (-1,-1) (1,1) (-1,1).
    const vec2 kCorners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0),
                                     vec2(1.0, 1.0), vec2(-1.0, 1.0));
    vec2 c = kCorners[corner];
    float angle = 6.2831853 * weatherUnit(weatherHash(q.seed + 9u)) + 0.05 * q.age;
    vec2 turned = mat2(cos(angle), sin(angle), -sin(angle), cos(angle)) * c;
    vUV = turned * 0.5 * 0.7071 + 0.5; // the turned square stays inside the texture
    vCell = vec4(0.0, 0.0, 1.0, 1.0);
    vec3 position = center + (weather.cameraRight.xyz * c.x + weather.cameraUp.xyz * c.y) * halfSize;
    gl_Position = frame.viewProj * vec4(position, 1.0);
    vColor = vec4(weather.mist.color.rgb, alpha);
    vFog = fogFactor(position);
    vMask = vec3(c, 1.0);
}
