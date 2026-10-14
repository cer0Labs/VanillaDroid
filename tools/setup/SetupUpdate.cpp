#include "SetupUpdate.h"

#include "ApkInfo.h"
#include "ChildProcess.h"
#include "HashCache.h"
#include "core/DevEnv.h"
#include "core/Json.h"
#include "core/Version.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <climits>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace vd::setup {
namespace {

constexpr i64 kCacheFormat = 1;

bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

std::filesystem::path withSuffix(const std::filesystem::path& path, std::string_view suffix) {
    std::filesystem::path out = path;
    out += pathFromUtf8(suffix);
    return out;
}

#if defined(_WIN32)
// A tool Windows ships in System32 (curl.exe and tar.exe since Windows 10 1803), before any other in the PATH
// (Git's tar cannot read a zip file).
std::filesystem::path systemTool(std::string_view name) {
    const wchar_t* root = _wgetenv(L"SystemRoot");
    if (root && *root) {
        const std::filesystem::path tool =
            std::filesystem::path(root) / L"System32" / pathFromUtf8(executableName(name));
        std::error_code error;
        if (std::filesystem::is_regular_file(tool, error)) {
            return tool;
        }
    }
    return findInPath(name);
}
#else
std::filesystem::path systemTool(std::string_view name) {
    for (const char* dir : {"/usr/bin/", "/bin/"}) {
        const std::filesystem::path tool = std::string(dir) + std::string(name);
        std::error_code error;
        if (std::filesystem::is_regular_file(tool, error)) {
            return tool;
        }
    }
    return findInPath(name);
}
#endif

Result<std::string> readSmallFile(const std::filesystem::path& path, usize maxBytes) {
    std::error_code error;
    const u64 size = std::filesystem::file_size(path, error);
    if (error || size > maxBytes) {
        return Error{"Cannot read " + utf8FromPath(path)};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return Error{"Cannot read " + utf8FromPath(path)};
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// The line fetchToPart asks curl to write after the transfer, with the HTTP status (0 when none came).
constexpr std::string_view kHttpStatusLine = "vd-http-status ";

// curl into `part` (deleted first, and on failure). `httpStatus`, when given, gets the server's answer
// (404...).
Result<void> fetchToPart(std::string_view url, const std::filesystem::path& part, const Operation& operation,
                         std::string_view label, u32 maxSeconds, u32* httpStatus = nullptr) {
    const std::filesystem::path curl = systemTool("curl");
    if (curl.empty()) {
        return Error{formatText("VD_SETUP_UPDATE_NO_CURL", {std::string(pack::kReleasePageUrl)})};
    }
    std::error_code error;
    std::filesystem::create_directories(part.parent_path(), error);
    std::filesystem::remove(part, error);
    operation.report("download", 0.0, label);
    ProcessOptions download;
    download.program = curl;
    // Only http(s), also after redirects (GitHub sends release assets to its storage).
    download.args = {"-fL",
                     "--proto",
                     "=http,https",
                     "--proto-redir",
                     "=http,https",
                     "--retry",
                     "2",
                     "--connect-timeout",
                     "20",
                     // Phase 22 (polish): the HTTP status on a line of its own (a 404 is not a network
                     // failure).
                     "-w",
                     "\\n" + std::string(kHttpStatusLine) + "%{http_code}\\n",
                     "-o",
                     utf8FromPath(part)};
    if (maxSeconds > 0) {
        download.args.insert(download.args.end(), {"--max-time", std::to_string(maxSeconds)});
    }
    download.args.emplace_back(url);
    download.cancel = operation.cancel;
    std::string lastLine;
    download.onLine = [&](std::string_view line) {
        if (line.substr(0, kHttpStatusLine.size()) == kHttpStatusLine) {
            if (httpStatus) {
                const std::string code(line.substr(kHttpStatusLine.size()));
                *httpStatus = static_cast<u32>(std::strtoul(code.c_str(), nullptr, 10));
            }
            return;
        }
        // curl's meter: "  45 6338k   45 2885k ..." (the first number is the percentage).
        if (!line.empty()) {
            lastLine = std::string(line);
        }
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
            operation.report("download", percent / 100.0, label);
        }
    };
    Result<ProcessResult> fetched = runProcess(download);
    if (!fetched) {
        return fetched.error();
    }
    if (fetched.value().cancelled) {
        std::filesystem::remove(part, error);
        return Error{"cancelled"};
    }
    if (fetched.value().exitCode != 0) {
        std::filesystem::remove(part, error);
        return Error{formatText("VD_SETUP_UPDATE_DOWNLOAD_FAILED", {std::string(url), lastLine})};
    }
    return {};
}

// "<name>.old" or "<name>.old1".."<name>.old9": the base name, else empty.
std::string leftoverBase(std::string_view name) {
    const usize at = name.rfind(kOldSuffix);
    if (at == std::string_view::npos || at == 0) {
        return {};
    }
    const std::string_view rest = name.substr(at + std::strlen(kOldSuffix));
    if (rest.size() > 1 || (rest.size() == 1 && (rest[0] < '1' || rest[0] > '9'))) {
        return {};
    }
    return std::string(name.substr(0, at));
}

} // namespace

std::string updateManifestUrl(std::string_view override) {
    if constexpr (!kDistBuild) {
        if (!override.empty()) {
            return std::string(override);
        }
        if (const char* url = VD_DEV_ENV("VD_UPDATE_URL"); url && *url) {
            return url;
        }
    }
    return std::string(pack::kReleaseManifestUrl);
}

std::string signatureUrlOf(std::string_view manifestUrl) {
    return std::string(manifestUrl) + std::string(pack::kReleaseSignatureSuffix);
}

i64 unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool UpdateCache::fresh(i64 now, std::string_view url) const {
    if (manifestUrl != url) {
        return false;
    }
    return (checkedAt > 0 && now - checkedAt < kUpdateCheckSeconds && now >= checkedAt) ||
           (attemptedAt > 0 && now - attemptedAt < kUpdateRetrySeconds && now >= attemptedAt);
}

UpdateCache loadUpdateCache(const std::filesystem::path& dataDir) {
    UpdateCache cache;
    Result<std::string> text = readSmallFile(dataDir / kUpdateCacheFile, 64 * 1024);
    if (!text) {
        return cache;
    }
    Result<json::Value> parsed = json::parse(text.value());
    if (!parsed || parsed.value().get("format").asInt() != kCacheFormat) {
        return cache;
    }
    const json::Value& root = parsed.value();
    cache.checkedAt = root.get("checkedAt").asInt();
    cache.attemptedAt = root.get("attemptedAt").asInt();
    cache.manifestUrl = std::string(root.get("manifestUrl").asString());
    cache.version = std::string(root.get("version").asString());
    cache.versionCode = static_cast<u32>(root.get("versionCode").asInt());
    cache.notes = std::string(root.get("notes").asString());
    cache.tooNew = root.get("tooNew").asBool();
    return cache;
}

Result<void> saveUpdateCache(const std::filesystem::path& dataDir, const UpdateCache& cache) {
    json::Value root = json::Value::object();
    root.set("format", json::Value::number(kCacheFormat));
    root.set("checkedAt", json::Value::number(static_cast<double>(cache.checkedAt)));
    root.set("attemptedAt", json::Value::number(static_cast<double>(cache.attemptedAt)));
    root.set("manifestUrl", json::Value::string(cache.manifestUrl));
    root.set("version", json::Value::string(cache.version));
    root.set("versionCode", json::Value::number(cache.versionCode));
    root.set("notes", json::Value::string(cache.notes));
    root.set("tooNew", json::Value::boolean(cache.tooNew));
    const std::string text = json::write(root);
    std::error_code error;
    std::filesystem::create_directories(dataDir, error);
    const std::filesystem::path file = dataDir / kUpdateCacheFile;
    const std::filesystem::path temp = withSuffix(file, ".tmp");
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

Result<void> curlDownload(std::string_view url, const std::filesystem::path& file, const Operation& operation,
                          std::string_view label, u32 maxSeconds, u32* httpStatus) {
    const std::filesystem::path part = withSuffix(file, ".part");
    if (Result<void> fetched = fetchToPart(url, part, operation, label, maxSeconds, httpStatus); !fetched) {
        return fetched;
    }
    std::error_code error;
    std::filesystem::rename(part, file, error);
    if (error) {
        std::filesystem::remove(part, error);
        return Error{formatText("VD_SETUP_UPDATE_DOWNLOAD_FAILED", {std::string(url), utf8FromPath(file)})};
    }
    return {};
}

Result<UpdateCheck> checkForUpdate(const std::filesystem::path& dataDir, std::string_view manifestUrl,
                                   const Operation& operation, u32 maxSeconds) {
    UpdateCache cache = loadUpdateCache(dataDir);
    if (cache.manifestUrl != manifestUrl) {
        cache = UpdateCache{}; // what another URL said is not this one's
        cache.manifestUrl = std::string(manifestUrl);
    }
    cache.attemptedAt = unixNow();
    const auto failed = [&](Error error) -> Result<UpdateCheck> {
        static_cast<void>(saveUpdateCache(dataDir, cache));
        return error;
    };
    const std::filesystem::path folder = dataDir / kUpdateFolder;
    const std::filesystem::path manifestFile = folder / "manifest.json";
    const std::filesystem::path signatureFile = folder / "manifest.json.sig";
    const std::string label = formatText("VD_SETUP_UPDATE_CHECKING");
    u32 status = 0;
    if (Result<void> got = curlDownload(manifestUrl, manifestFile, operation, label, maxSeconds, &status);
        !got) {
        if (status == 404 && !operation.cancelled()) {
            // Phase 22 (polish): no release published yet (GitHub answers 404 before the first one): nothing
            // newer, which is a check that worked.
            cache.checkedAt = cache.attemptedAt;
            cache.version.clear();
            cache.versionCode = 0;
            cache.notes.clear();
            cache.tooNew = false;
            static_cast<void>(saveUpdateCache(dataDir, cache));
            UpdateCheck none;
            none.published = false;
            return none;
        }
        return failed(got.error());
    }
    if (Result<void> got =
            curlDownload(signatureUrlOf(manifestUrl), signatureFile, operation, label, maxSeconds);
        !got) {
        return failed(got.error());
    }
    Result<std::string> json = readSmallFile(manifestFile, pack::kMaxReleaseManifestBytes);
    Result<std::string> signature = readSmallFile(signatureFile, pack::kMaxReleaseSignatureBytes);
    if (!json || !signature) {
        return failed(Error{formatText("VD_SETUP_UPDATE_BAD_SIGNATURE")});
    }
    UpdateCheck check;
    check.release = pack::readSignedRelease(json.value(), signature.value());
    switch (check.release.status) {
    case pack::SignedRelease::Status::BadSignature:
        return failed(Error{formatText("VD_SETUP_UPDATE_BAD_SIGNATURE")});
    case pack::SignedRelease::Status::Damaged:
        return failed(Error{formatText("VD_SETUP_UPDATE_DAMAGED", {check.release.error})});
    case pack::SignedRelease::Status::Ok:
    case pack::SignedRelease::Status::TooNew: break;
    }
    const pack::ReleaseManifest& release = check.release.manifest;
    check.newer = release.versionCode > kVersionCode;
    cache.checkedAt = cache.attemptedAt;
    cache.version = release.version;
    cache.versionCode = release.versionCode;
    cache.notes = release.notes;
    cache.tooNew = release.tooNew();
    static_cast<void>(saveUpdateCache(dataDir, cache));
    return check;
}

Message updateStatusMessage(const UpdateCache& cache, std::string_view url) {
    if (cache.manifestUrl != url || cache.checkedAt == 0) {
        return {Message::Level::Info, "VD_SETUP_STATUS_UPDATE_UNKNOWN", {}};
    }
    if (cache.tooNew) { // a manifest this program cannot read (its versionCode may be unknown): newer
        return {Message::Level::Warning,
                "VD_SETUP_STATUS_UPDATE_TOO_NEW",
                {cache.version, std::string(pack::kReleasePageUrl)}};
    }
    if (cache.versionCode > kVersionCode) {
        return {Message::Level::Warning, "VD_SETUP_STATUS_UPDATE_AVAILABLE", {cache.version}};
    }
    return {Message::Level::Info, "VD_SETUP_STATUS_UPDATE_NONE", {}};
}

Result<void> downloadReleaseFile(const pack::ReleaseFile& source, const std::filesystem::path& file,
                                 const Operation& operation, std::string_view label) {
    if (!source.present()) {
        return Error{formatText("VD_SETUP_UPDATE_DAMAGED", {utf8FromPath(file.filename())})};
    }
    std::error_code error;
    if (std::filesystem::is_regular_file(file, error) &&
        std::filesystem::file_size(file, error) == source.size) {
        Result<std::string> digest = sha256FileProgress(file, {}, operation.cancel);
        if (digest && digest.value() == source.sha256) {
            return {}; // downloaded before
        }
    }
    const std::filesystem::path part = withSuffix(file, ".part");
    if (Result<void> fetched = fetchToPart(source.url, part, operation, label, 0); !fetched) {
        return fetched;
    }
    // Checked before it takes its name: a wrong file never stands where a right one is looked for.
    const u64 size = std::filesystem::file_size(part, error);
    operation.report("verify", -1.0,
                     formatText("VD_SETUP_UPDATE_VERIFYING", {utf8FromPath(file.filename())}));
    Result<std::string> digest = sha256FileProgress(part, {}, operation.cancel);
    if (error || size != source.size || !digest || digest.value() != source.sha256) {
        std::filesystem::remove(part, error);
        return Error{formatText("VD_SETUP_UPDATE_WRONG_FILE", {utf8FromPath(file.filename())})};
    }
    std::filesystem::remove(file, error);
    std::filesystem::rename(part, file, error);
    if (error) {
        std::filesystem::remove(part, error);
        return Error{formatText("VD_SETUP_UPDATE_DOWNLOAD_FAILED", {source.url, utf8FromPath(file)})};
    }
    return {};
}

std::string fileNameOfUrl(std::string_view url, std::string_view fallback) {
    const usize end = std::min(url.find('?'), url.find('#'));
    std::string_view path = url.substr(0, end);
    const usize slash = path.rfind('/');
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const bool safe =
        !name.empty() && name != "." && name != ".." && std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                   c == '-' || c == '_';
        });
    return std::string(safe ? name : fallback);
}

// --- Replacing the installed Setup
// ----------------------------------------------------------------------------

std::filesystem::path runningProgram() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    char buffer[PATH_MAX];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) != 0) {
        return {};
    }
    std::error_code error;
    const std::filesystem::path resolved = std::filesystem::canonical(buffer, error);
    return error ? std::filesystem::path(buffer) : resolved;
#else
    std::error_code error;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path() : self;
#endif
}

InstallLayout installLayout(const std::filesystem::path& program) {
    InstallLayout layout;
    layout.program = program;
    layout.dir = program.parent_path();
    // <dir>/VanillaDroid Setup.app/Contents/MacOS/<program>: the bundle is what an update swaps.
    const std::filesystem::path macos = program.parent_path();
    const std::filesystem::path contents = macos.parent_path();
    const std::filesystem::path app = contents.parent_path();
    if (macos.filename() == "MacOS" && contents.filename() == "Contents" && app.extension() == ".app") {
        layout.bundle = app;
        layout.dir = app.parent_path();
    }
    return layout;
}

bool isInstalledSetup(const InstallLayout& layout) {
    if (layout.program.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::is_regular_file(
        layout.program.parent_path() / pathFromUtf8(executableName("vd_baker")), error);
}

Result<void> checkStagedSetup(const std::filesystem::path& staged, const InstallLayout& layout) {
    std::error_code error;
    std::filesystem::path programDir = staged;
    if (!layout.bundle.empty()) {
        // One .app, with this program inside.
        std::filesystem::path app;
        u32 apps = 0;
        for (const auto& entry : std::filesystem::directory_iterator(staged, error)) {
            if (entry.is_directory(error) && entry.path().extension() == ".app") {
                app = entry.path();
                ++apps;
            }
        }
        if (apps != 1) {
            return Error{formatText("VD_SETUP_UPDATE_BAD_ZIP")};
        }
        programDir = app / "Contents" / "MacOS";
    }
    const std::filesystem::path program = programDir / layout.program.filename();
    const std::filesystem::path baker = programDir / pathFromUtf8(executableName("vd_baker"));
    if (!std::filesystem::is_regular_file(program, error) ||
        !std::filesystem::is_regular_file(baker, error)) {
        return Error{formatText("VD_SETUP_UPDATE_BAD_ZIP")};
    }
    return {};
}

Result<std::filesystem::path> unpackSetup(const std::filesystem::path& zip, const InstallLayout& layout,
                                          std::string_view version, const Operation& operation) {
    std::error_code error;
    const std::filesystem::path staged =
        layout.dir / pathFromUtf8(std::string(kStagingPrefix) + std::string(version));
    std::filesystem::remove_all(staged, error);
    std::filesystem::create_directories(staged, error);
    if (error) {
        return Error{formatText("VD_SETUP_UPDATE_CANNOT_WRITE", {utf8FromPath(layout.dir)})};
    }
    operation.report("unpack", -1.0, formatText("VD_SETUP_UPDATE_UNPACKING"));
    std::string output;
    Result<ProcessResult> unpacked = Error{""};
#if defined(_WIN32)
    const std::filesystem::path tar = systemTool("tar"); // Windows 10 1803+: bsdtar, which reads zip files
    if (tar.empty()) {
        std::filesystem::remove_all(staged, error);
        return Error{formatText("VD_SETUP_UPDATE_NO_TAR", {std::string(pack::kReleasePageUrl)})};
    }
    unpacked = runProcessCapture(tar, {"-xf", utf8FromPath(zip), "-C", utf8FromPath(staged)}, output,
                                 operation.cancel);
#elif defined(__APPLE__)
    // ditto keeps the bundle's permissions and links.
    unpacked = runProcessCapture("/usr/bin/ditto", {"-x", "-k", utf8FromPath(zip), utf8FromPath(staged)},
                                 output, operation.cancel);
#else
    const std::filesystem::path unzip = systemTool("unzip");
    if (unzip.empty()) {
        std::filesystem::remove_all(staged, error);
        return Error{formatText("VD_SETUP_UPDATE_NO_TAR", {std::string(pack::kReleasePageUrl)})};
    }
    unpacked = runProcessCapture(unzip, {"-o", "-q", utf8FromPath(zip), "-d", utf8FromPath(staged)}, output,
                                 operation.cancel);
#endif
    if (!unpacked || unpacked.value().cancelled || unpacked.value().exitCode != 0) {
        std::filesystem::remove_all(staged, error);
        return Error{
            formatText("VD_SETUP_UPDATE_UNPACK_FAILED", {unpacked ? output : unpacked.error().message})};
    }
    if (Result<void> checked = checkStagedSetup(staged, layout); !checked) {
        std::filesystem::remove_all(staged, error);
        return checked.error();
    }
    return staged;
}

Result<u32> replaceInstall(const std::filesystem::path& staged, const InstallLayout& layout) {
    struct Moved {
        std::filesystem::path target;
        std::filesystem::path old; // empty: nothing was there
    };
    std::vector<Moved> moved;
    std::error_code error;
    std::vector<std::filesystem::path> entries;
    for (const auto& entry : std::filesystem::directory_iterator(staged, error)) {
        entries.push_back(entry.path());
    }
    std::sort(entries.begin(), entries.end());
    const auto rollback = [&] {
        std::error_code ignored;
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
            std::filesystem::remove_all(it->target, ignored);
            if (!it->old.empty()) {
                std::filesystem::rename(it->old, it->target, ignored);
            }
        }
        std::filesystem::remove_all(staged, ignored);
    };
    for (const std::filesystem::path& entry : entries) {
        const bool bundle = !layout.bundle.empty() && entry.extension() == ".app";
        const std::filesystem::path target = bundle ? layout.bundle : layout.dir / entry.filename();
        Moved move{target, {}};
        if (std::filesystem::exists(std::filesystem::symlink_status(target, error))) {
            // "<name>.old", or the first free "<name>.oldN" when an earlier one is still in use.
            for (u32 n = 0; n < 10 && move.old.empty(); ++n) {
                const std::filesystem::path old =
                    withSuffix(target, std::string(kOldSuffix) + (n ? std::to_string(n) : std::string()));
                std::filesystem::remove_all(old, error);
                if (!std::filesystem::exists(std::filesystem::symlink_status(old, error))) {
                    move.old = old;
                }
            }
            std::filesystem::rename(target, move.old, error);
            if (move.old.empty() || error) {
                rollback();
                return Error{formatText("VD_SETUP_UPDATE_REPLACE_FAILED", {utf8FromPath(target.filename())})};
            }
        }
        std::filesystem::rename(entry, target, error);
        if (error) {
            if (!move.old.empty()) {
                std::filesystem::rename(move.old, target, error);
            }
            rollback();
            return Error{formatText("VD_SETUP_UPDATE_REPLACE_FAILED", {utf8FromPath(target.filename())})};
        }
        moved.push_back(std::move(move));
    }
    std::filesystem::remove_all(staged, error);
    return static_cast<u32>(moved.size());
}

void removeUpdateLeftovers(const InstallLayout& layout) {
    std::error_code error;
    std::vector<std::filesystem::path> leftovers;
    for (const auto& entry : std::filesystem::directory_iterator(layout.dir, error)) {
        const std::string name = utf8FromPath(entry.path().filename());
        if (name.rfind(kStagingPrefix, 0) == 0 && entry.is_directory(error)) {
            leftovers.push_back(entry.path());
            continue;
        }
        const std::string base = leftoverBase(name);
        if (!base.empty() && std::filesystem::exists(
                                 std::filesystem::symlink_status(layout.dir / pathFromUtf8(base), error))) {
            leftovers.push_back(entry.path());
        }
    }
    for (const std::filesystem::path& path : leftovers) {
        std::filesystem::remove_all(path, error); // a program still running from it stays until a later start
    }
}

Result<int> relaunch(const std::filesystem::path& program, const std::vector<std::string>& args, bool wait) {
#if defined(_WIN32)
    std::string commandLine = quoteWindowsArgument(utf8FromPath(program));
    for (const std::string& argument : args) {
        commandLine += ' ';
        commandLine += quoteWindowsArgument(argument);
    }
    const int wideLength =
        MultiByteToWideChar(CP_UTF8, 0, commandLine.data(), static_cast<int>(commandLine.size()), nullptr, 0);
    std::wstring wideCommand(static_cast<usize>(wideLength), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, commandLine.data(), static_cast<int>(commandLine.size()),
                        wideCommand.data(), wideLength);

    // The standard handles (the console, or the pipes a caller gave): duplicated as inheritable, the only
    // handles the new program gets.
    const DWORD kinds[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    HANDLE inherited[3] = {};
    HANDLE list[3] = {};
    DWORD count = 0;
    for (u32 i = 0; i < 3; ++i) {
        const HANDLE handle = GetStdHandle(kinds[i]);
        if (handle && handle != INVALID_HANDLE_VALUE &&
            DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &inherited[i], 0, TRUE,
                            DUPLICATE_SAME_ACCESS)) {
            list[count++] = inherited[i];
        }
    }
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<u8> attributeBuffer(attributeSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
    const bool listReady = count > 0 && InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize) &&
                           UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list,
                                                     count * sizeof(HANDLE), nullptr, nullptr);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    if (listReady) {
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = inherited[0];
        startup.StartupInfo.hStdOutput = inherited[1];
        startup.StartupInfo.hStdError = inherited[2];
        startup.lpAttributeList = attributes;
    }
    PROCESS_INFORMATION process{};
    const std::wstring path = program.wstring();
    const BOOL started =
        CreateProcessW(path.c_str(), wideCommand.data(), nullptr, nullptr, listReady ? TRUE : FALSE,
                       CREATE_UNICODE_ENVIRONMENT | (listReady ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr,
                       nullptr, &startup.StartupInfo, &process);
    const DWORD startError = GetLastError();
    if (listReady) {
        DeleteProcThreadAttributeList(attributes);
    }
    for (HANDLE handle : inherited) {
        if (handle) {
            CloseHandle(handle);
        }
    }
    if (!started) {
        return Error{"Cannot start " + utf8FromPath(program) + " (error " + std::to_string(startError) + ")"};
    }
    CloseHandle(process.hThread);
    DWORD code = 0;
    if (wait) {
        WaitForSingleObject(process.hProcess, INFINITE);
        GetExitCodeProcess(process.hProcess, &code);
    }
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
#else
    static_cast<void>(wait);
    std::vector<std::string> storage;
    storage.push_back(utf8FromPath(program));
    storage.insert(storage.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& argument : storage) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    execv(storage.front().c_str(), argv.data());
    return Error{"Cannot start " + storage.front() + " (" + std::strerror(errno) + ")"};
#endif
}

// --- The APK
// --------------------------------------------------------------------------------------------------

std::filesystem::path downloadedApkDir(const std::filesystem::path& dataDir) {
    return dataDir / "apk";
}

std::vector<std::filesystem::path> downloadedApks(const std::filesystem::path& dataDir) {
    std::vector<std::pair<u32, std::filesystem::path>> found;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(downloadedApkDir(dataDir), error)) {
        if (entry.is_regular_file(error) && equalsNoCase(utf8FromPath(entry.path().extension()), ".apk")) {
            Result<ApkInfo> info = readApkInfo(entry.path());
            found.emplace_back(info ? info.value().versionCode : 0u, entry.path());
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    std::vector<std::filesystem::path> apks;
    for (auto& [code, path] : found) {
        apks.push_back(std::move(path));
    }
    return apks;
}

std::filesystem::path newestApk(std::span<const std::filesystem::path> candidates) {
    std::filesystem::path best;
    u32 bestCode = 0;
    for (const std::filesystem::path& candidate : candidates) {
        if (candidate.empty()) {
            continue;
        }
        Result<ApkInfo> info = readApkInfo(candidate);
        if (info && (best.empty() || info.value().versionCode > bestCode)) {
            best = candidate;
            bestCode = info.value().versionCode;
        }
    }
    return best;
}

void removeOtherApks(const std::filesystem::path& dataDir, const std::filesystem::path& keep) {
    std::error_code error;
    for (const std::filesystem::path& apk : downloadedApks(dataDir)) {
        if (!std::filesystem::equivalent(apk, keep, error)) {
            std::filesystem::remove(apk, error);
        }
    }
}

} // namespace vd::setup
