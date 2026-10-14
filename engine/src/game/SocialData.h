#pragma once

#include "core/Types.h"
#include "game/Social.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vd::pack {
class PackReader;
}

namespace vd::game {

// The client tables of the social side (phase 8), from db.vdpk: zones and their names (AreaTable), the automatic
// chat channels (ChatChannels), the text emotes with their lines (EmotesText, EmotesTextData) and the names of the
// races and classes (ChrRaces, ChrClasses). Indexed once at start-up; lookups do not allocate (zoneChannels() reuses
// its storage).
class SocialData {
public:
    // EmotesText lines (m_emoteText): who sees which. A %s stands for the emoter, then the target.
    enum EmoteText : u32 {
        kEmoteOthersTarget = 0, // "%s waves at %s."
        kEmoteToYou = 1,        // "%s waves at you."
        kEmoteYouTarget = 2,    // "You wave at %s."
        kEmoteOthers = 4,       // "%s waves."
        kEmoteYou = 6,          // "You wave."
    };
    struct TextEmote {
        u32 id = 0;
        std::string_view token; // "WAVE": the slash command, lower case
        u32 emote = 0;          // Emotes.dbc animation
        std::array<u32, 16> texts{};
    };

    bool init(const pack::PackReader& db);
    bool ready() const { return m_ready; }

    // The zone an area belongs to (itself for a zone), 0 when unknown.
    u32 zoneOf(u32 areaId) const;
    std::string_view areaName(u32 areaId) const; // enUS, as the client data has it
    // A capital with a trade channel (AREA_FLAG_SLAVE_CAPITAL).
    bool isCity(u32 zoneId) const;
    // A race's and a class's name (ChrRaces / ChrClasses, enUS as the client data has them: "Night Elf",
    // "Warrior"); empty when unknown.
    std::string_view raceName(u32 race) const;
    std::string_view className(u32 cls) const;
    // Retouches (friends frame): what a who query's tags look in (game/WhoQuery): the races' and classes' names by id,
    // and the top-level zones (AreaTable rows without a parent) in the table's order with their names.
    std::span<const std::string_view> raceNames() const { return m_raceNames; }
    std::span<const std::string_view> classNames() const { return m_classNames; }
    std::span<const u32> topZones() const { return m_topZones; }
    std::span<const std::string_view> topZoneNames() const { return m_topZoneNames; }

    // The channels the original client keeps the player in while in `zoneId`, in number order: General, Trade
    // (empty name outside the cities: the kind keeps its number), LocalDefense and LookingForGroup. The names
    // stay valid until the next call. Phase 21: named with the base client's tables ("Trade - City" with a language
    // overlay too): the server matches them against its own ChatChannels.dbc, which is enUS.
    std::span<const ZoneChannel> zoneChannels(u32 zoneId);
    // The ChatChannels id of a channel by its name (as the server matches it), 0 for a custom channel.
    u32 channelKind(std::string_view name) const;
    // Phase 21 (languages): a zone channel's name as the player reads it, from its name as the server knows it
    // ("2. Trade - City" -> "2. Comercio - Ciudad", an optional "N. " kept); a custom channel's, or any without a
    // language overlay, as it is. Into `out` (or `name` itself).
    std::string_view displayChannel(std::string_view name, std::span<char> out) const;
    // Its shown name without the zone, as ChatFrame cuts it before a line (gsub(arg4, "%s%-%s.*", ""): "Trade - City"
    // -> "Trade"; the esES client names it "Comercio: Ciudad", which that cut leaves whole). Into `out`.
    std::string_view displayChannelBase(std::string_view name, std::span<char> out) const;

    const TextEmote* textEmote(u32 id) const;
    const TextEmote* textEmoteByToken(std::string_view token) const; // case insensitive
    // Phase 21 (languages): the text emote a typed command does in the client's language (GlobalStrings'
    // EMOTE<n>_CMD<m> = "/vale" and ChatFrame.lua's EMOTE<n>_TOKEN = "AGREE", both in the strings table), as
    // ChatFrame.lua's hash_EmoteTokenList; `word` without its '/'. Nothing without a strings table.
    const TextEmote* textEmoteByCommand(std::string_view word) const;
    // The line `variant` (EmoteText) of a text emote, empty when it has none.
    std::string_view textEmoteLine(u32 id, u32 variant) const;
    const std::vector<TextEmote>& textEmotes() const { return m_emotes; }

private:
    struct Area {
        u32 zone = 0;
        u32 flags = 0;
        std::string_view name;
        std::string_view wireName; // phase 21: the base client's ("Elwynn Forest" under "Bosque de Elwynn")
    };
    struct Channel {
        u32 id = 0;
        u32 flags = 0;
        std::string_view pattern;     // "General - %s"
        std::string_view wirePattern; // phase 21: the base client's
    };

    std::unordered_map<u32, Area> m_areas;
    std::string_view m_cityName; // AREA_FLAG_CITY's name: "City" (Trade - City)
    std::string_view m_wireCityName;
    std::unordered_map<std::string_view, std::string_view> m_displayAreaNames; // phase 21: wire -> shown
    bool m_translatedChannels = false; // phase 21: a language overlay renames them
    std::vector<Channel> m_channels;
    std::vector<TextEmote> m_emotes;
    std::vector<std::string> m_tokens; // lower-case tokens the emotes point into
    std::unordered_map<u32, std::string_view> m_emoteLines;
    std::array<std::string, 4> m_zoneNames;
    std::array<ZoneChannel, 4> m_zoneChannels{};
    std::array<std::string_view, 16> m_raceNames{}, m_classNames{};
    std::vector<u32> m_topZones; // retouches (friends frame)
    std::vector<std::string_view> m_topZoneNames;
    bool m_ready = false;
};

} // namespace vd::game
