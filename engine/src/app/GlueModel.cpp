#include "app/GlueModel.h"

#include "core/Text.h"
#include "net/NetClient.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace vd {
namespace {

// RGBA8 as ui::rgba packs it (red in the low byte).
constexpr u32 color(f32 r, f32 g, f32 b) {
    return static_cast<u32>(r * 255.0f + 0.5f) | (static_cast<u32>(g * 255.0f + 0.5f) << 8) |
           (static_cast<u32>(b * 255.0f + 0.5f) << 16) | 0xFF000000u;
}
// GlueFonts.xml's colors.
constexpr u32 kNormalColor = color(1.0f, 0.82f, 0.0f);  // NORMAL_FONT_COLOR
constexpr u32 kGrayColor = color(0.5f, 0.5f, 0.5f);     // GRAY_FONT_COLOR
constexpr u32 kGreenColor = color(0.1f, 1.0f, 0.1f);    // GREEN_FONT_COLOR
constexpr u32 kRedColor = color(1.0f, 0.1f, 0.1f);      // RED_FONT_COLOR
constexpr u32 kBlueColor = color(0.0f, 0.749f, 0.953f); // BLUE_FONT_COLOR

char upper(char c) {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

bool lessNoCase(std::string_view a, std::string_view b) {
    const usize n = std::min(a.size(), b.size());
    for (usize i = 0; i < n; ++i) {
        const char x = upper(a[i]), y = upper(b[i]);
        if (x != y) {
            return x < y;
        }
    }
    return a.size() < b.size();
}

// The realm type's order for SortRealms("mode"): Normal, PVP, RP, RPPVP.
u32 modeRank(const RealmRow& row) {
    return (row.pvp ? 1u : 0u) + (row.rp ? 2u : 0u);
}

bool sameNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        if (upper(a[i]) != upper(b[i])) {
            return false;
        }
    }
    return true;
}

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && isSpace(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && isSpace(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

std::string_view unquoted(std::string_view s) {
    return s.size() >= 2 && s.front() == '"' && s.back() == '"' ? s.substr(1, s.size() - 2) : s;
}

// The first word of `line`, taken from it with the spaces after it.
std::string_view word(std::string_view& line) {
    usize end = 0;
    while (end < line.size() && !isSpace(line[end])) {
        ++end;
    }
    const std::string_view first = line.substr(0, end);
    line = trimmed(line.substr(end));
    return first;
}

} // namespace

std::string_view glueVersionText(std::span<char> out) {
    return text::format(out, "VERSION_TEMPLATE", text::get("BETA_BUILD"), kGlueVersion, kGlueBuild,
                        text::get("RELEASE_BUILD"), kGlueBuildDate);
}

f32 glueFade(f32 elapsed, f32 duration, bool in) {
    const f32 t = duration > 0.0f ? std::clamp(elapsed / duration, 0.0f, 1.0f) : 1.0f;
    return in ? t : 1.0f - t;
}

GlueStatus glueStatus(const GlueSessionState& s, std::span<char> scratch) {
    using Step = GlueSessionState::Step;
    GlueStatus out;
    // A failure first: OKAY over whatever screen the session is back at.
    if (!s.failure.empty() && s.step != Step::Reconnecting) {
        out.type = GlueDialogType::Okay;
        out.text = s.failure;
        return out;
    }
    if (s.realmListShown) {
        return out;
    }
    out.type = GlueDialogType::Cancel;
    out.action = GlueStatus::Action::Logout;
    switch (s.step) {
    case Step::Reconnecting: out.text = text::get("VD_LOGIN_RECONNECTING"); break;
    case Step::Connecting: out.text = text::get("LOGIN_STATE_CONNECTING"); break;
    case Step::Authenticating: out.text = text::get("LOGIN_STATE_AUTHENTICATING"); break;
    case Step::Authenticated: out.text = text::get("LOGIN_STATE_AUTHENTICATED"); break;
    case Step::WorldConnecting: out.text = text::get("GAME_SERVER_LOGIN"); break;
    case Step::Queued:
        // The client's queue text, with the realm's name; no time estimate from vMaNGOS: "Calculating...".
        out.text = s.realm.empty()
                       ? text::format(scratch, "QUEUE_TIME_LEFT_UNKNOWN", s.queuePosition)
                       : text::format(scratch, "QUEUE_NAME_TIME_LEFT_UNKNOWN", s.realm, s.queuePosition);
        out.button = text::get("CHANGE_REALM");
        out.action = GlueStatus::Action::ChangeRealm;
        break;
    case Step::CharacterList:
        if (s.realmListPending) {
            out.text = text::get("REALM_LIST_IN_PROGRESS");
            out.action = GlueStatus::Action::CancelRealmList;
        } else if (s.deleting) {
            out.text = text::get("CHAR_DELETE_IN_PROGRESS");
            out.action = GlueStatus::Action::Close;
        } else if (s.renaming) {
            out.text = text::get("CHAR_RENAME_IN_PROGRESS");
            out.action = GlueStatus::Action::Close;
        } else if (s.characterListPending) {
            out.text = text::get("CHAR_LIST_RETRIEVING");
        } else {
            out = {};
        }
        break;
    default: out = {}; break;
    }
    return out;
}

void realmType(u32 icon, bool& pvp, bool& rp) {
    // Cfg_Configs.dbc (1.12.1): realm type 1, 3, 5 and 10 PvP; 6 and 7 RP; 8 both; the rest normal.
    static constexpr u8 kPvp[11] = {0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1};
    static constexpr u8 kRp[11] = {0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0};
    pvp = icon < 11 && kPvp[icon] != 0;
    rp = icon < 11 && kRp[icon] != 0;
}

std::string_view realmTypeText(const RealmRow& row) {
    return text::get(row.pvp && row.rp ? "RPPVP_PARENTHESES"
                     : row.rp          ? "RP_PARENTHESES"
                     : row.pvp         ? "PVP_PARENTHESES"
                                       : "GAMETYPE_NORMAL");
}

u32 realmTypeColor(const RealmRow& row) {
    return row.rp && !row.pvp ? kGreenColor : row.pvp && !row.rp ? kRedColor : kNormalColor;
}

std::string_view realmLoadText(const RealmRow& row) {
    if (row.down) {
        return text::get("REALM_DOWN");
    }
    switch (row.load) {
    case RealmLoad::Recommended: return text::get("LOAD_RECOMMENDED");
    case RealmLoad::New: return text::get("LOAD_NEW");
    case RealmLoad::Full: return text::get("LOAD_FULL");
    case RealmLoad::High: return text::get("LOAD_HIGH");
    case RealmLoad::Low: return text::get("LOAD_LOW");
    case RealmLoad::Medium: break;
    }
    return text::get("LOAD_MEDIUM");
}

u32 realmLoadColor(const RealmRow& row) {
    if (row.down) {
        return kGrayColor;
    }
    switch (row.load) {
    case RealmLoad::Recommended: return kBlueColor;
    case RealmLoad::New:
    case RealmLoad::Low: return kGreenColor;
    case RealmLoad::Full:
    case RealmLoad::High: return kRedColor;
    case RealmLoad::Medium: break;
    }
    return kNormalColor;
}

u32 realmNameColor(const RealmRow& row) {
    return row.down             ? kGrayColor
           : row.invalid        ? kRedColor
           : row.characters > 0 ? kGreenColor
                                : color(1.0f, 0.78f, 0.0f);
}

u32 realmHighlightColor(const RealmRow& row) {
    return row.invalid ? kRedColor : row.characters > 0 ? kGreenColor : color(1.0f, 0.78f, 0.0f);
}

bool realmIsFull(const RealmRow& row) {
    return !row.down && row.load == RealmLoad::Full;
}

void RealmSort::by(RealmSortKey key) {
    u32 at = 0;
    while (at < keys.size() && keys[at].key != key) {
        ++at;
    }
    if (at == 0) {
        keys[0].descending = !keys[0].descending; // already first: the other way round
        return;
    }
    if (at == keys.size()) {
        return;
    }
    const Key moved = keys[at];
    for (u32 i = at; i > 0; --i) {
        keys[i] = keys[i - 1];
    }
    keys[0] = moved;
}

void realmRows(std::span<const net::RealmInfo> realms, std::string_view current, const RealmSort& sort,
               std::vector<RealmRow>& out) {
    out.clear();
    // The population's mean and 0.6 standard deviations (WoW.exe 0x46E66D: one realm or none, 1 and 0).
    f32 mean = 1.0f, spread = 0.0f;
    if (realms.size() > 1) {
        f32 sum = 0.0f;
        for (const net::RealmInfo& r : realms) {
            sum += r.population;
        }
        mean = sum / static_cast<f32>(realms.size());
        f32 squares = 0.0f;
        for (const net::RealmInfo& r : realms) {
            squares += (r.population - mean) * (r.population - mean);
        }
        spread = std::sqrt(squares / static_cast<f32>(realms.size() - 1)) * 0.6f;
    }
    for (usize i = 0; i < realms.size(); ++i) {
        const net::RealmInfo& r = realms[i];
        RealmRow row;
        row.realm = static_cast<u32>(i);
        row.name = r.name;
        realmType(r.icon, row.pvp, row.rp);
        row.invalid = (r.flags & net::kRealmFlagInvalid) != 0;
        row.down = r.offline();
        row.current = !current.empty() && r.name == current;
        row.characters = r.characters;
        if (r.flags & net::kRealmFlagNewPlayers) {
            row.load = RealmLoad::Recommended;
        } else if (r.flags & net::kRealmFlagRecommended) {
            row.load = RealmLoad::New;
        } else if (r.flags & net::kRealmFlagFull) {
            row.load = RealmLoad::Full;
        } else if (mean - spread > r.population) {
            row.load = RealmLoad::Low;
        } else if (mean + spread < r.population) {
            row.load = RealmLoad::High;
        } else {
            row.load = RealmLoad::Medium;
        }
        out.push_back(row);
    }
    std::stable_sort(out.begin(), out.end(), [&](const RealmRow& a, const RealmRow& b) {
        for (const RealmSort::Key& k : sort.keys) {
            i32 order = 0;
            switch (k.key) {
            case RealmSortKey::Name:
                order = lessNoCase(a.name, b.name) ? -1 : lessNoCase(b.name, a.name) ? 1 : 0;
                break;
            case RealmSortKey::Load: order = static_cast<i32>(a.load) - static_cast<i32>(b.load); break;
            case RealmSortKey::Characters:
                order = static_cast<i32>(a.characters) - static_cast<i32>(b.characters);
                break;
            case RealmSortKey::Mode:
                order = static_cast<i32>(modeRank(a)) - static_cast<i32>(modeRank(b));
                break;
            }
            if (order != 0) {
                return k.descending ? order > 0 : order < 0;
            }
        }
        return false;
    });
}

i32 selectedCharacter(u32 count, i32 lastIndex, CharacterListReason reason) {
    if (count == 0) {
        return -1;
    }
    switch (reason) {
    case CharacterListReason::Created: return static_cast<i32>(count) - 1;
    case CharacterListReason::Deleted: return 0;
    case CharacterListReason::Login: break;
    }
    return lastIndex >= 0 && lastIndex < static_cast<i32>(count) ? lastIndex : 0;
}

bool deleteConfirmed(std::string_view typed) {
    const std::string_view wanted = text::get("DELETE_CONFIRM_STRING");
    if (typed.size() != wanted.size() || wanted.empty()) {
        return false;
    }
    for (usize i = 0; i < typed.size(); ++i) {
        if (upper(typed[i]) != upper(wanted[i])) {
            return false;
        }
    }
    return true;
}

GlueConfig parseGlueConfig(std::string_view text) {
    GlueConfig config;
    config.loaded = true;
    if (text.substr(0, 3) == "\xEF\xBB\xBF") {
        text.remove_prefix(3); // a UTF-8 byte order mark (a realmlist.wtf saved with Notepad)
    }
    while (!text.empty()) {
        const usize end = text.find('\n');
        std::string_view line = trimmed(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        // SET <name> "<value>"
        if (!sameNoCase(word(line), "SET")) {
            continue;
        }
        const std::string_view name = word(line);
        const std::string_view value = unquoted(line);
        if (sameNoCase(name, "accountName")) {
            config.accountName.assign(value);
        } else if (sameNoCase(name, "realmName")) {
            config.realmName.assign(value);
        } else if (sameNoCase(name, "realmList")) {
            config.realmList.assign(trimmed(value));
        } else if (sameNoCase(name, "lastCharacterIndex")) {
            config.lastCharacterIndex = std::atoi(std::string(value).c_str());
        }
    }
    return config;
}

std::string writeGlueConfig(const GlueConfig& config) {
    std::string out;
    const auto set = [&](std::string_view name, std::string_view value) {
        out.append("SET ").append(name).append(" \"").append(value).append("\"\n");
    };
    set("accountName", config.accountName);
    set("realmName", config.realmName);
    set("realmList", config.realmList);
    set("lastCharacterIndex", std::to_string(config.lastCharacterIndex));
    return out;
}

std::string_view glueRealmList(std::string_view launch, std::string_view player,
                               std::string_view configured) {
    for (const std::string_view source : {launch, player, configured}) {
        if (const std::string_view address = trimmed(source); !address.empty()) {
            return address;
        }
    }
    return {};
}

bool glueNeedsServer(std::string_view server) {
    return trimmed(server).empty();
}

GlueServerEntry glueServerEntry(std::string_view typed, std::string_view fallback, bool forLogin) {
    GlueServerEntry out;
    std::string_view address = trimmed(typed);
    // A line of realmlist.wtf pasted whole, as servers publish it: set realmlist logon.example.org.
    std::string_view rest = address;
    if (sameNoCase(word(rest), "set") && sameNoCase(word(rest), "realmlist")) {
        address = rest;
    }
    address = trimmed(unquoted(address));
    if (address.empty()) {
        out.server = trimmed(fallback);
        out.close = !forLogin || !out.server.empty();
        out.error = out.close ? std::string_view{} : std::string_view("VD_LOGIN_ENTER_SERVER");
        return out;
    }
    std::string host;
    u16 port = 0;
    switch (net::parseRealmdAddress(address, host, port)) {
    case net::AddressError::None:
        out.close = true;
        out.realmList = address;
        out.server = address;
        break;
    case net::AddressError::NoHost: out.error = "VD_LOGIN_ENTER_SERVER"; break;
    case net::AddressError::BadPort: out.error = "VD_LOGIN_BAD_PORT"; break;
    }
    return out;
}

} // namespace vd
