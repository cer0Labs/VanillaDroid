#pragma once

#include "core/Types.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Phase 22 (distribution, ADR-0005 §4): VanillaDroid Setup's core (vd_setup), shared by the console tool
// (vdsetup) and, later, the window (22.6). Long operations report their progress through an Operation (a
// step, a fraction, a line of text the player can read) and can be cancelled from another thread; they return
// vd::Result. Everything the player reads is a key of core/Text (locale/<locale>/setup.lang): results carry
// keys and arguments, not sentences.
namespace vd::setup {

// Cancels an operation from another thread (a window's Cancel button, Ctrl+C in the console).
class CancelToken {
public:
    void cancel() { m_cancelled.store(true, std::memory_order_relaxed); }
    void reset() { m_cancelled.store(false, std::memory_order_relaxed); }
    bool cancelled() const { return m_cancelled.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> m_cancelled{false};
};

struct ProgressEvent {
    std::string_view step; // what is running, an id for the program: "hash", "bake:textures", "push"
    f64 fraction = -1.0;   // of the whole operation, 0..1; negative when unknown
    std::string_view text; // a line for the player (already in their language), or a raw tool line
};

// What a long operation needs from its caller. Both members are optional.
struct Operation {
    std::function<void(const ProgressEvent&)> progress;
    const CancelToken* cancel = nullptr;

    void report(std::string_view step, f64 fraction, std::string_view text) const {
        if (progress) {
            progress(ProgressEvent{step, fraction, text});
        }
    }
    bool cancelled() const { return cancel && cancel->cancelled(); }
};

// Something the player should read, as a key of core/Text and its arguments (all strings): a check's
// problems, a step's outcome. format() writes it in the loaded language.
struct Message {
    enum class Level : u8 { Info, Warning, Error };
    Level level = Level::Info;
    std::string key;
    std::vector<std::string> args;
};
std::string format(const Message& message);
// The text of `key` with string arguments (core/Text's formats: %s, %1$s...).
std::string formatText(std::string_view key, const std::vector<std::string>& args = {});

// Paths as UTF-8 in files and on command lines, whatever the system's code page.
std::filesystem::path pathFromUtf8(std::string_view utf8);
std::string utf8FromPath(const std::filesystem::path& path);

// ASCII helpers.
std::string toLowerAscii(std::string_view text);
bool equalsNoCase(std::string_view a, std::string_view b);
// "4.50 GB", "350.0 MB", "12 KB": sizes in decimal units, as Windows Explorer and Android show them roughly.
// Phase 22 (polish): in the loaded language (VD_SETUP_SIZE_*: "4,50 GB" in Spanish).
std::string formatBytes(u64 bytes);

// The child of `dir` named `name` whatever its case ("Data", "data", "DATA"), or an empty path when there is
// none.
std::filesystem::path findChildNoCase(const std::filesystem::path& dir, std::string_view name);

// The folder of the running executable (where the baker, adb and the APK are looked for).
std::filesystem::path executableDir();

// Random bytes from the system's generator (BCryptGenRandom, getentropy / /dev/urandom). False on failure.
bool randomBytes(void* out, usize size);

} // namespace vd::setup
