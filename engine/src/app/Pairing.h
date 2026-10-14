#pragma once

#include "core/Types.h"
#include "net/HttpClient.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Phase 22 (pairing, ROADMAP 22.4, ADR-0005 §5): the app and VanillaDroid Setup on the PC. The phone learns
// the PC's address and its pairing key from the link of the PC's page
// (vanilladroid://pair?h=<ip>:<port>&k=<key>, the QR code's way) or from an address and the 6-digit code
// typed by hand (exchanged for the key: GET /pair?code=), and keeps them in its internal storage
// (pairing.json; never the external one, which a cable reaches). Pure rules and formats (tests); the
// downloads are app/DataDownload, the screen app/FirstRunScreens.
namespace vd {

inline constexpr u16 kSetupPort = 47800; // VanillaDroid Setup's first port (vdsetup serve)
inline constexpr std::string_view kPairingFile = "pairing.json";
inline constexpr u32 kPairingCodeDigits = 6;

// The PC the phone downloads from.
struct PairedPc {
    net::HttpEndpoint endpoint;
    std::string key;         // the pairing key: 32 hex digits, lowercase
    bool pending = false;    // a download from it began and did not end
    bool includeWow = false; // the player chose to copy the WoW's archives too
    bool valid() const;
};
// 32 hex digits (a 128-bit key), any case.
bool isPairingKey(std::string_view key);

// A link of the PC's page: the PC, or nothing for a link that is not ours, lacks the key or names an address
// outside the local network (net::isLocalNetworkAddress). The scheme and the host are matched in any case; h
// and k are percent-decoded; other parameters are ignored.
std::optional<PairedPc> parsePairingLink(std::string_view uri);
// The code typed on the phone, its digits only ("123 456" -> "123456"); empty when it is not
// kPairingCodeDigits digits.
std::string pairingCode(std::string_view typed);

// pairing.json: {"format": 1, "host": "192.168.1.20", "port": 47800, "key": "...", "pending": false, "wow":
// false}. An empty or damaged file, or a forgotten PC ({"format": 1}), reads as none.
std::string writePairingFile(const PairedPc& pc);
std::string writeForgottenPairing();
std::optional<PairedPc> readPairingFile(std::string_view text);
std::optional<PairedPc> loadPairing(const std::filesystem::path& storageDir);

// What the game cannot start without: the tables, the textures, the interface, the models, the characters and
// creatures, and the terrain of at least one map (world_*.vdpk). The rest (sound, collision, grass...) only
// takes something away while it is missing.
inline constexpr std::array<std::string_view, 5> kRequiredPacks = {"db.vdpk", "textures.vdpk", "ui.vdpk",
                                                                   "models.vdpk", "actors.vdpk"};
bool hasGameData(const std::filesystem::path& packsDir);
// Whether the game opens on the first-run screen: no game data, or a download from a paired PC left half way,
// or (phase 22, phonebake) a first bake on the phone that has not ended (<packsDir>/../bake).
bool needsFirstRun(const std::filesystem::path& packsDir, const std::filesystem::path& storageDir);
// The WoW's archives copied to the phone (an .MPQ in wow/Data, by `vdsetup adb send-wow` or the download):
// baking on the phone (ROADMAP 22.7).
bool hasWowOnDevice(const std::filesystem::path& wowDataDir);

// The first-run screen's language: the configured one (client.cfg's locale or VD_LOCALE; empty when neither),
// else the system's ("es", "es-MX": the first client language of it, "esES") when the client has one, else
// English.
std::string firstRunLocale(std::string_view configured, std::string_view systemLanguage);

} // namespace vd
