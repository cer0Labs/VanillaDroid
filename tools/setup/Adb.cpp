#include "Adb.h"

#include "ChildProcess.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace vd::setup {
namespace {

std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        const usize end = text.find('\n');
        std::string_view line = text.substr(0, end);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        if (end == std::string_view::npos) {
            break;
        }
        text.remove_prefix(end + 1);
    }
    return lines;
}

std::vector<std::string_view> splitWords(std::string_view line) {
    std::vector<std::string_view> words;
    usize i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        const usize begin = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
            ++i;
        }
        if (i > begin) {
            words.push_back(line.substr(begin, i - begin));
        }
    }
    return words;
}

std::string_view baseName(std::string_view path) {
    const usize slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

bool contains(std::string_view text, std::string_view word) {
    return text.find(word) != std::string_view::npos;
}

std::filesystem::path environmentPath(const char* name) {
#if defined(_WIN32)
    std::wstring wide(name, name + std::char_traits<char>::length(name));
    const wchar_t* value = _wgetenv(wide.c_str());
#else
    const char* value = std::getenv(name);
#endif
    return value && *value ? std::filesystem::path(value) : std::filesystem::path();
}

} // namespace

std::vector<AdbDevice> parseAdbDevices(std::string_view output) {
    std::vector<AdbDevice> devices;
    for (const std::string_view line : splitLines(output)) {
        const std::vector<std::string_view> words = splitWords(line);
        if (words.size() < 2 || line.rfind("List of devices", 0) == 0 || line.rfind("* daemon", 0) == 0 ||
            line.rfind("adb server", 0) == 0) {
            continue;
        }
        AdbDevice device;
        device.serial = std::string(words[0]);
        device.state = std::string(words[1]);
        for (usize i = 2; i < words.size(); ++i) {
            if (words[i].rfind("model:", 0) == 0) {
                device.model = std::string(words[i].substr(6));
            }
        }
        devices.push_back(std::move(device));
    }
    return devices;
}

std::map<std::string, std::string> parseSha256sum(std::string_view output) {
    std::map<std::string, std::string> digests;
    for (const std::string_view line : splitLines(output)) {
        const usize space = line.find(' ');
        if (space != 64) {
            continue; // "sha256sum: x: No such file or directory" and the like
        }
        std::string_view path = line.substr(space);
        while (!path.empty() && (path.front() == ' ' || path.front() == '*')) {
            path.remove_prefix(1);
        }
        digests[std::string(baseName(path))] = toLowerAscii(line.substr(0, 64));
    }
    return digests;
}

std::map<std::string, u64> parseStatSizes(std::string_view output) {
    std::map<std::string, u64> sizes;
    for (const std::string_view line : splitLines(output)) {
        const usize space = line.find(' ');
        if (space == std::string_view::npos || space == 0) {
            continue;
        }
        u64 size = 0;
        bool number = true;
        for (const char c : line.substr(0, space)) {
            number &= c >= '0' && c <= '9';
            size = size * 10 + static_cast<u64>(c - '0');
        }
        if (number) {
            sizes[std::string(baseName(line.substr(space + 1)))] = size;
        }
    }
    return sizes;
}

std::string shellQuote(std::string_view text) {
    std::string out = "'";
    for (const char c : text) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

std::string remoteFilesDir(std::string_view appId) {
    return "/sdcard/Android/data/" + std::string(appId) + "/files";
}

std::string_view platformToolsUrl() {
#if defined(_WIN32)
    return "https://dl.google.com/android/repository/platform-tools-latest-windows.zip";
#elif defined(__APPLE__)
    return "https://dl.google.com/android/repository/platform-tools-latest-darwin.zip";
#else
    return "https://dl.google.com/android/repository/platform-tools-latest-linux.zip";
#endif
}

std::string_view platformToolsTermsUrl() {
    return "https://developer.android.com/studio/releases/platform-tools";
}

std::filesystem::path findAdb(const std::filesystem::path& explicitPath,
                              const std::filesystem::path& dataDir) {
    std::error_code error;
    if (!explicitPath.empty()) {
        return std::filesystem::is_regular_file(explicitPath, error) ? explicitPath : std::filesystem::path();
    }
    const std::filesystem::path name = pathFromUtf8(executableName("adb"));
    std::vector<std::filesystem::path> candidates = {dataDir / "platform-tools" / name};
    for (const char* variable : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) {
        if (const std::filesystem::path sdk = environmentPath(variable); !sdk.empty()) {
            candidates.push_back(sdk / "platform-tools" / name);
        }
    }
#if defined(_WIN32)
    if (const std::filesystem::path local = environmentPath("LOCALAPPDATA"); !local.empty()) {
        candidates.push_back(local / "Android" / "Sdk" / "platform-tools" / name);
    }
#elif defined(__APPLE__)
    candidates.push_back(environmentPath("HOME") / "Library" / "Android" / "sdk" / "platform-tools" / name);
#else
    candidates.push_back(environmentPath("HOME") / "Android" / "Sdk" / "platform-tools" / name);
#endif
    for (const std::filesystem::path& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate, error)) {
            return candidate;
        }
    }
    return findInPath("adb");
}

Result<std::filesystem::path> installPlatformTools(const std::filesystem::path& dataDir,
                                                   const Operation& operation) {
    const std::filesystem::path curl = findInPath("curl");
    if (curl.empty()) {
        return Error{formatText("VD_SETUP_ADB_NO_CURL")};
    }
    std::error_code error;
    std::filesystem::create_directories(dataDir, error);
    const std::filesystem::path zip = dataDir / "platform-tools.zip";
    std::filesystem::path partial = zip;
    partial += ".part";
    std::filesystem::remove(partial, error);

    const std::string downloading = formatText("VD_SETUP_ADB_DOWNLOADING", {std::string(platformToolsUrl())});
    operation.report("download", 0.0, downloading);
    ProcessOptions download;
    download.program = curl;
    download.args = {"-fL", "--retry", "2", "-o", utf8FromPath(partial), std::string(platformToolsUrl())};
    download.cancel = operation.cancel;
    std::string lastLine;
    download.onLine = [&](std::string_view line) {
        // curl's meter: "  45 6338k   45 2885k    0     0  1820k      0  0:00:03 ..." (the first number is
        // %).
        lastLine = std::string(line);
        usize i = 0;
        while (i < line.size() && line[i] == ' ') {
            ++i;
        }
        u32 percent = 0;
        usize digits = 0;
        while (i < line.size() && line[i] >= '0' && line[i] <= '9' && digits < 3) {
            percent = percent * 10 + static_cast<u32>(line[i++] - '0');
            ++digits;
        }
        if (digits > 0 && i < line.size() && line[i] == ' ' && percent <= 100) {
            operation.report("download", percent / 100.0, downloading);
        }
    };
    Result<ProcessResult> fetched = runProcess(download);
    if (!fetched) {
        return fetched.error();
    }
    if (fetched.value().cancelled) {
        std::filesystem::remove(partial, error);
        return Error{"cancelled"};
    }
    if (fetched.value().exitCode != 0) {
        std::filesystem::remove(partial, error);
        return Error{formatText("VD_SETUP_ADB_DOWNLOAD_FAILED", {lastLine})};
    }
    std::filesystem::rename(partial, zip, error);
    if (error) {
        return Error{formatText("VD_SETUP_ADB_DOWNLOAD_FAILED", {utf8FromPath(zip)})};
    }

    operation.report("unpack", -1.0, formatText("VD_SETUP_ADB_UNPACKING"));
    std::filesystem::remove_all(dataDir / "platform-tools", error);
    std::string output;
    Result<ProcessResult> unpacked = Error{""};
#if defined(_WIN32)
    // Windows 10 1803+ ships bsdtar as tar.exe, which reads zip files.
    const std::filesystem::path tar = findInPath("tar");
    if (tar.empty()) {
        return Error{formatText("VD_SETUP_ADB_NO_TAR")};
    }
    unpacked = runProcessCapture(tar, {"-xf", utf8FromPath(zip), "-C", utf8FromPath(dataDir)}, output);
#elif defined(__APPLE__)
    unpacked =
        runProcessCapture("/usr/bin/ditto", {"-x", "-k", utf8FromPath(zip), utf8FromPath(dataDir)}, output);
#else
    const std::filesystem::path unzip = findInPath("unzip");
    if (unzip.empty()) {
        return Error{formatText("VD_SETUP_ADB_NO_TAR")};
    }
    unpacked = runProcessCapture(unzip, {"-o", "-q", utf8FromPath(zip), "-d", utf8FromPath(dataDir)}, output);
#endif
    if (!unpacked) {
        return unpacked.error();
    }
    std::filesystem::remove(zip, error);
    const std::filesystem::path adb = dataDir / "platform-tools" / pathFromUtf8(executableName("adb"));
    if (unpacked.value().exitCode != 0 || !std::filesystem::is_regular_file(adb, error)) {
        return Error{formatText("VD_SETUP_ADB_UNPACK_FAILED", {output})};
    }
    return adb;
}

Message explainAdbFailure(std::string_view output) {
    struct Known {
        std::string_view code;
        const char* key;
    };
    static constexpr Known kKnown[] = {
        {"INSTALL_FAILED_UPDATE_INCOMPATIBLE", "VD_SETUP_ADB_SIGNATURE"},
        {"INSTALL_FAILED_VERSION_DOWNGRADE", "VD_SETUP_ADB_DOWNGRADE"},
        {"INSTALL_FAILED_USER_RESTRICTED", "VD_SETUP_ADB_USER_RESTRICTED"},
        {"INSTALL_FAILED_INSUFFICIENT_STORAGE", "VD_SETUP_ADB_NO_SPACE"},
        {"No space left on device", "VD_SETUP_ADB_NO_SPACE"},
        {"INSTALL_FAILED_NO_MATCHING_ABIS", "VD_SETUP_ADB_ABI"},
        {"unauthorized", "VD_SETUP_ADB_UNAUTHORIZED"},
        {"no devices/emulators found", "VD_SETUP_ADB_NO_DEVICE"},
        {"device offline", "VD_SETUP_ADB_OFFLINE"},
        {"more than one device", "VD_SETUP_ADB_MANY_DEVICES"},
    };
    for (const Known& known : kKnown) {
        if (contains(output, known.code)) {
            return Message{Message::Level::Error, known.key, {}};
        }
    }
    std::string last;
    for (const std::string_view line : splitLines(output)) {
        if (!line.empty()) {
            last = std::string(line);
        }
    }
    return Message{Message::Level::Error, "VD_SETUP_ADB_FAILED", {last}};
}

Result<std::string> Adb::run(const std::vector<std::string>& args, const CancelToken* cancel) const {
    std::vector<std::string> full;
    if (!m_serial.empty()) {
        full = {"-s", m_serial};
    }
    full.insert(full.end(), args.begin(), args.end());
    std::string output;
    Result<ProcessResult> result = runProcessCapture(m_program, full, output, cancel);
    if (!result) {
        return result.error();
    }
    if (result.value().cancelled) {
        return Error{"cancelled"};
    }
    if (result.value().exitCode != 0) {
        return Error{output};
    }
    return output;
}

Result<std::string> Adb::shell(std::string_view command, const CancelToken* cancel) const {
    std::vector<std::string> full;
    if (!m_serial.empty()) {
        full = {"-s", m_serial};
    }
    full.insert(full.end(), {"shell", std::string(command)});
    std::string output;
    Result<ProcessResult> result = runProcessCapture(m_program, full, output, cancel);
    if (!result) {
        return result.error();
    }
    if (result.value().cancelled) {
        return Error{"cancelled"};
    }
    return output;
}

Result<std::vector<AdbDevice>> Adb::devices() const {
    // Not tied to the chosen serial: every device.
    std::string output;
    Result<ProcessResult> result = runProcessCapture(m_program, {"devices", "-l"}, output);
    if (!result) {
        return result.error();
    }
    if (result.value().exitCode != 0) {
        return Error{format(explainAdbFailure(output))};
    }
    return parseAdbDevices(output);
}

Result<AdbDevice> Adb::chooseDevice() const {
    Result<std::vector<AdbDevice>> list = devices();
    if (!list) {
        return list.error();
    }
    const std::vector<AdbDevice>& devices = list.value();
    const AdbDevice* chosen = nullptr;
    if (!m_serial.empty()) {
        for (const AdbDevice& device : devices) {
            if (device.serial == m_serial) {
                chosen = &device;
            }
        }
        if (!chosen) {
            return Error{formatText("VD_SETUP_ADB_SERIAL_NOT_FOUND", {m_serial})};
        }
    } else if (devices.empty()) {
        return Error{formatText("VD_SETUP_ADB_NO_DEVICE")};
    } else if (devices.size() > 1) {
        return Error{formatText("VD_SETUP_ADB_MANY_DEVICES")};
    } else {
        chosen = &devices.front();
    }
    if (chosen->state == "unauthorized") {
        return Error{formatText("VD_SETUP_ADB_UNAUTHORIZED")};
    }
    if (chosen->state != "device") {
        return Error{formatText("VD_SETUP_ADB_OFFLINE")};
    }
    return *chosen;
}

Result<std::string> Adb::pair(std::string_view hostPort, std::string_view code) const {
    std::string output;
    Result<ProcessResult> result =
        runProcessCapture(m_program, {"pair", std::string(hostPort), std::string(code)}, output);
    if (!result) {
        return result.error();
    }
    if (result.value().exitCode != 0 || !contains(output, "Successfully paired")) {
        return Error{formatText("VD_SETUP_ADB_PAIR_FAILED", {output})};
    }
    return output;
}

Result<std::string> Adb::connect(std::string_view hostPort) const {
    std::string output;
    Result<ProcessResult> result = runProcessCapture(m_program, {"connect", std::string(hostPort)}, output);
    if (!result) {
        return result.error();
    }
    // adb connect exits with 0 even when it fails: its words decide.
    if (result.value().exitCode != 0 || !contains(output, "connected to") || contains(output, "failed") ||
        contains(output, "cannot")) {
        return Error{formatText("VD_SETUP_ADB_CONNECT_FAILED", {output})};
    }
    return output;
}

Result<bool> Adb::isInstalled(std::string_view appId) const {
    Result<std::string> output = shell("pm list packages " + std::string(appId));
    if (!output) {
        return output.error();
    }
    for (const std::string_view line : splitLines(output.value())) {
        if (line == "package:" + std::string(appId)) {
            return true;
        }
    }
    return false;
}

Result<void> Adb::install(const std::filesystem::path& apk, const Operation& operation) const {
    operation.report("install", -1.0, formatText("VD_SETUP_ADB_INSTALLING"));
    Result<std::string> output = run({"install", "-r", utf8FromPath(apk)}, operation.cancel);
    if (!output) {
        return Error{format(explainAdbFailure(output.error().message))};
    }
    if (!contains(output.value(), "Success")) {
        return Error{format(explainAdbFailure(output.value()))};
    }
    return {};
}

Result<PushOutcome> pushFiles(const Adb& adb, const std::string& remoteDir, std::span<const PushItem> items,
                              const Operation& operation) {
    PushOutcome outcome;
    if (Result<std::string> made = adb.shell("mkdir -p " + shellQuote(remoteDir) + " && echo vd-ok");
        !made || made.value().find("vd-ok") == std::string::npos) {
        return Error{formatText("VD_SETUP_ADB_NO_APP_FOLDER", {remoteDir})};
    }
    // What the phone has: sizes first (instant), then the digests of the files whose size matches.
    operation.report("push", 0.0, formatText("VD_SETUP_ADB_COMPARING"));
    Result<std::string> stat = adb.shell("cd " + shellQuote(remoteDir) + " && stat -c '%s %n' * 2>/dev/null");
    if (!stat) {
        return stat.error();
    }
    const std::map<std::string, u64> remoteSizes = parseStatSizes(stat.value());
    std::string candidates;
    for (const PushItem& item : items) {
        const auto size = remoteSizes.find(item.name);
        if (size != remoteSizes.end() && size->second == item.size) {
            candidates += ' ' + shellQuote(item.name);
        }
    }
    std::map<std::string, std::string> remoteDigests;
    if (!candidates.empty()) {
        Result<std::string> sums = adb.shell(
            "cd " + shellQuote(remoteDir) + " && sha256sum" + candidates + " 2>/dev/null", operation.cancel);
        if (!sums) {
            return sums.error();
        }
        remoteDigests = parseSha256sum(sums.value());
    }
    u64 total = 0;
    std::vector<const PushItem*> toPush;
    for (const PushItem& item : items) {
        const auto digest = remoteDigests.find(item.name);
        if (digest != remoteDigests.end() && digest->second == item.sha256) {
            ++outcome.skipped;
            continue;
        }
        toPush.push_back(&item);
        total += item.size;
    }

    u64 done = 0;
    for (usize i = 0; i < toPush.size(); ++i) {
        const PushItem& item = *toPush[i];
        if (operation.cancelled()) {
            return Error{"cancelled"};
        }
        const std::string remote = remoteDir + "/" + item.name;
        const std::string partial = remote + ".part";
        const std::string label =
            formatText("VD_SETUP_ADB_PUSHING", {item.name, formatBytes(item.size), std::to_string(i + 1),
                                                std::to_string(toPush.size())});
        operation.report("push", total ? static_cast<f64>(done) / static_cast<f64>(total) : 0.0, label);

        // While adb pushes (it shows no progress when its output is not a terminal), the size on the phone
        // tells.
        std::mutex waitMutex;
        std::condition_variable wake;
        bool finished = false;
        std::thread poll([&] {
            std::unique_lock<std::mutex> lock(waitMutex);
            while (!wake.wait_for(lock, std::chrono::seconds(2), [&] { return finished; })) {
                lock.unlock();
                Result<std::string> sizeText =
                    adb.shell("stat -c '%s %n' " + shellQuote(partial) + " 2>/dev/null");
                lock.lock();
                if (finished || !sizeText) {
                    continue;
                }
                const std::map<std::string, u64> size = parseStatSizes(sizeText.value());
                const auto it = size.find(item.name + ".part");
                if (it != size.end() && total > 0) {
                    const u64 now = done + std::min(it->second, item.size);
                    operation.report("push", static_cast<f64>(now) / static_cast<f64>(total), label);
                }
            }
        });
        Result<std::string> pushed = adb.run({"push", utf8FromPath(item.local), partial}, operation.cancel);
        {
            std::lock_guard<std::mutex> lock(waitMutex);
            finished = true;
        }
        wake.notify_all();
        poll.join();
        if (!pushed) {
            if (operation.cancelled()) {
                return Error{"cancelled"};
            }
            return Error{format(explainAdbFailure(pushed.error().message))};
        }
        // Checked on the phone before it takes the real name.
        Result<std::string> check = adb.shell("sha256sum " + shellQuote(partial), operation.cancel);
        if (!check) {
            return check.error();
        }
        const std::map<std::string, std::string> digest = parseSha256sum(check.value());
        const auto it = digest.find(item.name + ".part");
        if (it == digest.end() || it->second != item.sha256) {
            static_cast<void>(adb.shell("rm -f " + shellQuote(partial)));
            return Error{formatText("VD_SETUP_ADB_HASH_MISMATCH", {item.name})};
        }
        Result<std::string> renamed =
            adb.shell("mv -f " + shellQuote(partial) + " " + shellQuote(remote) + " && echo vd-ok");
        if (!renamed || renamed.value().find("vd-ok") == std::string::npos) {
            return Error{formatText("VD_SETUP_ADB_FAILED", {item.name})};
        }
        done += item.size;
        outcome.bytes += item.size;
        ++outcome.pushed;
    }
    operation.report("push", 1.0, {});
    return outcome;
}

} // namespace vd::setup
