#pragma once

#include "SetupCommon.h"
#include "core/Result.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Phase 22: the programs VanillaDroid Setup drives (vd_baker, adb, curl, tar), run as child processes: their
// output line by line (stdout and stderr together) while they run, cancellable, with no window of their own
// and no handle of ours inherited (a listening socket must not outlive us in an adb server). Arguments are
// UTF-8.
namespace vd::setup {

struct ProcessOptions {
    std::filesystem::path program;
    std::vector<std::string> args; // without the program itself
    // Each line of output without its end ("\n", "\r\n", or "\r" for progress meters), as it comes.
    std::function<void(std::string_view line)> onLine;
    const CancelToken* cancel = nullptr;
};

struct ProcessResult {
    int exitCode = -1;
    bool cancelled = false; // stopped through the token (exitCode is then meaningless)
};

// Runs the program to its end (or until cancelled: it is terminated). An error when it cannot be started.
Result<ProcessResult> runProcess(const ProcessOptions& options);

// Runs it and returns its whole output (both streams) in `output`.
Result<ProcessResult> runProcessCapture(const std::filesystem::path& program,
                                        const std::vector<std::string>& args, std::string& output,
                                        const CancelToken* cancel = nullptr);

// One argument as Windows programs split their command line (CommandLineToArgvW and the C runtime's rules).
std::string quoteWindowsArgument(std::string_view argument);

// An executable named `name` ("adb", "curl") in the PATH, with ".exe" on Windows; empty when there is none.
std::filesystem::path findInPath(std::string_view name);

// `name` with the system's executable suffix ("vd_baker" -> "vd_baker.exe" on Windows).
std::string executableName(std::string_view name);

} // namespace vd::setup
