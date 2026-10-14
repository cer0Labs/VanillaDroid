// Weather (phase 11): the frame's rain, snow or sand (set 1, binding 0). Must match WeatherUniforms and
// WeatherParticles in render/Renderer.h (std140). Vertex shaders only: everything moves here, from the frame's clock.
//
// Like the 1.12 client's weather packets (MapWeather.cpp, Vertex_rain / _patter / _snowpoint / _sand), a particle
// is a start position, a velocity and a time of birth: where it is now is start + velocity * age. Here the start
// comes from a seed instead of a vertex buffer: particle i lives `life` (plus a random part of its own), and each
// life starts at a new random place of a box the size of `box`, in world space modulo the box, so that the box can
// follow the camera while what is in it stays where it is in the world. All the arithmetic is on small numbers: the
// camera arrives as its position modulo the box, the clock as integer ticks of 0.1 ms.

struct WeatherParticles {
    vec4 box;      // xyz: the box (yards); w: its bottom under the camera
    vec4 phase;    // xyz: the camera's world position modulo the box
    vec4 velocity; // xyz: shared (yd/s); w: random horizontal speed of each
    vec4 shape;    // x: random vertical speed, y: size (yd), z: smallest size (radians), w: opacity
    vec4 fades;    // x: fade in, y: fade out before the end, z: fade once landed (0: never lands); w: height (mists),
                   // 1 = rounded off (grains of sand)
    vec4 color;    // rgb: lit color; a: largest size (radians, 0: none)
    uvec4 life;    // x: count, y: life, z: random extra life (0.1 ms ticks), w: seed
};

layout(std140, set = 1, binding = 0) uniform WeatherBlock {
    uvec4 time;        // x: clock (0.1 ms ticks), y: kind (1 rain, 2 snow, 3 sand)
    vec4 cameraRight;  // world; w: the splashes' opacity
    vec4 cameraUp;     // world; w: the camera's world z
    vec4 rain;         // x: half width of a drop, y: its tail, z: splash size (yd), w: splash time (s)
    vec4 blur[3];      // the drops' motion blur rotation (rows)
    vec4 ground;       // xy: the camera's world position modulo the ground's span, z: point spacing, w: known radius
    WeatherParticles particles;
    WeatherParticles mist;
    vec4 heights[256]; // kWeatherGroundSide (32) squared world z, -1e4 where nothing is known
} weather;

const float kWeatherNoGround = -1.0e4;
const vec4 kWeatherHidden = vec4(2.0, 2.0, 2.0, 1.0); // outside the clip volume: the whole triangle is dropped

uint weatherHash(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

float weatherUnit(uint h) {
    return float(h >> 8u) * (1.0 / 16777216.0);
}

// A particle this frame.
struct WeatherParticle {
    vec3 position; // camera-relative
    vec3 velocity;
    float age;     // seconds into its life
    float life;    // seconds
    uint seed;     // of this life
};

WeatherParticle weatherParticle(WeatherParticles p, uint index) {
    uint h = weatherHash(index * 0x9E3779B9u + p.life.w);
    uint life = p.life.y + (p.life.z > 0u ? h % (p.life.z + 1u) : 0u);
    uint clock = weather.time.x + weatherHash(h ^ 0x68E31DA4u) % life;
    uint cycle = clock / life;
    WeatherParticle q;
    q.age = float(clock - cycle * life) * 1.0e-4;
    q.life = float(life) * 1.0e-4;
    q.seed = weatherHash(h ^ (cycle * 0x85EBCA6Bu));
    vec3 start = vec3(weatherUnit(weatherHash(q.seed + 1u)), weatherUnit(weatherHash(q.seed + 2u)),
                      weatherUnit(weatherHash(q.seed + 3u))) * p.box.xyz;
    vec3 own = vec3(weatherUnit(weatherHash(q.seed + 4u)), weatherUnit(weatherHash(q.seed + 5u)),
                    weatherUnit(weatherHash(q.seed + 6u))) * 2.0 - 1.0;
    q.velocity = p.velocity.xyz + vec3(own.xy * p.velocity.w, own.z * p.shape.x);
    vec3 moved = start + q.velocity * q.age - p.phase.xyz;
    q.position.xy = mod(moved.xy + 0.5 * p.box.xy, p.box.xy) - 0.5 * p.box.xy;
    q.position.z = mod(moved.z + p.box.w, p.box.z) - p.box.w;
    return q;
}

float weatherGroundPoint(ivec2 point) {
    ivec2 slot = point & 31; // the side is 32: world point modulo the side, also for negative points
    int i = slot.y * 32 + slot.x;
    return weather.heights[i >> 2][i & 3];
}

// The ground under a camera-relative point, relative to the camera; kWeatherNoGround where it is not known. Between
// four known points, bilinear; across a step (a roof's edge, a wall) the nearest point, so a splash does not hang in
// the air between the two.
float weatherGround(vec2 position) {
    if (abs(position.x) > weather.ground.w || abs(position.y) > weather.ground.w) {
        return kWeatherNoGround;
    }
    vec2 p = (position + weather.ground.xy) / weather.ground.z;
    vec2 cell = floor(p);
    vec2 t = p - cell;
    ivec2 g = ivec2(cell);
    float h00 = weatherGroundPoint(g);
    float h10 = weatherGroundPoint(g + ivec2(1, 0));
    float h01 = weatherGroundPoint(g + ivec2(0, 1));
    float h11 = weatherGroundPoint(g + ivec2(1, 1));
    float lowest = min(min(h00, h10), min(h01, h11));
    if (lowest < -9000.0) {
        return kWeatherNoGround;
    }
    float h;
    if (max(max(h00, h10), max(h01, h11)) - lowest > 1.5) {
        h = t.y < 0.5 ? (t.x < 0.5 ? h00 : h10) : (t.x < 0.5 ? h01 : h11);
    } else {
        h = mix(mix(h00, h10, t.x), mix(h01, h11, t.x), t.y);
    }
    return h - weather.cameraUp.w;
}
