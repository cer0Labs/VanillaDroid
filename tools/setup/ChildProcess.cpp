#include "ChildProcess.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace vd::setup {
namespace {

// Splits a byte stream into lines; "\r" ends a line too (curl's and adb's progress meters rewrite one line).
class LineSplitter {
public:
    explicit LineSplitter(const std::function<void(std::string_view)>& onLine) : m_onLine(onLine) {}

    void feed(const char* data, usize size) {
        for (usize i = 0; i < size; ++i) {
            const char c = data[i];
            if (c == '\n' || c == '\r') {
                if (!(c == '\n' && m_lastWasCr)) {
                    emit();
                }
                m_lastWasCr = c == '\r';
                continue;
            }
            m_lastWasCr = false;
            m_line.push_back(c);
        }
    }
    void finish() {
        if (!m_line.empty()) {
            emit();
        }
    }

private:
    void emit() {
        if (m_onLine) {
            m_onLine(m_line);
        }
        m_line.clear();
    }

    const std::function<void(std::string_view)>& m_onLine;
    std::string m_line;
    bool m_lastWasCr = false;
};

#if defined(_WIN32)
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
#endif

} // namespace

std::string quoteWindowsArgument(std::string_view argument) {
    if (!argument.empty() && argument.find_first_of(" \t\n\v\"") == std::string_view::npos) {
        return std::string(argument);
    }
    std::string out = "\"";
    usize backslashes = 0;
    for (const char c : argument) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\'); // the backslashes, doubled, and one for the quote
        } else {
            out.append(backslashes, '\\');
        }
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, '\\'); // before the closing quote
    out.push_back('"');
    return out;
}

std::string executableName(std::string_view name) {
#if defined(_WIN32)
    return std::string(name) + ".exe";
#else
    return std::string(name);
#endif
}

std::filesystem::path findInPath(std::string_view name) {
#if defined(_WIN32)
    const wchar_t* path = _wgetenv(L"PATH");
    const wchar_t separator = L';';
    if (!path) {
        return {};
    }
    const std::wstring list(path);
#else
    const char* path = std::getenv("PATH");
    const char separator = ':';
    if (!path) {
        return {};
    }
    const std::string list(path);
#endif
    const std::filesystem::path file = pathFromUtf8(executableName(name));
    usize begin = 0;
    while (begin <= list.size()) {
        usize end = list.find(separator, begin);
        if (end == list.npos) {
            end = list.size();
        }
        if (end > begin) {
            auto dir = list.substr(begin, end - begin);
#if defined(_WIN32)
            if (dir.size() >= 2 && dir.front() == L'"' && dir.back() == L'"') {
                dir = dir.substr(1, dir.size() - 2);
            }
#endif
            const std::filesystem::path candidate = std::filesystem::path(dir) / file;
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error)) {
#if !defined(_WIN32)
                if (access(candidate.c_str(), X_OK) != 0) {
                    begin = end + 1;
                    continue;
                }
#endif
                return candidate;
            }
        }
        begin = end + 1;
    }
    return {};
}

#if defined(_WIN32)

Result<ProcessResult> runProcess(const ProcessOptions& options) {
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &inheritable, 1 << 16)) {
        return Error{"CreatePipe failed"};
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                               OPEN_EXISTING, 0, nullptr);

    // Only these two handles reach the child (PROC_THREAD_ATTRIBUTE_HANDLE_LIST).
    HANDLE inherited[2] = {writeEnd, input};
    const DWORD inheritedCount = input != INVALID_HANDLE_VALUE ? 2 : 1;
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<u8> attributeBuffer(attributeSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
    const bool listReady =
        InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize) &&
        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                  inheritedCount * sizeof(HANDLE), nullptr, nullptr);

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input != INVALID_HANDLE_VALUE ? input : nullptr;
    startup.StartupInfo.hStdOutput = writeEnd;
    startup.StartupInfo.hStdError = writeEnd;
    startup.lpAttributeList = listReady ? attributes : nullptr;

    std::string commandLine = quoteWindowsArgument(utf8FromPath(options.program));
    for (const std::string& argument : options.args) {
        commandLine += ' ';
        commandLine += quoteWindowsArgument(argument);
    }
    std::wstring wideCommand = widen(commandLine);
    const std::wstring program = options.program.wstring();
    PROCESS_INFORMATION process{};
    const DWORD flags =
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | (listReady ? EXTENDED_STARTUPINFO_PRESENT : 0);
    const BOOL started = CreateProcessW(program.c_str(), wideCommand.data(), nullptr, nullptr, TRUE, flags,
                                        nullptr, nullptr, &startup.StartupInfo, &process);
    const DWORD startError = GetLastError();
    if (listReady) {
        DeleteProcThreadAttributeList(attributes);
    }
    CloseHandle(writeEnd);
    if (input != INVALID_HANDLE_VALUE) {
        CloseHandle(input);
    }
    if (!started) {
        CloseHandle(readEnd);
        return Error{"Cannot start " + utf8FromPath(options.program) + " (error " +
                     std::to_string(startError) + ")"};
    }
    CloseHandle(process.hThread);

    ProcessResult result;
    LineSplitter lines(options.onLine);
    char buffer[16384];
    bool exited = false;
    for (;;) {
        DWORD available = 0;
        const bool pipeOpen = PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr) != FALSE;
        if (pipeOpen && available > 0) {
            DWORD got = 0;
            if (ReadFile(readEnd, buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr) &&
                got > 0) {
                lines.feed(buffer, got);
                continue;
            }
        }
        if (exited) {
            // Done once the program ended and its output is drained. A program it left running (adb's server)
            // may keep the pipe open: the exit of ours is what counts.
            break;
        }
        if (WaitForSingleObject(process.hProcess, pipeOpen ? 0 : 50) == WAIT_OBJECT_0) {
            exited = true; // one more pass drains what is left
            continue;
        }
        if (options.cancel && options.cancel->cancelled()) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
            result.cancelled = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    lines.finish();
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    result.exitCode = static_cast<int>(code);
    CloseHandle(process.hProcess);
    CloseHandle(readEnd);
    return result;
}

#else

Result<ProcessResult> runProcess(const ProcessOptions& options) {
    int fds[2];
    if (pipe(fds) != 0) {
        return Error{"pipe failed"};
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1); // dup2 clears close-on-exec on 1 and 2
    posix_spawn_file_actions_adddup2(&actions, fds[1], 2);

    const std::string program = utf8FromPath(options.program);
    std::vector<std::string> storage;
    storage.push_back(program);
    storage.insert(storage.end(), options.args.begin(), options.args.end());
    std::vector<char*> argv;
    for (std::string& argument : storage) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, program.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (spawned != 0) {
        close(fds[0]);
        return Error{"Cannot start " + program + " (" + std::strerror(spawned) + ")"};
    }
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);

    ProcessResult result;
    LineSplitter lines(options.onLine);
    char buffer[16384];
    bool exited = false;
    int status = 0;
    for (;;) {
        pollfd descriptor{fds[0], POLLIN, 0};
        const int ready = poll(&descriptor, 1, exited ? 0 : 50);
        if (ready > 0) {
            const ssize_t got = read(fds[0], buffer, sizeof(buffer));
            if (got > 0) {
                lines.feed(buffer, static_cast<usize>(got));
                continue;
            }
            if (got == 0 && !exited) { // every writer closed: wait for the program
                waitpid(pid, &status, 0);
                exited = true;
                break;
            }
        }
        if (exited) {
            break; // drained
        }
        if (waitpid(pid, &status, WNOHANG) == pid) {
            exited = true;
            continue;
        }
        if (options.cancel && options.cancel->cancelled()) {
            kill(pid, SIGTERM);
            for (int i = 0; i < 100 && waitpid(pid, &status, WNOHANG) != pid; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (waitpid(pid, &status, WNOHANG) == 0) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
            }
            result.cancelled = true;
            break;
        }
    }
    lines.finish();
    close(fds[0]);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

#endif

Result<ProcessResult> runProcessCapture(const std::filesystem::path& program,
                                        const std::vector<std::string>& args, std::string& output,
                                        const CancelToken* cancel) {
    output.clear();
    ProcessOptions options;
    options.program = program;
    options.args = args;
    options.cancel = cancel;
    options.onLine = [&](std::string_view line) {
        output.append(line);
        output.push_back('\n');
    };
    return runProcess(options);
}

} // namespace vd::setup
