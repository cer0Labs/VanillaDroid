#include "formats/MpqArchiveSet.h"

#include "core/Log.h"

#include <StormLib.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>

namespace vd::formats {
namespace {

std::string lowercase(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Higher value = higher priority. patch-N beats patch-2 beats patch beats base archives.
int archivePriority(const std::string& lowerName) {
    if (lowerName == "patch.mpq") {
        return 100;
    }
    if (lowerName.size() == 11 && lowerName.rfind("patch-", 0) == 0 && lowerName.substr(7) == ".mpq") {
        const char n = lowerName[6];
        if (std::isdigit(static_cast<unsigned char>(n))) {
            return 200 + (n - '0');
        }
        if (std::isalpha(static_cast<unsigned char>(n))) {
            return 300 + (n - 'a'); // patch-A..Z: custom server patches, loaded after the numbered ones
        }
    }
    return 0;
}

bool isInternalFile(const char* name) {
    return name[0] == '(' && (std::strcmp(name, "(listfile)") == 0 || std::strcmp(name, "(attributes)") == 0 ||
                              std::strcmp(name, "(signature)") == 0);
}

} // namespace

std::string toMpqPath(std::string_view path) {
    std::string out(path);
    std::replace(out.begin(), out.end(), '/', '\\');
    return out;
}

MpqArchiveSet::~MpqArchiveSet() {
    close();
}

std::filesystem::path findDataDir(const std::filesystem::path& clientDir) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(clientDir, error)) {
        if (entry.is_directory(error) && lowercase(entry.path().filename().string()) == "data") {
            return entry.path();
        }
    }
    return clientDir / "Data";
}

Result<void> MpqArchiveSet::open(const std::filesystem::path& dataDir, std::span<const std::string> skipArchives) {
    close();
    std::error_code error;
    if (!std::filesystem::is_directory(dataDir, error)) {
        return Error{"Not a directory: " + dataDir.string()};
    }

    struct Candidate {
        std::filesystem::path path;
        std::string lowerName;
        int priority;
    };
    std::vector<Candidate> candidates;
    for (const auto& entry : std::filesystem::directory_iterator(dataDir, error)) {
        const std::string lowerName = lowercase(entry.path().filename().string());
        if (entry.is_regular_file() && lowerName.size() > 4 &&
            lowerName.compare(lowerName.size() - 4, 4, ".mpq") == 0) {
            if (std::any_of(skipArchives.begin(), skipArchives.end(),
                            [&](const std::string& skip) { return lowercase(skip) == lowerName; })) {
                VD_LOG_INFO("MPQ skipped: %s", entry.path().filename().string().c_str()); // phase 22
                continue;
            }
            candidates.push_back({entry.path(), lowerName, archivePriority(lowerName)});
        }
    }
    if (candidates.empty()) {
        return Error{"No .MPQ archives in " + dataDir.string()};
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.priority != b.priority ? a.priority > b.priority : a.lowerName < b.lowerName;
    });

    for (const Candidate& candidate : candidates) {
        HANDLE handle = nullptr;
        if (!SFileOpenArchive(candidate.path.string().c_str(), 0, MPQ_OPEN_READ_ONLY, &handle)) {
            const std::string name = candidate.path.filename().string();
            close();
            return Error{"SFileOpenArchive failed for " + name + " (error " +
                         std::to_string(SErrGetLastError()) + ")"}; // phase 22: also off Windows
        }
        m_archives.push_back({candidate.path.filename().string(), handle});
    }

    std::string order;
    for (const Archive& archive : m_archives) {
        order += (order.empty() ? "" : " > ") + archive.name;
    }
    VD_LOG_INFO("MPQ priority: %s", order.c_str());
    return {};
}

void MpqArchiveSet::close() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (Archive& archive : m_archives) {
        SFileCloseArchive(archive.handle);
    }
    m_archives.clear();
}

bool MpqArchiveSet::contains(std::string_view path) const {
    const std::string mpqPath = toMpqPath(path);
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const Archive& archive : m_archives) {
        if (SFileHasFile(archive.handle, mpqPath.c_str())) {
            return true;
        }
    }
    return false;
}

Result<std::vector<u8>> MpqArchiveSet::read(std::string_view path) const {
    const std::string mpqPath = toMpqPath(path);
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const Archive& archive : m_archives) {
        if (!SFileHasFile(archive.handle, mpqPath.c_str())) {
            continue;
        }
        HANDLE file = nullptr;
        if (!SFileOpenFileEx(archive.handle, mpqPath.c_str(), SFILE_OPEN_FROM_MPQ, &file)) {
            return Error{"SFileOpenFileEx failed: " + mpqPath + " in " + archive.name};
        }
        DWORD sizeHigh = 0;
        const DWORD size = SFileGetFileSize(file, &sizeHigh);
        if (size == 0 && sizeHigh == 0) {
            // patch.MPQ carries 0-byte "deletion" entries for ~5.7k files that still exist in the base
            // archives (old minimap tiles, some creature/item skins). Fall through to the real data.
            SFileCloseFile(file);
            continue;
        }
        std::vector<u8> bytes(size);
        DWORD read = 0;
        const bool ok = SFileReadFile(file, bytes.data(), size, &read, nullptr);
        SFileCloseFile(file);
        if (!ok || read != size || sizeHigh != 0) {
            return Error{"SFileReadFile failed: " + mpqPath + " in " + archive.name};
        }
        return bytes;
    }
    return Error{"File not found in any MPQ: " + mpqPath};
}

std::vector<std::string> MpqArchiveSet::list(const char* mask, std::string_view onlyArchive) const {
    std::vector<std::string> names;
    std::unordered_set<std::string> seen;
    const std::string onlyLower = lowercase(onlyArchive);
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const Archive& archive : m_archives) {
        if (!onlyLower.empty() && lowercase(archive.name) != onlyLower) {
            continue;
        }
        SFILE_FIND_DATA data{};
        HANDLE find = SFileFindFirstFile(archive.handle, mask, &data, nullptr);
        if (!find) {
            continue;
        }
        do {
            if (!isInternalFile(data.cFileName) && seen.insert(lowercase(data.cFileName)).second) {
                names.emplace_back(data.cFileName);
            }
        } while (SFileFindNextFile(find, &data));
        SFileFindClose(find);
    }
    return names;
}

std::vector<std::string> MpqArchiveSet::archiveNames() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> names;
    for (const Archive& archive : m_archives) {
        names.push_back(archive.name);
    }
    return names;
}

} // namespace vd::formats
