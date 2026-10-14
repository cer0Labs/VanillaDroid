#include "SetupSettings.h"

#include "SetupCommon.h"
#include "core/Json.h"
#include "core/Sha256.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace vd::setup {
namespace {

constexpr i64 kFormat = 1;

std::string pathText(const std::filesystem::path& path) {
    return path.empty() ? std::string() : utf8FromPath(path);
}

std::filesystem::path environmentPath(const char* name) {
#if defined(_WIN32)
    // _wgetenv: the variable as the system stores it (UTF-16), whatever the code page.
    std::wstring wide(name, name + std::char_traits<char>::length(name));
    const wchar_t* value = _wgetenv(wide.c_str());
    return value && *value ? std::filesystem::path(value) : std::filesystem::path();
#else
    const char* value = std::getenv(name);
    return value && *value ? std::filesystem::path(value) : std::filesystem::path();
#endif
}

} // namespace

Result<Settings> Settings::load(const std::filesystem::path& file) {
    Settings settings;
    std::error_code error;
    if (!std::filesystem::exists(file, error)) {
        return settings;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return Error{"Cannot open " + utf8FromPath(file)};
    }
    std::ostringstream content;
    content << in.rdbuf();
    Result<json::Value> parsed = json::parse(content.str());
    if (!parsed) {
        return Error{utf8FromPath(file) + ": " + parsed.error().message};
    }
    const json::Value& root = parsed.value();
    if (root.get("format").asInt() != kFormat) {
        return Error{utf8FromPath(file) + ": unknown format"};
    }
    settings.language = root.get("language").asString();
    settings.wow = pathFromUtf8(root.get("wow").asString());
    settings.localeClient = pathFromUtf8(root.get("localeClient").asString());
    for (const json::Value& name : root.get("includeArchives").items()) {
        settings.includeArchives.emplace_back(name.asString());
    }
    settings.packs = pathFromUtf8(root.get("packs").asString());
    const i64 port = root.get("port").asInt(kDefaultPort);
    settings.port = port > 0 && port < 65536 ? static_cast<u16>(port) : kDefaultPort;
    settings.pairingKey = root.get("pairingKey").asString();
    settings.adb = pathFromUtf8(root.get("adb").asString());
    settings.apk = pathFromUtf8(root.get("apk").asString());
    settings.appId = root.get("appId").asString(kDistAppId);
    settings.serial = root.get("serial").asString();
    settings.lastBake = root.get("lastBake").asString();
    return settings;
}

Result<void> Settings::save(const std::filesystem::path& file) const {
    json::Value root = json::Value::object();
    root.set("format", json::Value::number(kFormat));
    root.set("language", json::Value::string(language));
    root.set("wow", json::Value::string(pathText(wow)));
    root.set("localeClient", json::Value::string(pathText(localeClient)));
    json::Value& archives = root.set("includeArchives", json::Value::array());
    for (const std::string& name : includeArchives) {
        archives.push(json::Value::string(name));
    }
    root.set("packs", json::Value::string(pathText(packs)));
    root.set("port", json::Value::number(port));
    root.set("pairingKey", json::Value::string(pairingKey));
    root.set("adb", json::Value::string(pathText(adb)));
    root.set("apk", json::Value::string(pathText(apk)));
    root.set("appId", json::Value::string(appId));
    root.set("serial", json::Value::string(serial));
    root.set("lastBake", json::Value::string(lastBake));
    const std::string text = json::write(root);

    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::filesystem::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            return Error{"Cannot write " + utf8FromPath(temp)};
        }
    }
    std::filesystem::rename(temp, file, error);
    if (error) {
        std::filesystem::remove(temp, error);
        return Error{"Cannot replace " + utf8FromPath(file)};
    }
    return {};
}

std::filesystem::path dataDir(const std::filesystem::path& overrideDir) {
    if (!overrideDir.empty()) {
        return std::filesystem::path(overrideDir).make_preferred();
    }
    if (std::filesystem::path dir = environmentPath("VD_SETUP_DIR"); !dir.empty()) {
        return dir.make_preferred();
    }
#if defined(_WIN32)
    if (std::filesystem::path local = environmentPath("LOCALAPPDATA"); !local.empty()) {
        return local / "VanillaDroid";
    }
    return environmentPath("USERPROFILE") / "AppData" / "Local" / "VanillaDroid";
#elif defined(__APPLE__)
    return environmentPath("HOME") / "Library" / "Application Support" / "VanillaDroid";
#else
    if (std::filesystem::path xdg = environmentPath("XDG_DATA_HOME"); !xdg.empty()) {
        return xdg / "VanillaDroid";
    }
    return environmentPath("HOME") / ".local" / "share" / "VanillaDroid";
#endif
}

std::filesystem::path packsDir(const Settings& settings, const std::filesystem::path& dataDir) {
    return settings.packs.empty() ? dataDir / "packs" : settings.packs;
}

std::string newPairingKey() {
    u8 bytes[16];
    if (!randomBytes(bytes, sizeof(bytes))) {
        return {};
    }
    return Sha256::hex(bytes);
}

std::string newPairingCode() {
    for (;;) {
        u32 value = 0;
        if (!randomBytes(&value, sizeof(value))) {
            return {};
        }
        if (value < 4294000000u) { // a multiple of 10^6: no code more likely than another
            char code[8];
            std::snprintf(code, sizeof(code), "%06u", value % 1000000u);
            return code;
        }
    }
}

} // namespace vd::setup
