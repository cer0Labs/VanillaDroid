#include "pack/AppPack.h"

#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace vd::pack {
namespace {

std::string readSmallFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// `text` into `path` through a temporary file and a rename.
bool writeSmallFile(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
}

} // namespace

Result<std::filesystem::path> installAppPack(const std::filesystem::path& dir, std::string_view stamp,
                                             AppPackWriter write, void* user, bool* copied) {
    if (copied) {
        *copied = false;
    }
    const std::filesystem::path target = dir / kAppPackFile;
    std::filesystem::path stampPath = target;
    stampPath += ".stamp";
    std::error_code error;
    if (std::filesystem::is_regular_file(target, error) && std::filesystem::file_size(target, error) > 0 &&
        !error && readSmallFile(stampPath) == stamp) {
        return target;
    }
    std::filesystem::create_directories(dir, error);
    std::filesystem::path temporary = target;
    temporary += ".tmp";
    std::FILE* out = std::fopen(temporary.string().c_str(), "wb");
    if (!out) {
        return Error{"cannot write " + temporary.string()};
    }
    const bool written = write && write(user, out);
    const bool closed = std::fclose(out) == 0;
    if (!written || !closed) {
        std::filesystem::remove(temporary, error);
        return Error{"cannot copy " + std::string(kAppPackFile) + " to " + dir.string()};
    }
    std::filesystem::rename(temporary, target, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return Error{"cannot rename " + temporary.string() + ": " + error.message()};
    }
    if (copied) {
        *copied = true;
    }
    // Without the stamp the next start copies it again: harmless.
    writeSmallFile(stampPath, stamp);
    return target;
}

} // namespace vd::pack
