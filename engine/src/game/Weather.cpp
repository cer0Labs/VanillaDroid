#include "game/Weather.h"

#include "core/Log.h"
#include "net/WeatherMessages.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vd::game {

u32 weatherSound(WeatherType type, f32 grade) {
    u32 first = 0; // the light loop; medium and heavy follow it
    switch (type) {
    case WeatherType::Rain: first = 8533; break;
    case WeatherType::Snow: first = 8536; break;
    case WeatherType::Sandstorm: first = 8556; break;
    case WeatherType::Fine: return 0;
    }
    if (grade < 0.3f) {
        return 0;
    }
    return grade < 0.6f ? first : (grade < 0.9f ? first + 1 : first + 2);
}

bool parseWeatherSpec(std::string_view spec, WeatherState& out) {
    const usize colon = spec.find(':');
    const std::string_view name = spec.substr(0, colon);
    std::string_view rest = colon == std::string_view::npos ? std::string_view{} : spec.substr(colon + 1);
    WeatherState state;
    if (name == "fine") {
        state.type = WeatherType::Fine;
    } else if (name == "rain") {
        state.type = WeatherType::Rain;
    } else if (name == "snow") {
        state.type = WeatherType::Snow;
    } else if (name == "sand" || name == "storm" || name == "sandstorm") {
        state.type = WeatherType::Sandstorm;
    } else {
        return false;
    }
    state.grade = state.type == WeatherType::Fine ? 0.0f : 1.0f;
    if (!rest.empty()) {
        const usize next = rest.find(':');
        const std::string_view number = rest.substr(0, next);
        rest = next == std::string_view::npos ? std::string_view{} : rest.substr(next + 1);
        // std::from_chars for floats is missing from some standard libraries.
        char buffer[32];
        const usize length = std::min(number.size(), sizeof(buffer) - 1);
        std::copy_n(number.data(), length, buffer);
        buffer[length] = '\0';
        f32 grade = 0.0f;
        char extra = 0;
        if (length == 0 || std::sscanf(buffer, "%f%c", &grade, &extra) != 1 || !std::isfinite(grade)) {
            return false;
        }
        state.grade = state.type == WeatherType::Fine ? 0.0f : std::clamp(grade, 0.0f, 1.0f);
    }
    if (!rest.empty()) {
        if (rest != "instant") {
            return false;
        }
        state.instant = true;
    }
    state.sound = weatherSound(state.type, state.grade);
    out = state;
    return true;
}

void Weather::enterWorld() {
    m_state = {};
    m_state.instant = true;
    m_firstInWorld = true;
    ++m_changes;
}

void Weather::leaveWorld() {
    enterWorld();
}

bool Weather::handle(std::span<const u8> payload) {
    net::WeatherUpdate update;
    if (!net::parseWeather(payload, update)) {
        return false;
    }
    m_state.type = static_cast<WeatherType>(update.type);
    // Fine comes with the server's smallest grade (Weather::NormalizeGrade: 0.0001): no weather at all.
    m_state.grade = m_state.type == WeatherType::Fine ? 0.0f : std::clamp(update.grade, 0.0f, 1.0f);
    m_state.sound = update.sound;
    m_state.instant = update.instant || m_firstInWorld;
    m_firstInWorld = false;
    ++m_changes;
    // The client's own line (WoW.exe: "Weather changed to %d, intensity %f").
    VD_LOG_INFO("Weather changed to %u, intensity %f (sound %u%s)", update.type,
                static_cast<f64>(m_state.grade), update.sound, m_state.instant ? ", at once" : "");
    return true;
}

} // namespace vd::game
