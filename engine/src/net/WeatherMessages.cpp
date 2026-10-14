#include "net/WeatherMessages.h"

#include "net/ByteBuffer.h"

#include <cmath>

namespace vd::net {

bool parseWeather(std::span<const u8> payload, WeatherUpdate& out) {
    ByteReader r(payload);
    out.type = r.u32v();
    out.grade = r.f32v();
    out.sound = r.u32v();
    out.instant = r.u8v() != 0; // ByteBuffer << bool: one byte
    if (r.ok() && (out.type >= kWeatherTypes || !std::isfinite(out.grade))) {
        r.fail(); // a type the client does not know, or a grade that is no number
    }
    return r.ok();
}

} // namespace vd::net
