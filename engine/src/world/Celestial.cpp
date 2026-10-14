#include "world/Celestial.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace vd::celestial {
namespace {

constexpr f32 kPi = 3.14159265f;
// Hours as fractions of the day, the unit of every key below.
constexpr f32 hours(f32 h) {
    return h / 24.0f;
}

// The light's polar angle (from the zenith, radians) of its travel direction: below the horizon (> 90
// degrees) means it shines downwards. Azimuth 225 degrees: it comes from the north-west.
constexpr std::array<Key, 4> kLightPolar{{{0.0f, kPi * 127.0f / 180.0f},
                                          {0.25f, kPi * 11.0f / 18.0f},
                                          {0.5f, kPi * 127.0f / 180.0f},
                                          {0.75f, kPi * 11.0f / 18.0f}}};
constexpr f32 kLightAzimuth = kPi * 1.25f;

// The bodies' polar angles (from the zenith): 100 degrees is 10 below the horizon.
constexpr f32 kBelow = kPi * 5.0f / 9.0f;
constexpr f32 kFiveMinutes = 1.0f / 288.0f;
constexpr std::array<Key, 5> kSunPolar{{{hours(5.5f), kBelow},
                                        {0.5f - kFiveMinutes, kPi / 36.0f},
                                        {0.5f, kPi / 36.0f},
                                        {0.5f + kFiveMinutes, kPi / 36.0f},
                                        {hours(21.5f), kBelow}}};
constexpr f32 kSunAzimuth = kPi * 0.25f;
constexpr std::array<Key, 4> kSunSize{
    {{hours(6.0f), 2.0f}, {hours(6.75f), 1.0f}, {hours(20.25f), 1.0f}, {hours(21.0f), 2.0f}}};

constexpr f32 kMoonHigh = kPi * 7.0f / 36.0f; // 35 degrees from the zenith
constexpr std::array<Key, 5> kMoonPolar{{{0.0f, kMoonHigh},
                                         {kFiveMinutes, kMoonHigh},
                                         {hours(4.0f), kBelow},
                                         {hours(22.0f), kBelow},
                                         {1.0f - kFiveMinutes, kMoonHigh}}};
constexpr f32 kMoonAzimuth = kPi * 0.25f;
constexpr f32 kWhiteLadySize = 1.75f;
// 0.0826389 of a day after 22:00 is 23:59 (WoW.exe's constant).
constexpr f32 kMoonLateKey = hours(22.0f) + 0.0826389f;
constexpr std::array<Key, 4> kMoonSize{
    {{hours(1.0f), 1.0f}, {hours(4.0f), 1.5f}, {hours(22.0f), 1.5f}, {kMoonLateKey, 1.0f}}};

constexpr f32 kBlueChildPeriod = 1.7f; // days
constexpr f32 kBlueChildSize = 1.0f;
constexpr std::array<Key, 3> kBlueChildAzimuth{
    {{0.0f, kPi * 0.75f}, {hours(4.0f), kPi * 5.0f / 6.0f}, {hours(22.0f), kPi * 11.0f / 12.0f}}};

constexpr std::array<Key, 4> kStarsAlpha{
    {{hours(3.0f), 1.0f}, {hours(4.5f), 0.0f}, {hours(22.5f), 0.0f}, {1.0f, 1.0f}}};
constexpr std::array<Key, 4> kSunGlareFade{
    {{hours(6.5f), 0.0f}, {hours(7.5f), 1.0f}, {hours(19.5f), 1.0f}, {hours(21.0f), 0.0f}}};
constexpr std::array<Key, 4> kMoonGlareFade{
    {{hours(2.0f), 1.0f}, {hours(3.25f), 0.0f}, {hours(22.75f), 0.0f}, {kMoonLateKey, 1.0f}}};
// WoW.exe's own table, keys in its order (the last four are not sorted; the search behaves as the client's).
constexpr f32 kFortyMinutes = 1.0f / 36.0f;
constexpr std::array<Key, 8> kCloudLight{{{hours(4.0f), 1.0f},
                                          {hours(4.0f) + kFortyMinutes, 0.0f},
                                          {hours(5.5f) - kFortyMinutes, 0.0f},
                                          {hours(5.5f), 1.0f},
                                          {hours(21.5f), 1.0f},
                                          {hours(21.5f) + kFortyMinutes, 0.0f},
                                          {hours(22.0f) - kFortyMinutes, 0.0f},
                                          {hours(22.0f), 1.0f}}};

glm::vec3 fromAngles(f32 polar, f32 azimuth) {
    const f32 s = std::sin(polar);
    return glm::vec3(s * std::cos(azimuth), s * std::sin(azimuth), std::cos(polar));
}

} // namespace

const Glare kSunGlare{3.0f, 20.0f, 0.5f, 1.0f, 4.0f, 0.0f};
const Glare kMoonGlare{1.0f, 1.0f, 0.1f, 1.0f, 3.030303f, 2.0f};

f32 sampleDay(std::span<const Key> keys, f32 dayFraction) {
    if (keys.empty()) {
        return 0.0f;
    }
    const f32 t = std::clamp(dayFraction, 0.0f, 1.0f);
    const usize count = keys.size();
    usize next = 0;
    while (next < count && t > keys[next].time) {
        ++next;
    }
    usize previous = 0;
    if (next == count) {
        next = 0;
        previous = count - 1;
    } else if (next == 0) {
        previous = count - 1;
    } else {
        previous = next - 1;
    }
    f32 span = keys[next].time - keys[previous].time;
    if (std::fabs(span) < 0.001f) {
        return keys[previous].value;
    }
    if (span < 0.0f) {
        span += 1.0f; // wraps past midnight
    }
    f32 into = t - keys[previous].time;
    if (into < 0.0f) {
        into += 1.0f;
    }
    return keys[previous].value + (keys[next].value - keys[previous].value) * (into / span);
}

glm::vec3 lightDirection(f32 dayFraction) {
    // The table gives the direction the light travels; towards the light is the opposite.
    return -fromAngles(sampleDay(kLightPolar, dayFraction), kLightAzimuth);
}

Body sun(f32 dayFraction) {
    return {fromAngles(sampleDay(kSunPolar, dayFraction), kSunAzimuth), sampleDay(kSunSize, dayFraction)};
}

Body whiteLady(f32 dayFraction) {
    return {fromAngles(sampleDay(kMoonPolar, dayFraction), kMoonAzimuth),
            sampleDay(kMoonSize, dayFraction) * kWhiteLadySize};
}

Body blueChild(f32 dayFraction, u32 day) {
    // Its own clock: the fraction of its 1.7-day cycle, run through the day's curves.
    const f64 days = static_cast<f64>(day) + static_cast<f64>(std::clamp(dayFraction, 0.0f, 1.0f));
    const f32 cycle =
        static_cast<f32>(std::fmod(days, static_cast<f64>(kBlueChildPeriod)) / kBlueChildPeriod);
    return {fromAngles(sampleDay(kMoonPolar, cycle), sampleDay(kBlueChildAzimuth, cycle)),
            sampleDay(kMoonSize, cycle) * kBlueChildSize};
}

f32 starsAlpha(f32 dayFraction) {
    return sampleDay(kStarsAlpha, dayFraction);
}

f32 sunGlareFade(f32 dayFraction) {
    return sampleDay(kSunGlareFade, dayFraction);
}

f32 moonGlareFade(f32 dayFraction) {
    return sampleDay(kMoonGlareFade, dayFraction);
}

bool cloudsLitBySun(f32 dayFraction) {
    return dayFraction >= hours(5.5f) - kFortyMinutes && dayFraction <= hours(21.5f) + kFortyMinutes;
}

f32 cloudLightFade(f32 dayFraction) {
    return sampleDay(kCloudLight, dayFraction);
}

u32 daysSince2000(u32 years, u32 month, u32 day) {
    // Days from the civil date (H. Hinnant's days_from_civil), minus those of 2000-01-01.
    auto daysFromCivil = [](i64 y, i64 m, i64 d) {
        y -= m <= 2 ? 1 : 0;
        const i64 era = (y >= 0 ? y : y - 399) / 400;
        const i64 yearOfEra = y - era * 400;
        const i64 dayOfYear = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const i64 dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
        return era * 146097 + dayOfEra;
    };
    const i64 days =
        daysFromCivil(2000 + i64{years}, i64{month} + 1, i64{day} + 1) - daysFromCivil(2000, 1, 1);
    return static_cast<u32>(std::max<i64>(days, 0));
}

f32 glareFacing(const glm::vec3& cameraForward, const glm::vec3& towardsBody) {
    const f32 cosine = std::max(glm::dot(cameraForward, towardsBody), kGlareCosine);
    return (cosine - kGlareCosine) / (1.0f - kGlareCosine);
}

} // namespace vd::celestial
