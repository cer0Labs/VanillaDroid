// vdsetup: VanillaDroid Setup by console (phase 22, ADR-0005 §4-§5). Checks the player's WoW 1.12.1, bakes
// the packs with vd_baker, and gets the APK and the packs onto the phone over the local network (serve: a QR
// code and a 6-digit code) or over ADB. Every text the player reads is a key of locale/<locale>/setup.lang.
#include "Adb.h"
#include "ApkInfo.h"
#include "BakeRunner.h"
#include "ChildProcess.h"
#include "HashCache.h"
#include "NetInterfaces.h"
#include "PhoneServer.h"
#include "QrText.h"
#include "SetupCommon.h"
#include "SetupSettings.h"
#include "SetupText.h"
#include "SetupUpdate.h" // phase 22 (updates)
#include "WowCheck.h"
#include "core/Clock.h"
#include "core/Log.h"
#include "core/Version.h"
#include "pack/PackManifest.h"
#include "pack/ReleaseManifest.h" // phase 22 (updates)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <io.h>
#include <shellapi.h> // after windows.h
#else
#include <csignal>
#include <unistd.h>
#endif

namespace {

using namespace vd;
using namespace vd::setup;

// --- Console -----------------------------------------------------------------------------------------------

CancelToken g_cancel;
bool g_console = false; // stdout is an interactive console (progress lines are rewritten in place)
bool g_ansi = false;    // it understands ANSI escapes (colours for the QR code)
std::mutex g_outputMutex;

#if defined(_WIN32)
BOOL WINAPI onConsoleSignal(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_cancel.cancel();
        return TRUE;
    }
    return FALSE;
}
#else
void onSignal(int) {
    g_cancel.cancel();
}
#endif

void initConsole() {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(out, &mode)) {
        g_console = true;
        g_ansi = SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    }
    SetConsoleCtrlHandler(onConsoleSignal, TRUE);
#else
    g_console = isatty(STDOUT_FILENO) != 0;
    g_ansi = g_console;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#endif
}

int consoleWidth() {
#if defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        return info.srWindow.Right - info.srWindow.Left + 1;
    }
#endif
    return 100;
}

// The first `columns` characters of a UTF-8 text (never half a character).
std::string clip(std::string_view text, usize columns) {
    usize count = 0;
    usize i = 0;
    while (i < text.size() && count < columns) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        ++count;
    }
    return std::string(text.substr(0, std::min(i, text.size())));
}

// Prints lines and one progress line under them (rewritten in place on a console; on a pipe, a new line every
// 10 %).
class Console {
public:
    void line(std::string_view text) {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        clearProgress();
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fputc('\n', stdout);
        drawProgress();
        std::fflush(stdout);
    }
    void say(std::string_view key, const std::vector<std::string>& args = {}) { line(formatText(key, args)); }
    void message(const Message& m) {
        const char* prefix = m.level == Message::Level::Error     ? "  X "
                             : m.level == Message::Level::Warning ? "  ! "
                                                                  : "  - ";
        line(prefix + format(m));
    }
    void progress(f64 fraction, std::string_view text) {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        const std::string keep = m_text;
        if (text.empty()) {
            text = keep; // a fraction alone: the same line
        }
        const int percent = fraction < 0 ? -1 : static_cast<int>(std::min(fraction, 1.0) * 100.0);
        if (!g_console) {
            if (text != m_text || percent / 10 != m_percent / 10) {
                m_text = std::string(text);
                m_percent = percent;
                std::string out = percent >= 0 ? "[" + std::to_string(percent) + "%] " + m_text : m_text;
                std::fwrite(out.data(), 1, out.size(), stdout);
                std::fputc('\n', stdout);
                std::fflush(stdout);
            }
            return;
        }
        m_text = std::string(text);
        m_percent = percent;
        m_active = true;
        clearProgress();
        drawProgress();
        std::fflush(stdout);
    }
    void endProgress() {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        clearProgress();
        m_active = false;
        m_text.clear();
        m_percent = -2;
        std::fflush(stdout);
    }

private:
    void clearProgress() {
        if (g_console && m_drawn > 0) {
            std::fputc('\r', stdout);
            std::fwrite(std::string(m_drawn, ' ').data(), 1, m_drawn, stdout);
            std::fputc('\r', stdout);
            m_drawn = 0;
        }
    }
    void drawProgress() {
        if (!g_console || !m_active || m_text.empty()) {
            return;
        }
        char percent[16] = "";
        if (m_percent >= 0) {
            std::snprintf(percent, sizeof(percent), "[%3d%%] ", m_percent);
        }
        const std::string out =
            clip(std::string(percent) + m_text, static_cast<usize>(std::max(20, consoleWidth() - 1)));
        std::fwrite(out.data(), 1, out.size(), stdout);
        m_drawn = 0;
        for (const char c : out) {
            m_drawn += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
        }
    }

    std::string m_text;
    int m_percent = -2;
    bool m_active = false;
    usize m_drawn = 0;
};

Console g_out;

// --- Arguments ---------------------------------------------------------------------------------------------

struct Arguments {
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;
    std::set<std::string> flags;

    bool has(const std::string& name) const { return flags.count(name) || options.count(name); }
    std::string get(const std::string& name, const std::string& fallback = {}) const {
        const auto it = options.find(name);
        return it != options.end() ? it->second : fallback;
    }
};

// Options that take no value.
const std::set<std::string> kFlags = {"force",   "wow",   "accept", "verbose",
                                      "new-key", "no-qr", "help",   "loopback",
                                      "check"}; // phase 22 (updates): update --check

bool parseArguments(const std::vector<std::string>& args, Arguments& out, std::string& bad) {
    for (usize i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg.rfind("--", 0) == 0 && arg.size() > 2) {
            std::string name = arg.substr(2);
            const usize equals = name.find('=');
            if (equals != std::string::npos) {
                out.options[name.substr(0, equals)] = name.substr(equals + 1);
            } else if (kFlags.count(name)) {
                out.flags.insert(name);
            } else if (i + 1 < args.size()) {
                out.options[name] = args[++i];
            } else {
                bad = arg;
                return false;
            }
        } else {
            out.positional.push_back(arg);
        }
    }
    return true;
}

std::vector<std::string> utf8Arguments(int argc, char** argv) {
    std::vector<std::string> args;
#if defined(_WIN32)
    // The wide command line: paths with letters outside the system's code page arrive intact.
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide) {
        for (int i = 1; i < count; ++i) {
            args.push_back(utf8FromPath(std::filesystem::path(wide[i])));
        }
        LocalFree(wide);
        return args;
    }
#endif
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return args;
}

std::vector<std::string> splitList(std::string_view text) {
    std::vector<std::string> items;
    usize begin = 0;
    while (begin <= text.size()) {
        const usize end = std::min(text.find_first_of(";,", begin), text.size());
        std::string item(text.substr(begin, end - begin));
        while (!item.empty() && item.front() == ' ') {
            item.erase(item.begin());
        }
        while (!item.empty() && item.back() == ' ') {
            item.pop_back();
        }
        if (!item.empty()) {
            items.push_back(item);
        }
        begin = end + 1;
    }
    return items;
}

// --- Shared state ------------------------------------------------------------------------------------------

struct Context {
    Arguments args;
    std::vector<std::string> rawArgs; // phase 22 (updates): as given, for the new program after an update
    std::filesystem::path dataDir;
    std::filesystem::path settingsFile;
    Settings settings;
    bool verbose = false;

    std::filesystem::path packs() const { return packsDir(settings, dataDir); }
    bool save() {
        if (Result<void> saved = settings.save(settingsFile); !saved) {
            g_out.say("VD_SETUP_SETTINGS_SAVE_FAILED", {saved.error().message});
            return false;
        }
        return true;
    }
};

Operation consoleOperation(Context& context) {
    Operation operation;
    operation.cancel = &g_cancel;
    operation.progress = [&context](const ProgressEvent& event) {
        if (event.step == "bake-log") {
            const bool error = event.text.rfind("[ERROR]", 0) == 0;
            if (context.verbose || error) {
                g_out.line(std::string("    ") + std::string(event.text));
            }
            return;
        }
        if (!event.text.empty() || event.fraction >= 0) {
            g_out.progress(event.fraction, event.text);
        }
    };
    return operation;
}

std::filesystem::path findApk(const Context& context) {
    std::error_code error;
    if (const std::string option = context.args.get("apk"); !option.empty()) {
        return pathFromUtf8(option);
    }
    if (!context.settings.apk.empty()) {
        return context.settings.apk;
    }
    const std::filesystem::path dir = executableDir();
    std::filesystem::path shipped = findChildNoCase(dir, "VanillaDroid.apk");
    if (shipped.empty()) {
        std::vector<std::filesystem::path> apks;
        for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
            if (equalsNoCase(utf8FromPath(entry.path().extension()), ".apk")) {
                apks.push_back(entry.path());
            }
        }
        std::sort(apks.begin(), apks.end());
        shipped = apks.empty() ? std::filesystem::path() : apks.front();
    }
    // Phase 22 (updates): the release's APK `vdsetup update` downloaded, when it is newer than the one shipped.
    const std::vector<std::filesystem::path> downloaded = downloadedApks(context.dataDir);
    if (!downloaded.empty()) {
        const std::filesystem::path candidates[] = {shipped, downloaded.front()};
        if (std::filesystem::path newest = newestApk(candidates); !newest.empty()) {
            return newest;
        }
    }
    return shipped;
}

void printChecks(const ClientCheck& check) {
    for (const Message& message : check.messages) {
        g_out.message(message);
    }
}

// --- Updates (phase 22: updates) ---------------------------------------------------------------------------

// The game data a release bakes differently from what the packs folder holds ("world (azeroth), models"; empty:
// none).
std::string releaseDataSteps(const Context& context, const pack::ReleaseManifest& release) {
    Result<pack::PackManifest> folder = pack::PackManifest::load(context.packs());
    const std::vector<std::string> changed = pack::releaseDataChanges(
        folder ? folder.value() : pack::PackManifest{}, release.packs, pack::releaseRecipes());
    return pack::describeSteps(pack::bakeStepsFor(changed));
}

// The manifest's URL, or empty when the command line asks for one a distribution build does not take.
std::string updateUrlOf(const Context& context) {
    if constexpr (kDistBuild) {
        if (context.args.has("manifest-url")) {
            g_out.say("VD_SETUP_UPDATE_URL_DEVELOPMENT");
            return {};
        }
    }
    return updateManifestUrl(context.args.get("manifest-url"));
}

// `status`: a release newer than this Setup, from the last check (a new one at most every few hours). A development
// build asks only a release folder it is given (--manifest-url, VD_UPDATE_URL), never by itself.
void printUpdateStatus(const Context& context) {
    if constexpr (!kDistBuild) {
        if (updateManifestUrl({}) == pack::kReleaseManifestUrl && !context.args.has("manifest-url")) {
            return;
        }
    }
    const std::string url = updateUrlOf(context);
    if (url.empty()) {
        return;
    }
    UpdateCache cache = loadUpdateCache(context.dataDir);
    if (!cache.fresh(unixNow(), url)) {
        Operation quiet;
        quiet.cancel = &g_cancel;
        static_cast<void>(checkForUpdate(context.dataDir, url, quiet, 10));
        cache = loadUpdateCache(context.dataDir);
    }
    const Message status = updateStatusMessage(cache, url); // phase 22 (polish): the window says the same
    g_out.say(status.key, status.args);
}

// The release's APK where `serve` and `adb install` take it (<data folder>/apk/), unless the one they offer is
// already as new.
bool fetchReleaseApk(Context& context, const pack::ReleaseManifest& release) {
    if (const std::filesystem::path offered = findApk(context); !offered.empty()) {
        if (Result<ApkInfo> info = readApkInfo(offered);
            info && info.value().versionCode >= release.versionCode) {
            g_out.say("VD_SETUP_UPDATE_APK_READY", {info.value().versionName, utf8FromPath(offered)});
            return true;
        }
    }
    const std::filesystem::path file =
        downloadedApkDir(context.dataDir) /
        pathFromUtf8(fileNameOfUrl(release.apk.url, "VanillaDroid-" + release.version + ".apk"));
    Result<void> got = downloadReleaseFile(
        release.apk, file, consoleOperation(context),
        formatText("VD_SETUP_UPDATE_DOWNLOADING", {utf8FromPath(file.filename()), formatBytes(release.apk.size)}));
    g_out.endProgress();
    if (!got) {
        g_out.line(got.error().message);
        return false;
    }
    Result<ApkInfo> info = readApkInfo(file);
    if (!info || info.value().versionCode != release.versionCode) {
        std::error_code error;
        std::filesystem::remove(file, error);
        g_out.say("VD_SETUP_UPDATE_APK_MISMATCH", {release.version});
        return false;
    }
    removeOtherApks(context.dataDir, file);
    g_out.say("VD_SETUP_UPDATE_APK_DOWNLOADED", {info.value().versionName, utf8FromPath(file)});
    if (info.value().package != context.settings.appId) {
        g_out.say("VD_SETUP_ADB_OTHER_APP", {info.value().package, context.settings.appId});
    }
    return true;
}

// The new Setup in place of this one, then run again with the same arguments (it fetches the APK and says what to
// bake).
int replaceSetup(Context& context, const pack::ReleaseManifest& release) {
    const pack::ReleaseFile& setup = release.setupForThisSystem();
    const std::string page = release.notes.empty() ? std::string(pack::kReleasePageUrl) : release.notes;
    if (!setup.present()) {
        g_out.say("VD_SETUP_UPDATE_NO_SETUP", {page});
        return 1;
    }
    const InstallLayout layout = installLayout(runningProgram());
    if (!isInstalledSetup(layout)) {
        g_out.say("VD_SETUP_UPDATE_NOT_INSTALLED", {utf8FromPath(layout.dir), page});
        return 1;
    }
    const std::filesystem::path zip =
        context.dataDir / kUpdateFolder /
        pathFromUtf8(fileNameOfUrl(setup.url, "VanillaDroidSetup-" + release.version + ".zip"));
    const Operation operation = consoleOperation(context);
    Result<void> got = downloadReleaseFile(
        setup, zip, operation,
        formatText("VD_SETUP_UPDATE_DOWNLOADING", {utf8FromPath(zip.filename()), formatBytes(setup.size)}));
    if (!got) {
        g_out.endProgress();
        g_out.line(got.error().message);
        return 1;
    }
    Result<std::filesystem::path> staged = unpackSetup(zip, layout, release.version, operation);
    g_out.endProgress();
    if (!staged) {
        g_out.line(staged.error().message);
        return 1;
    }
    Result<u32> replaced = replaceInstall(staged.value(), layout);
    if (!replaced) {
        g_out.line(replaced.error().message);
        return 1;
    }
    std::error_code error;
    std::filesystem::remove(zip, error);
    g_out.say("VD_SETUP_UPDATE_REPLACED", {release.version});
    std::vector<std::string> args = context.rawArgs;
    args.insert(args.end(), {"--updated-from", kVersion});
    std::fflush(stdout);
    Result<int> code = relaunch(layout.program, args, true);
    if (!code) {
        g_out.say("VD_SETUP_UPDATE_RELAUNCH_FAILED", {code.error().message});
        return 1;
    }
    return code.value();
}

// vdsetup update [--check] [--manifest-url <url>]: what the latest release brings; without --check, this Setup
// replaced by the release's, its APK fetched, and what the packs need baked.
int commandUpdate(Context& context) {
    const std::string url = updateUrlOf(context);
    if (url.empty()) {
        return 2;
    }
    const std::string updatedFrom = context.args.get("updated-from");
    if (!updatedFrom.empty()) {
        g_out.say("VD_SETUP_UPDATE_UPDATED", {updatedFrom, kVersion});
    }
    if (url != pack::kReleaseManifestUrl) {
        g_out.say("VD_SETUP_UPDATE_FROM", {url});
    }
    Result<UpdateCheck> found = checkForUpdate(context.dataDir, url, consoleOperation(context));
    g_out.endProgress();
    if (!found) {
        g_out.line(found.error().message);
        return 1;
    }
    if (!found.value().published) { // phase 22 (polish): no release yet (404): nothing newer, not an error
        g_out.say("VD_SETUP_UPDATE_UP_TO_DATE", {kVersion});
        return 0;
    }
    const pack::ReleaseManifest& release = found.value().release.manifest;
    const std::string page = release.notes.empty() ? std::string(pack::kReleasePageUrl) : release.notes;
    if (release.tooNew()) {
        g_out.say("VD_SETUP_UPDATE_TOO_NEW", {release.version, std::string(pack::kReleasePageUrl)});
        return 1;
    }
    const bool check = context.args.has("check");
    const std::string steps = releaseDataSteps(context, release);
    if (found.value().newer) {
        g_out.say("VD_SETUP_UPDATE_AVAILABLE", {release.version, kVersion, release.date});
        if (!release.notes.empty()) {
            g_out.say("VD_SETUP_UPDATE_NOTES", {release.notes});
        }
        g_out.say(steps.empty() ? "VD_SETUP_UPDATE_DATA_SAME" : "VD_SETUP_UPDATE_DATA_CHANGES", {steps});
        if (check) {
            g_out.say("VD_SETUP_UPDATE_HOW");
            return 0;
        }
        if (!updatedFrom.empty()) {
            // The program that just took this one's place is not the release's: never replace in a loop.
            g_out.say("VD_SETUP_UPDATE_DID_NOT_TAKE", {release.version, page});
            return 1;
        }
        return replaceSetup(context, release);
    }
    g_out.say("VD_SETUP_UPDATE_UP_TO_DATE", {kVersion});
    if (check) {
        return 0;
    }
    bool ok = true;
    if (release.apk.present()) {
        ok = fetchReleaseApk(context, release);
        if (ok) {
            g_out.say("VD_SETUP_UPDATE_APK_NEXT");
        }
    }
    g_out.say(steps.empty() ? "VD_SETUP_UPDATE_DATA_READY" : "VD_SETUP_UPDATE_DATA_BAKE", {steps});
    return ok ? 0 : 1;
}

// --- Commands ----------------------------------------------------------------------------------------------

int commandHelp() {
    g_out.say("VD_SETUP_TITLE", {kVersion});
    g_out.say("VD_SETUP_USAGE");
    return 0;
}

int commandLang(Context& context) {
    if (!context.args.positional.empty()) {
        const std::string wanted = context.args.positional[0];
        const std::vector<std::string> locales = setupLocales();
        const auto it = std::find_if(locales.begin(), locales.end(),
                                     [&](const std::string& l) { return equalsNoCase(l, wanted); });
        if (it == locales.end() && !equalsNoCase(wanted, "system")) {
            std::string list;
            for (const std::string& locale : locales) {
                list += (list.empty() ? "" : ", ") + locale;
            }
            g_out.say("VD_SETUP_LANG_UNKNOWN", {wanted, list});
            return 1;
        }
        context.settings.language = it == locales.end() ? std::string() : *it;
        if (!context.save()) {
            return 1;
        }
        loadSetupTexts(context.settings.language);
    }
    std::string list;
    for (const std::string& locale : setupLocales()) {
        list += (list.empty() ? "" : ", ") + locale;
    }
    g_out.say(context.settings.language.empty() ? "VD_SETUP_LANG_SYSTEM" : "VD_SETUP_LANG_CHOSEN",
              {context.settings.language.empty() ? systemSetupLocale() : context.settings.language, list});
    return 0;
}

int commandWow(Context& context) {
    Settings& settings = context.settings;
    if (const std::string include = context.args.get("include"); !include.empty()) {
        settings.includeArchives =
            equalsNoCase(include, "none") ? std::vector<std::string>() : splitList(include);
    }
    // The folder given, or the one chosen before (checked again: `vdsetup wow` alone tells how it is).
    const std::filesystem::path folder =
        context.args.positional.empty() ? settings.wow : pathFromUtf8(context.args.positional[0]);
    if (folder.empty() && !context.args.has("locale-client")) {
        g_out.say("VD_SETUP_WOW_NONE");
        return 1;
    }
    ClientCheck base;
    if (!folder.empty()) {
        g_out.say("VD_SETUP_WOW_CHECKING", {utf8FromPath(folder)});
        base = checkClient(folder, settings.includeArchives);
        printChecks(base);
        if (!base.usable) {
            g_out.say("VD_SETUP_WOW_REFUSED");
            return 1;
        }
        settings.wow = base.folder;
    }
    if (context.args.has("locale-client")) {
        const std::string option = context.args.get("locale-client");
        if (option.empty() || equalsNoCase(option, "none")) {
            settings.localeClient.clear();
            g_out.say("VD_SETUP_WOW_LOCALE_CLIENT_REMOVED");
        } else {
            const std::filesystem::path second = pathFromUtf8(option);
            g_out.say("VD_SETUP_WOW_CHECKING_LOCALE", {utf8FromPath(second)});
            const ClientCheck check = checkClient(second, settings.includeArchives);
            printChecks(check);
            if (!check.usable) {
                g_out.say("VD_SETUP_WOW_REFUSED");
                return 1;
            }
            if (base.usable && base.locale == check.locale) {
                g_out.say("VD_SETUP_WOW_SAME_LOCALE", {check.locale});
                return 1;
            }
            settings.localeClient = check.folder;
        }
    }
    if (!context.save()) {
        return 1;
    }
    g_out.say("VD_SETUP_WOW_SAVED");
    g_out.say("VD_SETUP_NEXT_BAKE");
    return 0;
}

// The two clients checked again (it takes a moment) for a bake or a status.
struct Clients {
    ClientCheck base;
    ClientCheck locale;
    bool hasLocale = false;
};

bool checkClients(const Context& context, Clients& clients, bool print) {
    if (context.settings.wow.empty()) {
        g_out.say("VD_SETUP_WOW_NONE");
        return false;
    }
    clients.base = checkClient(context.settings.wow, context.settings.includeArchives);
    if (print || !clients.base.usable) {
        g_out.say("VD_SETUP_STATUS_WOW", {utf8FromPath(context.settings.wow)});
        printChecks(clients.base);
    }
    if (!clients.base.usable) {
        return false;
    }
    if (!context.settings.localeClient.empty()) {
        clients.locale = checkClient(context.settings.localeClient, context.settings.includeArchives);
        clients.hasLocale = clients.locale.usable && clients.locale.locale != clients.base.locale;
        if (print || !clients.hasLocale) {
            g_out.say("VD_SETUP_STATUS_LOCALE_CLIENT", {utf8FromPath(context.settings.localeClient)});
            printChecks(clients.locale);
        }
        if (!clients.hasLocale) {
            g_out.say("VD_SETUP_BAKE_LOCALE_CLIENT_SKIPPED");
        }
    }
    return true;
}

BakePlan planFor(const Context& context, const Clients& clients, bool force, const std::string& only) {
    PlanInputs inputs;
    inputs.baseLocale = clients.base.locale;
    inputs.overlayLocale = clients.hasLocale ? clients.locale.locale : std::string();
    const std::string fingerprint = bakeFingerprint(
        clients.base, clients.hasLocale ? &clients.locale : nullptr, context.settings.includeArchives);
    inputs.inputsChanged = !context.settings.lastBake.empty() && context.settings.lastBake != fingerprint;
    inputs.force = force;
    inputs.only = only;
    Result<pack::PackManifest> manifest = pack::PackManifest::load(context.packs());
    const std::vector<PackFile> files = listPackFiles(context.packs());
    return planBake(inputs, manifest ? manifest.value() : pack::PackManifest{}, pack::releaseRecipes(),
                    files);
}

void printPlan(const BakePlan& plan) {
    if (plan.inputsChanged) {
        g_out.say("VD_SETUP_BAKE_INPUTS_CHANGED");
    }
    if (plan.noManifest) {
        g_out.say("VD_SETUP_BAKE_NO_MANIFEST");
    }
    std::string missing;
    for (const std::string& step : plan.missing) {
        missing += (missing.empty() ? "" : ", ") + step;
    }
    if (!missing.empty()) {
        g_out.say("VD_SETUP_BAKE_MISSING", {missing});
    }
    std::string outdated;
    for (const std::string& file : plan.outdated) {
        outdated += (outdated.empty() ? "" : ", ") + file;
    }
    if (!outdated.empty()) {
        g_out.say("VD_SETUP_BAKE_OUTDATED", {outdated});
    }
}

std::string describeJob(const BakeJob& job) {
    std::string text = job.step;
    if (!job.maps.empty()) {
        text += " (";
        for (usize i = 0; i < job.maps.size(); ++i) {
            text += (i ? ", " : "") + job.maps[i];
        }
        text += ")";
    }
    return text;
}

int commandBake(Context& context) {
    const std::string only = context.args.get("only");
    if (!only.empty() && !isBakeStep(only)) {
        std::string steps;
        for (const std::string_view step : bakeSteps()) {
            steps += (steps.empty() ? "" : ", ") + std::string(step);
        }
        g_out.say("VD_SETUP_BAKE_UNKNOWN_STEP", {only, steps});
        return 1;
    }
    Clients clients;
    if (!checkClients(context, clients, false)) {
        return 1;
    }
    const BakePlan plan = planFor(context, clients, context.args.has("force"), only);
    printPlan(plan);
    if (plan.jobs.empty()) {
        g_out.say(only.empty() ? "VD_SETUP_BAKE_UP_TO_DATE" : "VD_SETUP_BAKE_STEP_UP_TO_DATE", {only});
        return 0;
    }
    std::string steps;
    for (const BakeJob& job : plan.jobs) {
        steps += (steps.empty() ? "" : ", ") + describeJob(job);
    }
    g_out.say("VD_SETUP_BAKE_PLAN", {steps});

    BakeRun run;
    run.baker =
        context.args.has("baker") ? pathFromUtf8(context.args.get("baker")) : findBaker(executableDir());
    std::error_code error;
    if (run.baker.empty() || !std::filesystem::is_regular_file(run.baker, error)) {
        g_out.say("VD_SETUP_BAKE_NO_BAKER");
        return 1;
    }
    run.wow = clients.base.folder;
    run.skipArchives = clients.base.skippedArchives(context.settings.includeArchives);
    if (clients.hasLocale) {
        run.localeClient = clients.locale.folder;
        run.localeSkipArchives = clients.locale.skippedArchives(context.settings.includeArchives);
    }
    run.packsDir = context.packs();
    std::filesystem::create_directories(run.packsDir, error);
    const std::filesystem::space_info space = std::filesystem::space(run.packsDir, error);
    constexpr u64 kFullBake = 9ull * 1000 * 1000 * 1000; // packs 4.5 GB + the baker's cache 3.2 GB, with room
    if (!error && space.available < kFullBake) {
        g_out.say("VD_SETUP_BAKE_LOW_SPACE", {formatBytes(space.available), utf8FromPath(run.packsDir)});
    }
    g_out.say("VD_SETUP_BAKE_START", {utf8FromPath(run.packsDir)});
    const auto start = Clock::now();
    Result<BakeOutcome> outcome = runBake(plan.jobs, run, consoleOperation(context));
    g_out.endProgress();
    if (!outcome) {
        g_out.say("VD_SETUP_BAKE_FAILED", {outcome.error().message});
        return 1;
    }
    if (outcome.value().cancelled) {
        g_out.say("VD_SETUP_BAKE_CANCELLED");
        return 1;
    }
    char seconds[32];
    std::snprintf(seconds, sizeof(seconds), "%.0f", secondsBetween(start, Clock::now()));
    if (!outcome.value().failed.empty()) {
        std::string failed;
        for (const std::string& step : outcome.value().failed) {
            failed += (failed.empty() ? "" : ", ") + step;
        }
        g_out.say("VD_SETUP_BAKE_STEPS_FAILED", {failed});
        return 1;
    }
    context.settings.lastBake = bakeFingerprint(clients.base, clients.hasLocale ? &clients.locale : nullptr,
                                                context.settings.includeArchives);
    context.save();
    g_out.say("VD_SETUP_BAKE_DONE", {seconds});
    g_out.say("VD_SETUP_NEXT_PHONE");
    return 0;
}

int commandStatus(Context& context) {
    g_out.say("VD_SETUP_TITLE", {kVersion});
    g_out.say("VD_SETUP_STATUS_SETTINGS", {utf8FromPath(context.settingsFile)});
    Clients clients;
    const bool clientsOk = context.settings.wow.empty() ? false : checkClients(context, clients, true);
    if (context.settings.wow.empty()) {
        g_out.say("VD_SETUP_WOW_NONE");
    }
    // Packs.
    const std::filesystem::path packs = context.packs();
    const std::vector<PackFile> files = listPackFiles(packs);
    u64 bytes = 0;
    for (const PackFile& file : files) {
        bytes += file.size;
    }
    g_out.say("VD_SETUP_STATUS_PACKS",
              {utf8FromPath(packs), std::to_string(files.size()), formatBytes(bytes)});
    if (Result<pack::PackManifest> manifest = pack::PackManifest::load(packs);
        manifest && !manifest.value().packs.empty()) {
        std::string overlays;
        for (const std::string& overlay : manifest.value().overlays) {
            overlays += " + " + overlay;
        }
        g_out.say("VD_SETUP_STATUS_MANIFEST",
                  {manifest.value().bakedBy, manifest.value().clientLocale + overlays});
    }
    if (clientsOk) {
        const BakePlan plan = planFor(context, clients, false, {});
        printPlan(plan);
        g_out.say(plan.jobs.empty() ? "VD_SETUP_STATUS_PACKS_OK" : "VD_SETUP_STATUS_PACKS_BAKE");
    }
    // The programs it drives, and the phone.
    const std::filesystem::path baker = findBaker(executableDir());
    g_out.say(baker.empty() ? "VD_SETUP_STATUS_NO_BAKER" : "VD_SETUP_STATUS_BAKER", {utf8FromPath(baker)});
    const std::filesystem::path apk = findApk(context);
    if (apk.empty()) {
        g_out.say("VD_SETUP_STATUS_NO_APK");
    } else if (Result<ApkInfo> info = readApkInfo(apk); info) {
        g_out.say("VD_SETUP_STATUS_APK", {utf8FromPath(apk), info.value().package, info.value().versionName,
                                          std::to_string(info.value().versionCode)});
    } else {
        g_out.say("VD_SETUP_STATUS_APK_BAD", {utf8FromPath(apk), info.error().message});
    }
    g_out.say("VD_SETUP_STATUS_APP_ID", {context.settings.appId});
    const std::filesystem::path adb = findAdb(context.settings.adb, context.dataDir);
    g_out.say(adb.empty() ? "VD_SETUP_STATUS_NO_ADB" : "VD_SETUP_STATUS_ADB", {utf8FromPath(adb)});
    if (!context.settings.serial.empty()) {
        g_out.say("VD_SETUP_STATUS_SERIAL", {context.settings.serial});
    }
    g_out.say(context.settings.pairingKey.empty() ? "VD_SETUP_STATUS_NOT_PAIRED"
                                                  : "VD_SETUP_STATUS_PAIRING_KEY");
    printUpdateStatus(context); // phase 22 (updates)
    return 0;
}

int commandServe(Context& context) {
    Settings& settings = context.settings;
    if (settings.pairingKey.empty() || context.args.has("new-key")) {
        settings.pairingKey = newPairingKey();
        if (settings.pairingKey.empty()) {
            g_out.say("VD_SETUP_SERVE_NO_RANDOM");
            return 1;
        }
        if (context.args.has("new-key")) {
            g_out.say("VD_SETUP_SERVE_NEW_KEY");
        }
    }
    if (context.args.has("port")) {
        const long port = std::strtol(context.args.get("port").c_str(), nullptr, 10);
        if (port <= 0 || port > 65535) {
            g_out.say("VD_SETUP_SERVE_BAD_PORT", {context.args.get("port")});
            return 1;
        }
        settings.port = static_cast<u16>(port);
    }
    context.save();

    // The address in the QR code.
    const std::vector<LocalAddress> addresses = localAddresses();
    std::string ip = context.args.get("ip");
    u32 forced = 0;
    if (!ip.empty() && !parseIpv4(ip, forced)) {
        g_out.say("VD_SETUP_SERVE_BAD_IP", {ip});
        return 1;
    }
    if (ip.empty()) {
        const LocalAddress* best = pickLanAddress(addresses);
        if (!best) {
            g_out.say("VD_SETUP_SERVE_NO_NETWORK");
            return 1;
        }
        ip = best->text();
    }

    ServeOptions options;
    options.packsDir = context.packs();
    options.apk = findApk(context);
    options.key = settings.pairingKey;
    options.code = newPairingCode();
    options.appId = settings.appId;
    options.htmlLang =
        equalsNoCase(settings.language.empty() ? systemSetupLocale() : settings.language, "esES") ? "es"
                                                                                                  : "en";
    options.hashCache = context.dataDir / "hashes.json";
    if (context.args.has("wow")) {
        if (settings.wow.empty()) {
            g_out.say("VD_SETUP_WOW_NONE");
            return 1;
        }
        const ClientCheck check = checkClient(settings.wow, settings.includeArchives);
        if (!check.usable) {
            printChecks(check);
            return 1;
        }
        options.wowData = check.dataDir;
        for (const ArchiveInfo& archive : check.gameArchives()) {
            options.wowArchives.push_back(archive.name);
        }
        g_out.say("VD_SETUP_SERVE_WOW", {formatBytes(check.gameBytes())});
    }
    if (listPackFiles(options.packsDir).empty()) {
        g_out.say("VD_SETUP_SERVE_NO_PACKS", {utf8FromPath(options.packsDir)});
    }
    if (options.apk.empty()) {
        g_out.say("VD_SETUP_SERVE_NO_APK");
    }

    PhoneServer server(options, [](const ServeEvent& event) {
        switch (event.kind) {
        case ServeEvent::Kind::Preparing: g_out.progress(event.fraction, event.text); break;
        case ServeEvent::Kind::Ready:
            g_out.endProgress();
            g_out.say("VD_SETUP_SERVE_READY", {formatBytes(event.bytes)});
            break;
        case ServeEvent::Kind::PrepareFailed:
            g_out.endProgress();
            g_out.say("VD_SETUP_SERVE_PREPARE_FAILED", {event.text});
            break;
        case ServeEvent::Kind::Download:
            g_out.say(
                event.offset > 0 ? "VD_SETUP_SERVE_RESUME" : "VD_SETUP_SERVE_DOWNLOAD",
                {ipv4Text(event.client), event.path, formatBytes(event.offset), formatBytes(event.total)});
            break;
        case ServeEvent::Kind::Sent:
            g_out.say(event.complete ? "VD_SETUP_SERVE_SENT" : "VD_SETUP_SERVE_CUT",
                      {ipv4Text(event.client), event.path, formatBytes(event.bytes)});
            break;
        case ServeEvent::Kind::Paired: g_out.say("VD_SETUP_SERVE_PAIRED", {ipv4Text(event.client)}); break;
        case ServeEvent::Kind::WrongCode:
            g_out.say("VD_SETUP_SERVE_WRONG_CODE", {ipv4Text(event.client)});
            break;
        case ServeEvent::Kind::CodeLocked: g_out.say("VD_SETUP_SERVE_CODE_LOCKED"); break;
        case ServeEvent::Kind::Unauthorized:
            g_out.say("VD_SETUP_SERVE_UNAUTHORIZED", {ipv4Text(event.client), event.path});
            break;
        case ServeEvent::Kind::Progress: break; // the window's progress bars (phase 22, gui)
        }
    });
    // --loopback (tests): 127.0.0.1 only, no firewall question; the phone cannot reach it.
    const bool loopback = context.args.has("loopback");
    if (loopback) {
        ip = "127.0.0.1";
    }
    Result<u16> port = server.start(settings.port, 20, loopback);
    if (!port) {
        g_out.say("VD_SETUP_SERVE_LISTEN_FAILED", {port.error().message});
        return 1;
    }
    const std::string host = ip + ":" + std::to_string(port.value());
    server.setHost(host);
    const std::string url = "http://" + host + "/?k=" + settings.pairingKey;

    g_out.say("VD_SETUP_SERVE_LISTENING", {host});
    if (addresses.size() > 1) {
        std::string others;
        for (const LocalAddress& address : addresses) {
            if (address.text() != ip) {
                others += (others.empty() ? "" : ", ") + address.text() + " (" + address.adapter + ")";
            }
        }
        g_out.say("VD_SETUP_SERVE_OTHER_ADDRESSES", {others});
    }
    if (!context.args.has("no-qr")) {
        g_out.say("VD_SETUP_SERVE_SCAN");
        if (Result<QrMatrix> qr = encodeQr(url); qr) {
            const std::string drawing = renderQrHalfBlocks(qr.value(), 2, g_ansi);
            std::lock_guard<std::mutex> lock(g_outputMutex);
            std::fwrite(drawing.data(), 1, drawing.size(), stdout);
            std::fflush(stdout);
        }
    }
    g_out.say("VD_SETUP_SERVE_URL", {url});
    g_out.say("VD_SETUP_SERVE_CODE", {host, options.code});
    g_out.say("VD_SETUP_SERVE_FIREWALL");
    g_out.say("VD_SETUP_SERVE_STOP");
    while (!g_cancel.cancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    g_out.endProgress();
    server.stop();
    g_out.say("VD_SETUP_SERVE_STOPPED");
    return 0;
}

// --- adb ---------------------------------------------------------------------------------------------------

std::vector<PushItem> packItems(Context& context, HashCache& cache, bool& ok) {
    ok = false;
    const std::filesystem::path packs = context.packs();
    std::vector<HashJob> jobs;
    std::vector<std::string> names;
    for (const PackFile& file : listPackFiles(packs)) {
        jobs.push_back({packs / pathFromUtf8(file.name), 0, 0, {}});
        names.push_back(file.name);
    }
    if (jobs.empty()) {
        g_out.say("VD_SETUP_SERVE_NO_PACKS", {utf8FromPath(packs)});
        return {};
    }
    if (Result<void> hashed = hashFiles(jobs, cache, consoleOperation(context)); !hashed) {
        g_out.endProgress();
        g_out.line(hashed.error().message);
        return {};
    }
    g_out.endProgress();
    std::vector<PushItem> items;
    for (usize i = 0; i < jobs.size(); ++i) {
        items.push_back({jobs[i].file, names[i], jobs[i].size, jobs[i].sha256});
    }
    // The manifest last, with each pack's digest in it (the phone then knows what it has).
    Result<pack::PackManifest> manifest = pack::PackManifest::load(packs);
    if (manifest && !manifest.value().packs.empty()) {
        bool changed = false;
        for (const PushItem& item : items) {
            if (const pack::PackManifest::Entry* entry = manifest.value().find(item.name);
                entry && entry->size == item.size && entry->sha256 != item.sha256) {
                manifest.value().record(item.name).sha256 = item.sha256;
                changed = true;
            }
        }
        if (changed) {
            static_cast<void>(manifest.value().save(packs));
        }
        const std::filesystem::path file = packs / std::string(pack::kManifestFile);
        std::error_code error;
        Result<std::string> digest = sha256FileProgress(file, {}, &g_cancel);
        if (digest) {
            items.push_back({file, std::string(pack::kManifestFile), std::filesystem::file_size(file, error),
                             digest.value()});
        }
    }
    ok = true;
    return items;
}

int adbHelp() {
    g_out.say("VD_SETUP_ADB_GUIDE");
    return 0;
}

int commandAdb(Context& context) {
    const std::string sub = context.args.positional.empty() ? "help" : context.args.positional[0];
    if (sub == "help") {
        return adbHelp();
    }
    Settings& settings = context.settings;
    if (context.args.has("adb")) {
        settings.adb = pathFromUtf8(context.args.get("adb"));
        context.save();
    }
    if (context.args.has("serial")) {
        settings.serial = context.args.get("serial");
        context.save();
    }
    if (sub == "setup") {
        if (std::filesystem::path existing = findAdb(settings.adb, context.dataDir); !existing.empty()) {
            g_out.say("VD_SETUP_ADB_FOUND", {utf8FromPath(existing)});
            if (!context.args.has("force")) {
                return 0;
            }
        }
        g_out.say("VD_SETUP_ADB_TERMS",
                  {std::string(platformToolsUrl()), std::string(platformToolsTermsUrl())});
        if (!context.args.has("accept")) {
            g_out.say("VD_SETUP_ADB_ACCEPT_PROMPT");
            char answer[16] = {};
            if (!std::fgets(answer, sizeof(answer), stdin) ||
                !(answer[0] == 'y' || answer[0] == 'Y' || answer[0] == 's' || answer[0] == 'S')) {
                g_out.say("VD_SETUP_ADB_DECLINED");
                return 1;
            }
        }
        Result<std::filesystem::path> installed =
            installPlatformTools(context.dataDir, consoleOperation(context));
        g_out.endProgress();
        if (!installed) {
            g_out.line(installed.error().message);
            return 1;
        }
        g_out.say("VD_SETUP_ADB_INSTALLED", {utf8FromPath(installed.value())});
        return 0;
    }
    const std::filesystem::path program = findAdb(settings.adb, context.dataDir);
    if (program.empty()) {
        g_out.say("VD_SETUP_ADB_MISSING");
        return 1;
    }
    Adb adb(program, settings.serial);
    if (sub == "pair" || sub == "connect") {
        if (context.args.positional.size() < (sub == "pair" ? 3u : 2u)) {
            g_out.say(sub == "pair" ? "VD_SETUP_ADB_PAIR_USAGE" : "VD_SETUP_ADB_CONNECT_USAGE");
            return 1;
        }
        Result<std::string> done = sub == "pair"
                                       ? adb.pair(context.args.positional[1], context.args.positional[2])
                                       : adb.connect(context.args.positional[1]);
        if (!done) {
            g_out.line(done.error().message);
            return 1;
        }
        g_out.say(sub == "pair" ? "VD_SETUP_ADB_PAIRED" : "VD_SETUP_ADB_CONNECTED",
                  {context.args.positional[1]});
        if (sub == "pair") {
            g_out.say("VD_SETUP_ADB_NEXT_CONNECT");
        }
        return 0;
    }
    if (sub == "devices") {
        Result<std::vector<AdbDevice>> devices = adb.devices();
        if (!devices) {
            g_out.line(devices.error().message);
            return 1;
        }
        if (devices.value().empty()) {
            g_out.say("VD_SETUP_ADB_NO_DEVICE");
            return 1;
        }
        for (const AdbDevice& device : devices.value()) {
            g_out.say(device.serial == settings.serial ? "VD_SETUP_ADB_DEVICE_CHOSEN" : "VD_SETUP_ADB_DEVICE",
                      {device.serial, device.model, device.state});
        }
        if (devices.value().size() > 1 && settings.serial.empty()) {
            g_out.say("VD_SETUP_ADB_MANY_DEVICES");
        }
        return 0;
    }
    if (sub != "install" && sub != "push" && sub != "send-wow") {
        g_out.say("VD_SETUP_ADB_UNKNOWN", {sub});
        return 1;
    }
    Result<AdbDevice> device = adb.chooseDevice();
    if (!device) {
        g_out.line(device.error().message);
        return 1;
    }
    adb.setSerial(device.value().serial);
    g_out.say("VD_SETUP_ADB_USING", {device.value().serial, device.value().model});

    if (sub == "install") {
        const std::filesystem::path apk = findApk(context);
        if (apk.empty()) {
            g_out.say("VD_SETUP_STATUS_NO_APK");
            return 1;
        }
        Result<ApkInfo> info = readApkInfo(apk);
        if (!info) {
            g_out.say("VD_SETUP_STATUS_APK_BAD", {utf8FromPath(apk), info.error().message});
            return 1;
        }
        if (info.value().package != settings.appId) {
            g_out.say("VD_SETUP_ADB_OTHER_APP", {info.value().package, settings.appId});
            return 1;
        }
        Result<void> installed = adb.install(apk, consoleOperation(context));
        g_out.endProgress();
        if (!installed) {
            g_out.line(installed.error().message);
            return 1;
        }
        g_out.say("VD_SETUP_ADB_INSTALL_DONE", {info.value().versionName});
        g_out.say("VD_SETUP_ADB_OPEN_ONCE");
        return 0;
    }

    // push / send-wow: the app must exist (its folder too: it is made when the app first runs).
    Result<bool> installed = adb.isInstalled(settings.appId);
    if (installed && !installed.value()) {
        g_out.say("VD_SETUP_ADB_NOT_INSTALLED", {settings.appId});
        return 1;
    }
    HashCache cache(context.dataDir / "hashes.json");
    cache.load();
    std::vector<PushItem> items;
    std::string remote;
    if (sub == "push") {
        bool ok = false;
        items = packItems(context, cache, ok);
        if (!ok) {
            return 1;
        }
        remote = remoteFilesDir(settings.appId) + "/packs";
    } else {
        if (settings.wow.empty()) {
            g_out.say("VD_SETUP_WOW_NONE");
            return 1;
        }
        const ClientCheck check = checkClient(settings.wow, settings.includeArchives);
        if (!check.usable) {
            printChecks(check);
            return 1;
        }
        std::vector<HashJob> jobs;
        for (const ArchiveInfo& archive : check.gameArchives()) {
            jobs.push_back({check.dataDir / pathFromUtf8(archive.name), 0, 0, {}});
        }
        if (Result<void> hashed = hashFiles(jobs, cache, consoleOperation(context)); !hashed) {
            g_out.endProgress();
            g_out.line(hashed.error().message);
            return 1;
        }
        g_out.endProgress();
        for (const HashJob& job : jobs) {
            items.push_back({job.file, utf8FromPath(job.file.filename()), job.size, job.sha256});
        }
        remote = remoteFilesDir(settings.appId) + "/wow/Data";
        g_out.say("VD_SETUP_ADB_SEND_WOW", {formatBytes(check.gameBytes())});
    }
    const auto start = Clock::now();
    Result<PushOutcome> pushed = pushFiles(adb, remote, items, consoleOperation(context));
    g_out.endProgress();
    if (!pushed) {
        g_out.line(pushed.error().message);
        return 1;
    }
    char seconds[32];
    std::snprintf(seconds, sizeof(seconds), "%.0f", secondsBetween(start, Clock::now()));
    g_out.say("VD_SETUP_ADB_PUSH_DONE",
              {std::to_string(pushed.value().pushed), formatBytes(pushed.value().bytes),
               std::to_string(pushed.value().skipped), seconds});
    return 0;
}

int run(int argc, char** argv) {
    initConsole();
    Context context;
    std::string bad;
    const std::vector<std::string> raw = utf8Arguments(argc, argv);
    context.rawArgs = raw; // phase 22 (updates)
    // Phase 22 (updates): what an earlier update left (the replaced program, once it no longer runs).
    removeUpdateLeftovers(installLayout(runningProgram()));
    Arguments& args = context.args;
    const bool parsed = parseArguments(raw, args, bad);
    context.dataDir =
        dataDir(args.has("data-dir") ? pathFromUtf8(args.get("data-dir")) : std::filesystem::path());
    context.settingsFile = context.dataDir / kSettingsFile;
    Result<Settings> settings = Settings::load(context.settingsFile);
    if (settings) {
        context.settings = std::move(settings.value());
    }
    loadSetupTexts(context.settings.language);
    if (!settings) {
        g_out.say("VD_SETUP_SETTINGS_DAMAGED", {settings.error().message});
    }
    context.verbose = args.has("verbose");
    vd::log::setMinimumLevel(context.verbose ? vd::log::Level::Debug : vd::log::Level::Error);
    if (!parsed) {
        g_out.say("VD_SETUP_BAD_ARGUMENT", {bad});
        return 2;
    }
    // Options that are settings: remembered.
    bool changed = false;
    if (args.has("packs")) {
        context.settings.packs = pathFromUtf8(args.get("packs"));
        changed = true;
    }
    if (args.has("app-id")) {
        context.settings.appId = args.get("app-id");
        changed = true;
    }
    if (args.has("apk")) {
        context.settings.apk = pathFromUtf8(args.get("apk"));
        changed = true;
    }
    if (changed && !context.save()) {
        return 1;
    }
    if (args.positional.empty() || args.has("help")) {
        return commandHelp();
    }
    const std::string command = args.positional[0];
    args.positional.erase(args.positional.begin());
    if (command == "help") {
        return commandHelp();
    }
    if (command == "status") {
        return commandStatus(context);
    }
    if (command == "wow") {
        return commandWow(context);
    }
    if (command == "bake") {
        return commandBake(context);
    }
    if (command == "serve") {
        return commandServe(context);
    }
    if (command == "adb") {
        return commandAdb(context);
    }
    if (command == "lang") {
        return commandLang(context);
    }
    if (command == "update") { // phase 22 (updates)
        return commandUpdate(context);
    }
    g_out.say("VD_SETUP_UNKNOWN_COMMAND", {command});
    g_out.say("VD_SETUP_USAGE");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    return run(argc, argv);
}
