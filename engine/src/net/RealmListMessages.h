#pragma once

#include "core/Types.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vd::net {

// Phase 18 (glue): realmd's realm list (CMD_REALM_LIST) as vmangos-core 4641790 writes it for a 1.x client
// (src/realmd/AuthSocket.cpp _HandleRealmList and LoadRealmlistAndWriteIntoBuffer, builds before 6299): the
// command byte and a u16 size, then u32 0, u8 count and per realm u32 icon (the realm type: Cfg_Configs.dbc's
// second column), u8 flags (REALM_FLAG_*, src/shared/Common.h), the name and "host:port" (cstrings), f32
// population, u8 the account's characters there, u8 category (Cfg_Categories), u8 0; after them u16 2.

constexpr u8 kCmdRealmList = 0x10;

// REALM_FLAG_* of vMaNGOS. The 1.12 client reads the population marks with other names (WoW.exe 0x46EC60, the
// load GetRealmInfo returns): 0x20 is "Recommended" (-3), 0x40 "New" (-2), 0x80 "Full" (2).
constexpr u8 kRealmFlagInvalid = 0x01; // GetRealmInfo's invalidRealm: red in RealmList
constexpr u8 kRealmFlagOffline = 0x02; // realmDown: "Offline", gray, not selectable
// vMaNGOS already appends " (1,12,1)" to the name for 1.x clients.
constexpr u8 kRealmFlagSpecifyBuild = 0x04;
constexpr u8 kRealmFlagNewPlayers = 0x20;
constexpr u8 kRealmFlagRecommended = 0x40;
constexpr u8 kRealmFlagFull = 0x80;

struct RealmInfo {
    u32 icon = 0;
    u8 flags = 0; // REALM_FLAG_* (src/shared/Common.h)
    std::string name;
    std::string address; // "host:port" of its world server
    f32 population = 0.0f;
    u8 characters = 0;
    u8 category = 0;
    bool offline() const { return (flags & kRealmFlagOffline) != 0; }
};

// The request: the command and a u32 0 (realmd skips 4 bytes, AuthSocket::_HandleRealmList).
void writeRealmListRequest(std::vector<u8>& out);
// The answer's body (after the command byte and the u16 size, up to its trailing u16). Nullopt when it is
// truncated.
std::optional<std::vector<RealmInfo>> parseRealmList(std::span<const u8> body);

// The realm the session connects to by itself after logging in, or -1 when the player has to choose in
// RealmList: `preferred` (the last realm connected to: Config.wtf's realmName, else client.cfg's realm.name)
// when it is listed and up, else the only realm when there is just one and it is up.
i32 pickRealm(std::span<const RealmInfo> realms, std::string_view preferred);

} // namespace vd::net
