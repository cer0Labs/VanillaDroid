#pragma once

#include "core/Types.h"

#include <array>
#include <span>
#include <string>
#include <string_view>

namespace vd::game {

// Retouches (friends frame): the text of a who query (WhoFrameEditBox, /who) read as 1.12's SendWho does before it
// sends CMSG_WHO (WoW.exe 0x5aebb0; PROTOCOL §2.32):
//  * only the first 127 bytes count; whitespace separates the words, a double quote turns on and off a stretch where
//    it does not (the quote itself goes), and a word left inside an unclosed quote is dropped; case stays as typed;
//  * a word starting with WHO_TAG_NAME "n-" (the tags compared without case, A-Z only) sets the name and "g-" the
//    guild (at most 47 bytes, the last one wins); "z-" adds the top-level zones (AreaTable rows without a parent, in
//    their order) whose name contains the rest, up to 10 zones (none found and none stored before: zone 0, which
//    matches nobody); "r-" and "c-" add to the race and class masks every ChrRaces / ChrClasses row whose name contains
//    the rest (the first one clears the mask "any" first: "c-war" is warriors and warlocks);
//  * a word starting with a digit sets the levels: "60" is 60..60, "10-20" 10..20, "10-" 10..100 (the rest of the
//    word ignored); "-20" sets the highest alone;
//  * any other word is searched as it is (the server looks for it in the names, the guilds and the zones); the
//    fourth one ends the reading: what follows is ignored.
// The defaults: levels 0..100, every race and class, no zone, nothing searched.
struct WhoQuery {
    u32 minLevel = 0, maxLevel = 100;
    std::string name, guild;
    u32 raceMask = 0xFFFFFFFFu, classMask = 0xFFFFFFFFu;
    std::array<u32, 10> zones{};
    u32 zoneCount = 0;
    std::array<std::string, 4> terms{};
    u32 termCount = 0;
};

// The names the tags look in.
struct WhoQueryNames {
    std::span<const std::string_view> races;     // ChrRaces' names by race id (empty: no such row)
    std::span<const std::string_view> classes;   // ChrClasses' by class id
    std::span<const u32> zones;                  // the top-level zones' AreaTable ids, in the table's order...
    std::span<const std::string_view> zoneNames; // ...and their names
};

void parseWhoQuery(std::string_view text, const WhoQueryNames& names, WhoQuery& out);

// WhoFrame_GetDefaultWhoCommand (FrameXML): what an empty /who asks, the player's zone and the levels around theirs:
// z-"Elwynn Forest" 9-15.
std::string defaultWhoQuery(std::string_view zone, u32 level);

} // namespace vd::game
