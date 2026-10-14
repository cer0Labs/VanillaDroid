#include "app/Pairing.h"

#include "app/PhoneBakePlan.h" // phase 22 (phonebake)
#include "core/Json.h"
#include "core/Locale.h"

#include <fstream>
#include <iterator>
#include <system_error>

namespace vd {
namespace {

constexpr i64 kFormat = 1;
constexpr usize kKeyDigits = 32;

char lowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equalsIgnoringCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        if (lowerAscii(a[i]) != lowerAscii(b[i])) {
            return false;
        }
    }
    return true;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = lowerAscii(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// %XX and '+' decoded; false for a malformed escape or a control character.
bool percentDecode(std::string_view in, std::string& out) {
    out.clear();
    for (usize i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '%') {
            if (i + 2 >= in.size()) {
                return false;
            }
            const int high = hexValue(in[i + 1]);
            const int low = hexValue(in[i + 2]);
            if (high < 0 || low < 0) {
                return false;
            }
            c = static_cast<char>(high * 16 + low);
            i += 2;
        } else if (c == '+') {
            c = ' ';
        }
        if (static_cast<unsigned char>(c) < 0x20) {
            return false;
        }
        out.push_back(c);
    }
    return true;
}

// The decoded value of a query parameter; nullopt when absent or malformed.
std::optional<std::string> queryValue(std::string_view query, std::string_view name) {
    while (!query.empty()) {
        const usize amp = query.find('&');
        const std::string_view pair = query.substr(0, amp);
        const usize equals = pair.find('=');
        if (pair.substr(0, equals) == name) {
            std::string value;
            if (equals == std::string_view::npos || !percentDecode(pair.substr(equals + 1), value)) {
                return std::nullopt;
            }
            return value;
        }
        if (amp == std::string_view::npos) {
            break;
        }
        query.remove_prefix(amp + 1);
    }
    return std::nullopt;
}

std::string lowered(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = lowerAscii(c);
    }
    return out;
}

} // namespace

bool isPairingKey(std::string_view key) {
    if (key.size() != kKeyDigits) {
        return false;
    }
    for (const char c : key) {
        if (hexValue(c) < 0) {
            return false;
        }
    }
    return true;
}

bool PairedPc::valid() const {
    return endpoint.valid() && isPairingKey(key);
}

std::optional<PairedPc> parsePairingLink(std::string_view uri) {
    // vanilladroid://pair?h=192.168.1.20:47800&k=<32 hex>  (also "vanilladroid://pair/?...")
    constexpr std::string_view kScheme = "vanilladroid://";
    if (uri.size() < kScheme.size() || !equalsIgnoringCase(uri.substr(0, kScheme.size()), kScheme)) {
        return std::nullopt;
    }
    std::string_view rest = uri.substr(kScheme.size());
    const usize question = rest.find('?');
    if (question == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view host = rest.substr(0, question);
    if (!host.empty() && host.back() == '/') {
        host.remove_suffix(1);
    }
    if (!equalsIgnoringCase(host, "pair")) {
        return std::nullopt;
    }
    std::string_view query = rest.substr(question + 1);
    if (const usize hash = query.find('#'); hash != std::string_view::npos) {
        query = query.substr(0, hash);
    }
    const std::optional<std::string> address = queryValue(query, "h");
    const std::optional<std::string> key = queryValue(query, "k");
    PairedPc pc;
    if (!address || !key || !isPairingKey(*key) ||
        !net::parseLocalEndpoint(*address, kSetupPort, pc.endpoint)) {
        return std::nullopt;
    }
    pc.key = lowered(*key);
    return pc;
}

std::string pairingCode(std::string_view typed) {
    std::string code;
    for (const char c : typed) {
        if (c >= '0' && c <= '9') {
            code.push_back(c);
        } else if (c != ' ' && c != '-') {
            return {};
        }
    }
    return code.size() == kPairingCodeDigits ? code : std::string();
}

std::string writePairingFile(const PairedPc& pc) {
    json::Value root = json::Value::object();
    root.set("format", json::Value::number(kFormat));
    root.set("host", json::Value::string(pc.endpoint.host));
    root.set("port", json::Value::number(pc.endpoint.port));
    root.set("key", json::Value::string(pc.key));
    root.set("pending", json::Value::boolean(pc.pending));
    root.set("wow", json::Value::boolean(pc.includeWow));
    return json::write(root);
}

std::string writeForgottenPairing() {
    json::Value root = json::Value::object();
    root.set("format", json::Value::number(kFormat));
    return json::write(root);
}

std::optional<PairedPc> readPairingFile(std::string_view text) {
    Result<json::Value> document = json::parse(text);
    if (!document || document.value().get("format").asInt() != kFormat) {
        return std::nullopt;
    }
    const json::Value& root = document.value();
    PairedPc pc;
    const i64 port = root.get("port").asInt();
    const std::string address =
        std::string(root.get("host").asString()) + ":" + std::to_string(port > 0 && port <= 65535 ? port : 0);
    if (!net::parseLocalEndpoint(address, kSetupPort, pc.endpoint)) {
        return std::nullopt;
    }
    pc.key = lowered(root.get("key").asString());
    pc.pending = root.get("pending").asBool();
    pc.includeWow = root.get("wow").asBool();
    if (!pc.valid()) {
        return std::nullopt;
    }
    return pc;
}

std::optional<PairedPc> loadPairing(const std::filesystem::path& storageDir) {
    std::ifstream in(storageDir / std::string(kPairingFile), std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return readPairingFile(text);
}

bool hasGameData(const std::filesystem::path& packsDir) {
    std::error_code error;
    for (const std::string_view pack : kRequiredPacks) {
        const std::filesystem::path path = packsDir / std::string(pack);
        if (!std::filesystem::is_regular_file(path, error) || std::filesystem::file_size(path, error) == 0 ||
            error) {
            return false;
        }
    }
    for (const auto& entry : std::filesystem::directory_iterator(packsDir, error)) {
        const std::string name = entry.path().filename().string();
        if (name.size() > 11 && name.compare(0, 6, "world_") == 0 &&
            name.compare(name.size() - 5, 5, ".vdpk") == 0 && entry.is_regular_file(error) &&
            entry.file_size(error) > 0) {
            return true;
        }
    }
    return false;
}

bool needsFirstRun(const std::filesystem::path& packsDir, const std::filesystem::path& storageDir) {
    if (!hasGameData(packsDir)) {
        return true;
    }
    // Phase 22 (phonebake): a first bake on the phone puts its first packs in place long before its last.
    if (phoneFirstBakeUnfinished(packsDir.parent_path() / "bake")) {
        return true;
    }
    const std::optional<PairedPc> paired = storageDir.empty() ? std::nullopt : loadPairing(storageDir);
    return paired && paired->pending;
}

bool hasWowOnDevice(const std::filesystem::path& wowDataDir) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(wowDataDir, error)) {
        const std::string name = entry.path().filename().string();
        if (name.size() > 4 && equalsIgnoringCase(std::string_view(name).substr(name.size() - 4), ".mpq") &&
            entry.is_regular_file(error)) {
            return true;
        }
    }
    return false;
}

std::string firstRunLocale(std::string_view configured, std::string_view systemLanguage) {
    if (!configured.empty()) {
        const std::string_view canonical = canonicalLocale(configured);
        return std::string(canonical.empty() ? configured : canonical);
    }
    // "es", "es-MX", "es_ES": the language's two letters, then the first client language of it ("es" ->
    // "esES", whose texts we have, also for a Mexican phone; "fr" -> "frFR").
    if (systemLanguage.size() >= 2) {
        const std::string language = lowered(systemLanguage.substr(0, 2));
        for (const std::string_view locale : kClientLocales) {
            if (locale.substr(0, 2) == language) {
                return std::string(locale);
            }
        }
    }
    return "enUS";
}

} // namespace vd
