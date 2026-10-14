#include "app/PhoneBakeRunner.h"

#include "core/DevEnv.h"
#include "core/Log.h"
#include "platform/Thread.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace vd {
namespace {

constexpr usize kLineBytes = 240; // of the baker's last line kept in status.json

bool readText(const std::filesystem::path& path, std::string& text) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

// Through a temporary name: the game never reads half a status.
void writeAtomically(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::path temp = path;
    temp += ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file) {
            return;
        }
    }
    std::error_code error;
    std::filesystem::rename(temp, path, error);
}

std::filesystem::path pathFromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

// The staging folder's files: what a step cut short left (its .cache folder stays: textures' resume).
void emptyStaging(const std::filesystem::path& staging) {
    std::error_code error;
    std::filesystem::create_directories(staging, error);
    for (const auto& entry : std::filesystem::directory_iterator(staging, error)) {
        if (entry.is_regular_file(error)) {
            std::filesystem::remove(entry.path(), error);
        }
    }
}

std::string clipped(std::string_view line) {
    return std::string(line.substr(0, kLineBytes));
}

} // namespace

i64 phoneBakeNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

PhoneBakeState runPhoneBakePlan(const PhoneBakePlaces& places, const PhoneBakeRunnerHooks& hooks) {
    std::string text;
    if (!readText(places.planFile(), text)) {
        VD_LOG_WARN("Phone bake: no plan in %s", places.bakeDir.string().c_str());
        return PhoneBakeState::None;
    }
    Result<PhoneBakePlan> loaded = readPhoneBakePlan(text);
    if (!loaded) {
        VD_LOG_ERROR("Phone bake: %s", loaded.error().message.c_str());
        return PhoneBakeState::None;
    }
    const PhoneBakePlan& plan = loaded.value();
    const auto now = [&] { return hooks.nowMs ? hooks.nowMs() : phoneBakeNowMs(); };
    const auto cancelled = [&] { return hooks.cancel && hooks.cancel->load(); };
    const std::filesystem::path statusFile = pathFromUtf8(plan.statusFile);

    PhoneBakeRun run;
    if (readText(statusFile, text)) {
        if (Result<PhoneBakeRun> previous = readPhoneBakeRun(text);
            previous && previous.value().plan == plan.id) {
            run = std::move(previous.value());
        }
    }
    if (run.plan == plan.id && run.state == PhoneBakeState::Done) {
        return PhoneBakeState::Done;
    }
    const bool fresh = run.plan != plan.id;
    if (fresh) {
        run = PhoneBakeRun{};
        run.plan = plan.id;
        run.planStartedMs = now();
    }
    run.seconds.resize(std::min<usize>(run.seconds.size(), run.finished));
    std::ofstream log(pathFromUtf8(plan.logFile), fresh ? std::ios::trunc : std::ios::app);
    const auto write = [&] {
        run.updatedMs = now();
        writeAtomically(statusFile, writePhoneBakeRun(run));
    };
    const auto readJobs = [&] {
        f32 headroom = std::numeric_limits<f32>::quiet_NaN();
        i32 status = -1;
        if (hooks.thermal) {
            hooks.thermal(headroom, status);
        }
        run.headroom = std::isnan(headroom) ? -1.0f : headroom;
        run.thermalStatus = status;
        return phoneBakeJobs(plan.thermal, headroom, status);
    };
    // Waits while the phone is too hot to bake (0 threads); false when cancelled meanwhile.
    const auto coolDown = [&](i32& jobs) {
        while (jobs == 0) {
            run.state = PhoneBakeState::Cooling;
            write();
            log << "=== too hot (headroom " << run.headroom << ", status " << run.thermalStatus
                << "): waiting\n";
            log.flush();
            for (u32 waited = 0; waited < hooks.coolingMs; waited += 1000) {
                if (cancelled()) {
                    return false;
                }
                if (hooks.sleep) {
                    hooks.sleep(1000);
                }
            }
            jobs = readJobs();
        }
        return !cancelled();
    };
    const auto stop = [&](PhoneBakeState state) {
        run.state = state;
        write();
        log << "=== " << phoneBakeStateName(state) << "\n";
        return state;
    };

    for (u32 i = run.finished; i < plan.steps.size(); ++i) {
        const PhoneBakePlanStep& step = plan.steps[i];
        run.step = i;
        run.fraction = -1.0f;
        run.line.clear();
        run.error.clear();
        run.exitCode = 0;
        run.stepStartedMs = now();
        i32 jobs = readJobs();
        if (!coolDown(jobs)) {
            return stop(PhoneBakeState::Cancelled);
        }
        emptyStaging(pathFromUtf8(plan.stagingDir));
        run.state = PhoneBakeState::Running;
        run.jobs = jobs;
        write();
        const auto onLine = [&](std::string_view line) {
            log << line << '\n';
            if (const f32 fraction = phoneBakeLineFraction(line, step.name); fraction >= 0.0f) {
                run.fraction = fraction;
            }
            if (const std::string_view error = phoneBakeErrorText(line); !error.empty()) {
                run.error = clipped(error);
            }
            run.line = clipped(line);
        };
        i32 code = 0;
        for (;;) { // the step; started again with fewer threads when the phone heats (textures)
            log << "=== " << step.name << " (" << (i + 1) << " of " << plan.steps.size() << "), " << jobs
                << " threads\n";
            log.flush();
            const i64 started = now();
            i64 lastBeat = started;
            i64 lastThermal = started;
            bool restart = false;
            i32 wanted = jobs;
            std::vector<std::string> args = step.bake;
            args.insert(args.end(), {"--jobs", std::to_string(jobs)});
            code = hooks.runBaker(args, onLine, [&] {
                if (cancelled()) {
                    return true;
                }
                const i64 t = now();
                if (t - lastBeat >= hooks.heartbeatMs) {
                    lastBeat = t;
                    write();
                }
                if (t - lastThermal >= hooks.thermalMs) {
                    lastThermal = t;
                    wanted = readJobs();
                    if (step.restartable && phoneBakeRestart(jobs, wanted, (t - started) / 1e3)) {
                        restart = true;
                        return true;
                    }
                }
                return false;
            });
            if (cancelled()) {
                return stop(PhoneBakeState::Cancelled);
            }
            if (!restart) {
                break;
            }
            log << "=== " << step.name << ": the phone heats, again with " << wanted << " threads\n";
            jobs = wanted;
            if (!coolDown(jobs)) {
                return stop(PhoneBakeState::Cancelled);
            }
            run.state = PhoneBakeState::Running;
            run.jobs = jobs;
            write();
        }
        if (code != 0) {
            run.exitCode = code;
            if (run.error.empty()) {
                run.error = run.line;
            }
            return stop(PhoneBakeState::Failed);
        }
        // Its packs into packs/ (never stopped half way: it only renames).
        code = hooks.runBaker(step.install, onLine, [] { return false; });
        if (code != 0) {
            run.exitCode = code;
            if (run.error.empty()) {
                run.error = run.line;
            }
            return stop(PhoneBakeState::Failed);
        }
        run.finished = i + 1;
        run.seconds.push_back(static_cast<f64>(now() - run.stepStartedMs) / 1e3);
        run.fraction = 1.0f;
        write();
    }
    run.step = static_cast<u32>(plan.steps.size());
    return stop(PhoneBakeState::Done);
}

#if defined(_WIN32)
namespace {

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<usize>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}

// One argument as Windows programs split their command line (CommandLineToArgvW and the C runtime's rules).
std::wstring quoted(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring out = L"\"";
    usize backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        out.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

// vd_baker.exe as a child process, below normal priority (the PC stays usable), its output (both streams)
// line by line on this thread; only the pipe's end is inherited (never the game's sockets).
i32 runDesktopBaker(const std::filesystem::path& baker, const std::vector<std::string>& args,
                    const std::function<void(std::string_view)>& onLine, const std::function<bool()>& tick) {
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &inheritable, 1 << 16)) {
        return -1;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<u8> attributeBuffer(attributeSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
    const bool listReady = InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize) &&
                           UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                     &writeEnd, sizeof(HANDLE), nullptr, nullptr);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = writeEnd;
    startup.StartupInfo.hStdError = writeEnd;
    startup.lpAttributeList = listReady ? attributes : nullptr;
    std::wstring command = quoted(baker.wstring());
    for (const std::string& argument : args) {
        command += L' ';
        command += quoted(widen(argument));
    }
    PROCESS_INFORMATION process{};
    const DWORD flags =
        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | (listReady ? EXTENDED_STARTUPINFO_PRESENT : 0);
    const BOOL started = CreateProcessW(baker.wstring().c_str(), command.data(), nullptr, nullptr, TRUE,
                                        flags, nullptr, nullptr, &startup.StartupInfo, &process);
    if (listReady) {
        DeleteProcThreadAttributeList(attributes);
    }
    CloseHandle(writeEnd);
    if (!started) {
        CloseHandle(readEnd);
        VD_LOG_ERROR("Phone bake: cannot start %s (error %lu)", baker.string().c_str(), GetLastError());
        return -1;
    }
    CloseHandle(process.hThread);
    std::string line;
    const auto feed = [&](const char* data, DWORD size) {
        for (DWORD i = 0; i < size; ++i) {
            if (data[i] == '\n' || data[i] == '\r') {
                if (!line.empty()) {
                    onLine(line);
                    line.clear();
                }
            } else {
                line.push_back(data[i]);
            }
        }
    };
    char buffer[8192];
    bool exited = false;
    bool stopped = false;
    auto lastTick = std::chrono::steady_clock::now();
    for (;;) {
        DWORD available = 0;
        const bool open = PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr) != FALSE;
        if (open && available > 0) {
            DWORD got = 0;
            if (ReadFile(readEnd, buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr) &&
                got > 0) {
                feed(buffer, got);
                continue;
            }
        }
        if (exited) {
            break;
        }
        if (WaitForSingleObject(process.hProcess, 50) == WAIT_OBJECT_0) {
            exited = true; // one more pass drains what is left
            continue;
        }
        if (std::chrono::steady_clock::now() - lastTick >= std::chrono::seconds(1)) {
            lastTick = std::chrono::steady_clock::now();
            if (tick()) {
                TerminateProcess(process.hProcess, 1);
                WaitForSingleObject(process.hProcess, 5000);
                stopped = true;
                break;
            }
        }
    }
    if (!line.empty()) {
        onLine(line);
    }
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(readEnd);
    return stopped ? -1 : static_cast<i32>(code);
}

class DesktopBakeLauncher final : public BakeLauncher {
public:
    explicit DesktopBakeLauncher(std::filesystem::path baker) : m_baker(std::move(baker)) {}
    ~DesktopBakeLauncher() override {
        m_cancel = true;
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    bool start(const std::filesystem::path& bakeDir) override {
        if (m_thread.joinable()) {
            if (!m_finished.load()) {
                return true; // already on it
            }
            m_thread.join();
        }
        if (m_baker.empty()) {
            VD_LOG_ERROR("Phone bake: no vd_baker.exe to run (build it, or set VD_BAKER)");
            return false;
        }
        m_cancel = false;
        m_finished = false;
        m_thread = std::thread([this, bakeDir] {
            setCurrentThreadName("vd-bakerun");
            PhoneBakeRunnerHooks hooks;
            hooks.runBaker = [this](const std::vector<std::string>& args,
                                    const std::function<void(std::string_view)>& onLine,
                                    const std::function<bool()>& tick) {
                return runDesktopBaker(m_baker, args, onLine, tick);
            };
            hooks.sleep = [](u32 ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
            hooks.cancel = &m_cancel;
            const PhoneBakeState state = runPhoneBakePlan(PhoneBakePlaces{{}, {}, bakeDir}, hooks);
            VD_LOG_INFO("Phone bake (desktop): %.*s", static_cast<int>(phoneBakeStateName(state).size()),
                        phoneBakeStateName(state).data());
            m_finished = true;
        });
        return true;
    }
    void cancel(const std::filesystem::path& bakeDir) override {
        static_cast<void>(bakeDir);
        m_cancel = true;
    }

private:
    std::filesystem::path m_baker;
    std::thread m_thread;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_finished{true};
};

} // namespace

std::unique_ptr<BakeLauncher> makeDesktopBakeLauncher(std::filesystem::path baker) {
    return std::make_unique<DesktopBakeLauncher>(std::move(baker));
}

std::filesystem::path findDesktopBaker() {
    std::error_code error;
    if (const char* chosen = VD_DEV_ENV("VD_BAKER"); chosen && *chosen) {
        return std::filesystem::path(chosen);
    }
    wchar_t module[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    const std::filesystem::path dir = std::filesystem::path(module).parent_path();
    // Next to the program, or build/<preset>/apps/desktop/<config>/ -> build/<preset>/tools/baker/<config>/.
    for (const std::filesystem::path& candidate :
         {dir / "vd_baker.exe", dir.parent_path().parent_path().parent_path() / "tools" / "baker" /
                                    dir.filename() / "vd_baker.exe"}) {
        if (std::filesystem::is_regular_file(candidate, error)) {
            return candidate;
        }
    }
    return {};
}
#endif

} // namespace vd
