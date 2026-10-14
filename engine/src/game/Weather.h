#pragma once

#include "core/Types.h"

#include <span>
#include <string_view>

namespace vd::game {

// SMSG_WEATHER's types (vmangos WeatherType). The 1.12 client draws the server's "storm" as a sandstorm: its
// sounds are SandStorm*.wav and its particles grains of sand (MapWeather.cpp: Rain, Snow, Sand).
enum class WeatherType : u8 { Fine = 0, Rain = 1, Snow = 2, Sandstorm = 3 };
constexpr u32 kWeatherTypes = 4;

// The loop the server names for a weather (vmangos Weather::GetSound): none under a grade of 0.3, then light,
// medium and heavy (SoundEntries "Weather - RainLight" 8533 ... "Weather - SandstormHeavy" 8558). Offline the
// development switch takes it from here.
u32 weatherSound(WeatherType type, f32 grade);

// What the server says the weather of the player's zone is.
struct WeatherState {
    WeatherType type = WeatherType::Fine;
    f32 grade = 0.0f; // intensity 0..1 (0 when fine)
    u32 sound = 0;    // SoundEntries id of its loop (8533-8538, 8556-8558), 0 none
    // Change at once instead of fading: the packet's flag, or its first after entering a world.
    bool instant = false;
};

// Development: a weather forced without a server, "<type>[:<grade>][:instant]" with type rain, snow, sand (or
// storm) or fine, e.g. "rain:0.8", "snow:1:instant", "fine" (VD_WEATHER, app/WeatherControl); the loop as the
// server would name it. False when malformed.
bool parseWeatherSpec(std::string_view spec, WeatherState& out);

// The weather of the player's zone (phase 11). The server sends SMSG_WEATHER whenever the player enters a
// zone (Player::UpdateZone: also right after logging in or a far teleport, fine where the zone has no
// weather) and to the whole zone when it changes (every 10 minutes, Weather::ReGenerate, or a GM's `.wchange
// <type> <grade>`); it always asks for a smooth change. A new world (login, another map) starts fine and
// takes its first packet at once: the loading screen hid the change, as the client shows the weather already
// there when it enters. The fade itself is the world's (world/WeatherEffect). Main thread; nothing allocates.
class Weather {
public:
    void enterWorld(); // login or another map
    void leaveWorld(); // logout, disconnection: fine

    // SMSG_WEATHER. False when malformed (nothing changes then).
    bool handle(std::span<const u8> payload);

    const WeatherState& state() const { return m_state; }
    // Packets applied so far: the world side starts a change when it moves.
    u32 changes() const { return m_changes; }

private:
    WeatherState m_state;
    bool m_firstInWorld = true;
    u32 m_changes = 0;
};

} // namespace vd::game
