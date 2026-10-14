// Weather (phase 11): SMSG_WEATHER byte for byte as vmangos-core 4641790 writes it
// (Weather::SendWeatherForPlayersInZone, Misc::WeatherUpdate: u32 type, f32 grade, u32 sound, u8 instant),
// every truncation and the values the client does not know refused; the zone's weather through game::Gameplay
// (a new world starts fine and takes its first weather at once, later ones fade); the loops the server names
// (Weather::GetSound); the development switch's spec.
#include "game/Entities.h"
#include "game/Gameplay.h"
#include "game/Weather.h"
#include "net/ByteBuffer.h"
#include "net/WeatherMessages.h"

#include <doctest/doctest.h>

#include <limits>
#include <span>
#include <vector>

namespace {

using namespace vd;
using net::Opcode;
using Bytes = std::vector<u8>;
using game::WeatherType;

constexpr u64 kPlayer = 0x21;

Bytes weatherPacket(u32 type, f32 grade, u32 sound, u8 instant) {
    Bytes out;
    net::ByteWriter(out).u32v(type).f32v(grade).u32v(sound).u8v(instant);
    return out;
}

} // namespace

TEST_CASE("SMSG_WEATHER as the server writes it") {
    // `.wchange 1 0.75`: rain, smooth (the server always sends 0), the medium loop.
    const Bytes rain = weatherPacket(1, 0.75f, 8534, 0);
    REQUIRE(rain.size() == 13);
    net::WeatherUpdate update;
    REQUIRE(net::parseWeather(rain, update));
    CHECK(update.type == net::kWeatherRain);
    CHECK(update.grade == doctest::Approx(0.75f));
    CHECK(update.sound == 8534u);
    CHECK_FALSE(update.instant);
    REQUIRE(net::parseWeather(weatherPacket(3, 0.9999f, 8558, 1), update));
    CHECK(update.type == net::kWeatherStorm);
    CHECK(update.instant);
    // Fine: the server's smallest grade (Weather::NormalizeGrade) and no loop.
    REQUIRE(net::parseWeather(weatherPacket(0, 0.0001f, 0, 0), update));
    CHECK(update.type == net::kWeatherFine);
    CHECK(update.sound == 0u);

    for (usize n = 0; n < rain.size(); ++n) {
        CHECK_FALSE(net::parseWeather(std::span<const u8>(rain.data(), n), update));
    }
    CHECK_FALSE(net::parseWeather(weatherPacket(4, 0.5f, 0, 0), update)); // no such weather
    CHECK_FALSE(net::parseWeather(weatherPacket(1, std::numeric_limits<f32>::quiet_NaN(), 0, 0), update));
}

TEST_CASE("The zone's weather through Gameplay") {
    game::EntityStore store;
    game::Gameplay play(store);
    play.setSink({[](void*, Opcode, std::span<const u8>) { return true; }, nullptr});
    const u32 now = 1000;
    play.enterWorld(kPlayer, 0, true, now);
    const game::Weather& weather = play.weather();
    // A new world: fine at once until the server tells.
    CHECK(weather.state().type == WeatherType::Fine);
    CHECK(weather.state().instant);
    u32 changes = weather.changes();

    // Its first packet (Player::UpdateZone right after the login) at once, the next ones smooth.
    REQUIRE(play.handle(Opcode::SMSG_WEATHER, weatherPacket(1, 0.5f, 8533, 0), now) ==
            game::Gameplay::Handled::Yes);
    CHECK(weather.changes() == ++changes);
    CHECK(weather.state().type == WeatherType::Rain);
    CHECK(weather.state().grade == doctest::Approx(0.5f));
    CHECK(weather.state().sound == 8533u);
    CHECK(weather.state().instant);
    REQUIRE(play.handle(Opcode::SMSG_WEATHER, weatherPacket(2, 0.8f, 8537, 0), now) ==
            game::Gameplay::Handled::Yes);
    CHECK(weather.changes() == ++changes);
    CHECK(weather.state().type == WeatherType::Snow);
    CHECK_FALSE(weather.state().instant);
    // A zone without weather sends fine with the smallest grade: none at all.
    REQUIRE(play.handle(Opcode::SMSG_WEATHER, weatherPacket(0, 0.0001f, 0, 0), now) ==
            game::Gameplay::Handled::Yes);
    CHECK(weather.state().type == WeatherType::Fine);
    CHECK(weather.state().grade == 0.0f);
    // Grades out of range are clamped; a malformed packet changes nothing.
    REQUIRE(play.handle(Opcode::SMSG_WEATHER, weatherPacket(3, 1.5f, 8558, 1), now) ==
            game::Gameplay::Handled::Yes);
    CHECK(weather.state().type == WeatherType::Sandstorm);
    CHECK(weather.state().grade == 1.0f);
    CHECK(weather.state().instant); // the packet's own flag
    changes = weather.changes();
    CHECK(play.handle(Opcode::SMSG_WEATHER, Bytes{1, 0, 0}, now) == game::Gameplay::Handled::Malformed);
    CHECK(weather.changes() == changes);
    CHECK(weather.state().type == WeatherType::Sandstorm);

    // Another map (SMSG_NEW_WORLD): fine at once, and the new zone's first weather at once again.
    play.enterWorld(kPlayer, 1, false, now);
    CHECK(weather.state().type == WeatherType::Fine);
    CHECK(weather.state().instant);
    REQUIRE(play.handle(Opcode::SMSG_WEATHER, weatherPacket(1, 0.2f, 0, 0), now) ==
            game::Gameplay::Handled::Yes);
    CHECK(weather.state().instant);
    CHECK(weather.state().sound == 0u); // light rain has no loop
    // Out of the world: none.
    play.leaveWorld();
    CHECK(weather.state().type == WeatherType::Fine);
    CHECK(weather.state().grade == 0.0f);
}

TEST_CASE("The weather's loops as the server names them") {
    // Weather::GetSound: none under 0.3, light under 0.6, medium under 0.9, then heavy.
    CHECK(game::weatherSound(WeatherType::Rain, 0.29f) == 0u);
    CHECK(game::weatherSound(WeatherType::Rain, 0.3f) == 8533u);
    CHECK(game::weatherSound(WeatherType::Rain, 0.6f) == 8534u);
    CHECK(game::weatherSound(WeatherType::Rain, 0.9f) == 8535u);
    CHECK(game::weatherSound(WeatherType::Snow, 0.5f) == 8536u);
    CHECK(game::weatherSound(WeatherType::Snow, 0.7f) == 8537u);
    CHECK(game::weatherSound(WeatherType::Snow, 0.9999f) == 8538u);
    CHECK(game::weatherSound(WeatherType::Sandstorm, 0.4f) == 8556u);
    CHECK(game::weatherSound(WeatherType::Sandstorm, 0.8f) == 8557u);
    CHECK(game::weatherSound(WeatherType::Sandstorm, 1.0f) == 8558u);
    CHECK(game::weatherSound(WeatherType::Fine, 1.0f) == 0u);
}

TEST_CASE("The development weather switch") {
    game::WeatherState state;
    REQUIRE(game::parseWeatherSpec("rain:0.8", state));
    CHECK(state.type == WeatherType::Rain);
    CHECK(state.grade == doctest::Approx(0.8f));
    CHECK(state.sound == 8534u);
    CHECK_FALSE(state.instant);
    REQUIRE(game::parseWeatherSpec("snow:1:instant", state));
    CHECK(state.type == WeatherType::Snow);
    CHECK(state.grade == 1.0f);
    CHECK(state.instant);
    REQUIRE(game::parseWeatherSpec("sand", state));
    CHECK(state.type == WeatherType::Sandstorm);
    CHECK(state.grade == 1.0f);
    CHECK(state.sound == 8558u);
    REQUIRE(game::parseWeatherSpec("storm:0.2", state));
    CHECK(state.type == WeatherType::Sandstorm);
    CHECK(state.sound == 0u);
    REQUIRE(game::parseWeatherSpec("fine", state));
    CHECK(state.type == WeatherType::Fine);
    CHECK(state.grade == 0.0f);
    CHECK_FALSE(game::parseWeatherSpec("hail:1", state));
    CHECK_FALSE(game::parseWeatherSpec("rain:lots", state));
    CHECK_FALSE(game::parseWeatherSpec("rain:0.5x", state));
    CHECK_FALSE(game::parseWeatherSpec("rain:0.5:soon", state));
    CHECK_FALSE(game::parseWeatherSpec("", state));
}
