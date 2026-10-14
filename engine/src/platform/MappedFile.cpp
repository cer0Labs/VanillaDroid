#include "platform/MappedFile.h"

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace vd {
namespace {

// Every open mapping (start and size), for releaseResidentPages(). Changed only when packs open and close.
struct Mappings {
    std::mutex mutex;
    std::vector<std::pair<const u8*, usize>> open;
};

Mappings& mappings() {
    static Mappings m;
    return m;
}

void registerMapping(const u8* data, usize size) {
    std::lock_guard<std::mutex> lock(mappings().mutex);
    mappings().open.emplace_back(data, size);
}

void unregisterMapping(const u8* data) {
    std::lock_guard<std::mutex> lock(mappings().mutex);
    std::vector<std::pair<const u8*, usize>>& open = mappings().open;
    open.erase(std::remove_if(open.begin(), open.end(), [data](const auto& m) { return m.first == data; }), open.end());
}

} // namespace

u64 MappedFile::releaseResidentPages() {
    std::lock_guard<std::mutex> lock(mappings().mutex);
    u64 bytes = 0;
    for (const auto& [data, size] : mappings().open) {
#if !defined(_WIN32)
        madvise(const_cast<u8*>(data), size, MADV_DONTNEED);
#endif
        bytes += size;
    }
    return bytes;
}

MappedFile::~MappedFile() {
    close();
}

MappedFile::MappedFile(MappedFile&& other) noexcept {
    *this = std::move(other);
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        close();
        std::swap(m_data, other.m_data);
        std::swap(m_size, other.m_size);
#if defined(_WIN32)
        std::swap(m_file, other.m_file);
        std::swap(m_mapping, other.m_mapping);
#endif
    }
    return *this;
}

#if defined(_WIN32)

Result<void> MappedFile::open(const std::filesystem::path& path) {
    close();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return Error{"Cannot open " + path.string()};
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart == 0) {
        CloseHandle(file);
        return Error{"Empty or unreadable file " + path.string()};
    }
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) {
        CloseHandle(file);
        return Error{"CreateFileMapping failed for " + path.string()};
    }
    const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(mapping);
        CloseHandle(file);
        return Error{"MapViewOfFile failed for " + path.string()};
    }
    m_file = file;
    m_mapping = mapping;
    m_data = static_cast<const u8*>(view);
    m_size = static_cast<usize>(size.QuadPart);
    registerMapping(m_data, m_size);
    return {};
}

void MappedFile::close() {
    if (m_data) {
        unregisterMapping(m_data);
        UnmapViewOfFile(m_data);
    }
    if (m_mapping) {
        CloseHandle(m_mapping);
    }
    if (m_file) {
        CloseHandle(m_file);
    }
    m_data = nullptr;
    m_size = 0;
    m_mapping = nullptr;
    m_file = nullptr;
}

#else

Result<void> MappedFile::open(const std::filesystem::path& path) {
    close();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return Error{"Cannot open " + path.string()};
    }
    struct stat info {};
    if (fstat(fd, &info) != 0 || info.st_size == 0) {
        ::close(fd);
        return Error{"Empty or unreadable file " + path.string()};
    }
    void* view = mmap(nullptr, static_cast<size_t>(info.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd); // the mapping keeps the file alive
    if (view == MAP_FAILED) {
        return Error{"mmap failed for " + path.string()};
    }
    m_data = static_cast<const u8*>(view);
    m_size = static_cast<usize>(info.st_size);
    registerMapping(m_data, m_size);
    return {};
}

void MappedFile::close() {
    if (m_data) {
        unregisterMapping(m_data);
        munmap(const_cast<u8*>(m_data), m_size);
    }
    m_data = nullptr;
    m_size = 0;
}

#endif

} // namespace vd
