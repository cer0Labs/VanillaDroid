#include "SetupCommon.h"

#include "core/Text.h"

#include <cstdio>
#include <fstream>

#if defined(_WIN32)
#include <windows.h>

#include <bcrypt.h> // after windows.h
#else
#include <climits>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/random.h>
#endif
#endif

namespace vd::setup {

std::string formatText(std::string_view key, const std::vector<std::string>& args) {
    std::vector<text::Arg> list;
    list.reserve(args.size());
    for (const std::string& arg : args) {
        list.emplace_back(std::string_view(arg));
    }
    // text::vappend stops at 1 KB; the tool's guides are longer: a buffer that grows until the text fits.
    const std::string_view format = text::get(text::Key(key));
    usize capacity = format.size() * 2 + 256;
    for (const std::string& arg : args) {
        capacity += arg.size() * 2;
    }
    std::vector<char> buffer;
    for (;;) {
        buffer.resize(capacity);
        const std::string_view written = text::vformat(buffer, format, list);
        if (written.size() + 8 < capacity || capacity >= (1u << 20)) {
            return std::string(written);
        }
        capacity *= 2;
    }
}

std::string format(const Message& message) {
    return formatText(message.key, message.args);
}

std::filesystem::path pathFromUtf8(std::string_view utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string utf8FromPath(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::string toLowerAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return out;
}

bool equalsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        const char x = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        const char y = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
        if (x != y) {
            return false;
        }
    }
    return true;
}

std::string formatBytes(u64 bytes) {
    // Phase 22 (polish): the number written the language's way ("4.50 GB", "4,50 GB"): the whole part and the
    // decimals go to a text of the language (VD_SETUP_SIZE_*), which puts its own decimal sign between them.
    const auto padded = [](u64 value, usize width) {
        std::string text = std::to_string(value);
        text.insert(0, width > text.size() ? width - text.size() : 0, '0');
        return text;
    };
    if (bytes >= 999'950'000ull) { // what would round to "1000.0 MB" is "1.00 GB"
        const u64 hundredths = (bytes + 5'000'000ull) / 10'000'000ull;
        return formatText("VD_SETUP_SIZE_GB",
                          {std::to_string(hundredths / 100), padded(hundredths % 100, 2)});
    }
    if (bytes >= 999'500ull) { // and "1000 KB" is "1.0 MB"
        const u64 tenths = (bytes + 50'000ull) / 100'000ull;
        return formatText("VD_SETUP_SIZE_MB", {std::to_string(tenths / 10), padded(tenths % 10, 1)});
    }
    if (bytes >= 1000) {
        return formatText("VD_SETUP_SIZE_KB", {std::to_string((bytes + 500) / 1000)});
    }
    return formatText("VD_SETUP_SIZE_B", {std::to_string(bytes)});
}

std::filesystem::path findChildNoCase(const std::filesystem::path& dir, std::string_view name) {
    std::error_code error;
    const std::filesystem::path exact = dir / pathFromUtf8(name);
    if (std::filesystem::exists(exact, error)) {
        // On a case-insensitive file system this finds "data" for "Data": take the spelling on disk.
        for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
            if (equalsNoCase(utf8FromPath(entry.path().filename()), name)) {
                return entry.path();
            }
        }
        return exact;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
        if (equalsNoCase(utf8FromPath(entry.path().filename()), name)) {
            return entry.path();
        }
    }
    return {};
}

std::filesystem::path executableDir() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer).parent_path();
#elif defined(__APPLE__)
    char buffer[PATH_MAX];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) != 0) {
        return {};
    }
    std::error_code error;
    const std::filesystem::path resolved = std::filesystem::canonical(buffer, error);
    return (error ? std::filesystem::path(buffer) : resolved).parent_path();
#else
    std::error_code error;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path() : self.parent_path();
#endif
}

bool randomBytes(void* out, usize size) {
#if defined(_WIN32)
    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(size),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    auto* bytes = static_cast<u8*>(out);
    while (size > 0) {
        const usize chunk = size < 256 ? size : 256; // getentropy's limit
        if (getentropy(bytes, chunk) != 0) {
            std::FILE* urandom = std::fopen("/dev/urandom", "rb");
            if (!urandom) {
                return false;
            }
            const bool read = std::fread(bytes, 1, size, urandom) == size;
            std::fclose(urandom);
            return read;
        }
        bytes += chunk;
        size -= chunk;
    }
    return true;
#endif
}

} // namespace vd::setup
