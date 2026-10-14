#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <filesystem>

namespace vd {

// Read-only memory-mapped file. Pages are loaded by the OS on first touch, so opening a multi-GB
// pack is instant and only the assets actually used ever reach RAM.
class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile();
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    Result<void> open(const std::filesystem::path& path);
    void close();

    const u8* data() const { return m_data; }
    usize size() const { return m_size; }

    // Phase 10 (memory in the background): drops the pages of every open mapping from this process's memory now
    // (Android: madvise MADV_DONTNEED; they are clean, so the next touch reads them from the file again; the kernel
    // would take them under pressure anyway, but a smaller process is the last the low-memory killer picks). Safe
    // while other threads read the mappings. Returns the bytes mapped; a no-op on the desktop.
    static u64 releaseResidentPages();

private:
    const u8* m_data = nullptr;
    usize m_size = 0;
#if defined(_WIN32)
    void* m_file = nullptr;
    void* m_mapping = nullptr;
#endif
};

} // namespace vd
