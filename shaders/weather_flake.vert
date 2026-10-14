#version 320 es
#extension GL_GOOGLE_include_directive : require

// Snowflakes and grains of sand (phase 11): three vertices per particle, no vertex buffer. The client draws them as
// points (Vertex_snowpoint, Vertex_sand: SnowFlake01 and WeatherMistGrainy01 point sprites); here a triangle facing
// the camera holds the sprite's square (the fragment shader cuts it to the square), sized in the world with a
// smallest angular size so far ones stay a pixel or two wide. Snow fades in over its first second, settles on the
// ground and fades there in a quarter of a second (Vertex_snowpoint); sand fades in and out over a fifth of a second
// at both ends of its life (Vertex_sand).

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
    uint index = uint(gl_VertexIndex) / 3u;
    uint corner = uint(gl_VertexIndex) % 3u;
    if (index >= weather.particles.life.x) {
        hide();
        return;
    }
    WeatherParticle q = weatherParticle(weather.particles, index);
    vec4 fades = weather.particles.fades;
    float alpha = weather.particles.shape.w * clamp(q.age / max(fades.x, 1.0e-3), 0.0, 1.0) *
                  clamp((q.life - q.age) / max(fades.y, 1.0e-3), 0.0, 1.0);
    vec3 position = q.position;
    if (fades.z > 0.0) {
        // Landing: where it reached the ground it stays, fading.
        float fall = max(-q.velocity.z, 0.1);
        float since = (weatherGround(q.position.xy) - q.position.z) / fall;
        if (since > q.age) {
            hide();
            return;
        }
        if (since > 0.0) {
            position -= q.velocity * since;
            alpha *= clamp(1.0 - since / fades.z, 0.0, 1.0);
        }
    }
    // Faded right in front of the camera, where one would cover the view (MapWeather sets a c_zfade for snow).
    float distance = length(position);
    alpha *= smoothstep(0.5, 1.5, distance / max(weather.particles.shape.y, 0.1));
    if (alpha <= 0.002) {
        hide();
        return;
    }
    // Its size in the world, within the smallest and the largest angle on screen (the client's points shrink with
    // the distance up to a largest size, c_pointSize).
    float largest = weather.particles.color.a > 0.0 ? distance * weather.particles.color.a : 1.0e4;
    float halfSize = 0.5 * clamp(weather.particles.shape.y, distance * weather.particles.shape.z, largest);
    // The square is [-1, 1] half sizes around the particle; this triangle holds it.
    vec2 c = corner == 0u ? vec2(-2.0, -1.0) : (corner == 1u ? vec2(2.0, -1.0) : vec2(0.0, 3.0));
    vUV = corner == 0u ? vec2(-0.5, 1.0) : (corner == 1u ? vec2(1.5, 1.0) : vec2(0.5, -1.0));
    position += (weather.cameraRight.xyz * c.x + weather.cameraUp.xyz * c.y) * halfSize;
    vCell = vec4(0.0, 0.0, 1.0, 1.0);
    gl_Position = frame.viewProj * vec4(position, 1.0);
    vColor = vec4(weather.particles.color.rgb, alpha);
    vFog = fogFactor(position);
    vMask = vec3(c, fades.w); // in the square's half sizes; sand's grains are rounded off
}
