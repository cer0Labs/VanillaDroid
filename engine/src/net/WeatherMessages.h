#pragma once

#include "core/Types.h"

#include <span>

// The weather packet as vmangos-core 4641790 writes it for build 5875 (Server/Packets/Misc: WeatherUpdate;
// sent by Weather::SendWeatherUpdateToPlayer when the player enters a zone (Player::UpdateZone) and by
// Weather::SendWeatherForPlayersInZone when the zone's weather changes: every 10 minutes
// (Weather::ReGenerate) or by `.wchange`). Same rules as net/GameMessages: the parser returns false on a
// malformed packet and never reads past its end.
namespace vd::net {

// WeatherType (SharedDefines.h). The 1.12 client draws "storm" as a sandstorm (its sounds are
// SandStorm*.wav).
constexpr u32 kWeatherFine = 0;
constexpr u32 kWeatherRain = 1;
constexpr u32 kWeatherSnow = 2;
constexpr u32 kWeatherStorm = 3;
constexpr u32 kWeatherTypes = 4;

// SMSG_WEATHER (13 bytes).
struct WeatherUpdate {
    u32 type = kWeatherFine; // kWeather*
    f32 grade = 0.0f;        // intensity 0..1 (the server keeps it in [0.0001, 0.9999])
    u32 sound = 0;           // SoundEntries id of the loop (Weather::GetSound: none below a grade of 0.3)
    bool instant = false;    // 1 = change at once, 0 = smooth change (the server always sends 0)
};
bool parseWeather(std::span<const u8> payload, WeatherUpdate& out);

} // namespace vd::net
