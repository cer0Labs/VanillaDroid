#pragma once

#include "core/Types.h"

#include <glm/vec3.hpp>

#include <span>

// The sky's moving parts as the 1.12.1 client computes them (DayNight in WoW.exe, build 5875): the direction
// of the light that shades the world, the sun, the two moons, the stars' fade and the glare around the sun
// and the moon. Every curve below is a table of keys over the day that WoW.exe builds at startup; the values
// are copied from its constants. World\dnc.db, the light table in the MPQs, is not read by this build of the
// client. World axes: X north, Y west, Z up.
namespace vd::celestial {

// A key of a curve over the day: `time` is the fraction of the day (0 = midnight, 0.5 = noon).
struct Key {
    f32 time;
    f32 value;
};

// The client's curves are linear between keys and wrap past midnight (from the last key to the first).
// dayFraction is clamped to [0, 1].
f32 sampleDay(std::span<const Key> keys, f32 dayFraction);

// The sun and the moons are quads facing the screen at this distance from the camera; their sizes are the
// quads' sides at that distance (1 = 1/12 radian).
constexpr f32 kDistance = 12.0f;

struct Body {
    glm::vec3 direction{0.0f, 0.0f, 1.0f}; // unit vector from the camera
    f32 size = 1.0f;                       // side of the quad at kDistance
};

// Unit vector towards the light that shades the world: always from the north-west, 37 degrees high at noon
// and midnight and 20 at 06:00 and 18:00 (the moon lights the night from the same place).
glm::vec3 lightDirection(f32 dayFraction);

// The sun: rises in the north-west around 06:10, climbs to 5 degrees from the zenith at noon and sets where
// it rose around 20:30; twice as big near the horizon.
Body sun(f32 dayFraction);
// The White Lady: up from about 22:20 to 03:20, 55 degrees high at midnight, in the north-west.
Body whiteLady(f32 dayFraction);
// The Blue Child: the White Lady's path over its own 1.7-day cycle (`day` counts whole game days), drifting
// from the south-west to the south.
Body blueChild(f32 dayFraction, u32 day);

// Opacity of the stars: full from 00:00 to 03:00, gone between 04:30 and 22:30.
f32 starsAlpha(f32 dayFraction);

// Fraction of a body's quad below the camera's horizontal plane is cut, and it fades over its last
// kHorizonFade units (at kDistance) above it: alpha = clamp(height * kHorizonFadeRate, 0, 1).
constexpr f32 kHorizonFadeRate = 2.5f;

// The glare drawn over the scene around the sun and the White Lady: grows and brightens as the camera looks
// at it, fades in and out with the time of day and when something hides its body.
struct Glare {
    f32 scaleAway; // quad side (at kDistance, times the body's size for the moon) looking away from it
    f32 scaleAt;   // ... looking straight at it
    f32 alphaAway;
    f32 alphaAt;
    f32 rise;      // visibility per second towards 1 (towards 0: kGlareFall)
    f32 bodyScale; // 0: absolute sizes (sun); else the sides are the body's size times this (moon)
};
constexpr f32 kGlareFall = 1.5151515f; // per second
constexpr f32 kGlareCosine = 0.7f;     // looking further than acos(0.7) = 45.6 degrees away counts as "away"
extern const Glare kSunGlare;
extern const Glare kMoonGlare;
// The time-of-day fade of each glare: the sun's from 07:30 to 19:30 (out by 06:30 and 21:00), the moon's from
// 23:59 to 02:00 (out between 03:15 and 22:45).
f32 sunGlareFade(f32 dayFraction);
f32 moonGlareFade(f32 dayFraction);
// How much the camera looks at a body: 0 at kGlareCosine or further away, 1 straight at it.
f32 glareFacing(const glm::vec3& cameraForward, const glm::vec3& towardsBody);

// The clouds are lit by the sun from 04:50 to 22:10 and by the White Lady the rest of the night.
bool cloudsLitBySun(f32 dayFraction);
// How much of that light reaches them: none around the hand-over at dawn (04:40-04:50) and at dusk.
f32 cloudLightFade(f32 dayFraction);

// Whole days from 2000-01-01 to a date given as the server packs it (years since 2000, month 0-11, day of the
// month 0-30).
u32 daysSince2000(u32 years, u32 month, u32 day);

} // namespace vd::celestial
