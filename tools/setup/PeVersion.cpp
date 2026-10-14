#include "PeVersion.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace vd::setup {
namespace {

constexpr u32 kResourceDirectory = 2; // IMAGE_DIRECTORY_ENTRY_RESOURCE
constexpr u32 kRtVersion = 16;        // RT_VERSION
constexpr u32 kFixedInfoSignature = 0xFEEF04BDu;
constexpr usize kMaxImageSize = 64u << 20; // larger is not a 1.12 client

// Little-endian reads that fail softly past the end.
class Reader {
public:
    explicit Reader(std::span<const u8> bytes) : m_bytes(bytes) {}
    bool has(u64 offset, u64 size) const {
        return offset <= m_bytes.size() && size <= m_bytes.size() - offset;
    }
    u16 u16At(u64 offset) const {
        if (!has(offset, 2)) {
            return 0;
        }
        return static_cast<u16>(m_bytes[offset] | (m_bytes[offset + 1] << 8));
    }
    u32 u32At(u64 offset) const {
        if (!has(offset, 4)) {
            return 0;
        }
        return static_cast<u32>(m_bytes[offset]) | (static_cast<u32>(m_bytes[offset + 1]) << 8) |
               (static_cast<u32>(m_bytes[offset + 2]) << 16) | (static_cast<u32>(m_bytes[offset + 3]) << 24);
    }

private:
    std::span<const u8> m_bytes;
};

struct Section {
    u32 virtualAddress;
    u32 virtualSize;
    u32 rawOffset;
    u32 rawSize;
};

FileVersion versionFrom(u32 ms, u32 ls) {
    return FileVersion{static_cast<u16>(ms >> 16), static_cast<u16>(ms & 0xFFFF), static_cast<u16>(ls >> 16),
                       static_cast<u16>(ls & 0xFFFF)};
}

} // namespace

std::string FileVersion::toString() const {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", major, minor, patch, build);
    return buffer;
}

Result<FileVersion> readPeFileVersion(std::span<const u8> image) {
    const Reader r(image);
    if (!r.has(0, 64) || image[0] != 'M' || image[1] != 'Z') {
        return Error{"not a Windows program (no MZ header)"};
    }
    const u32 pe = r.u32At(0x3C);
    if (!r.has(pe, 24) || std::memcmp(image.data() + pe, "PE\0\0", 4) != 0) {
        return Error{"not a Windows program (no PE header)"};
    }
    const u16 sectionCount = r.u16At(pe + 6);
    const u16 optionalSize = r.u16At(pe + 20);
    const u64 optional = pe + 24;
    const u16 magic = r.u16At(optional);
    u64 directoryCountAt = 0;
    u64 directories = 0;
    if (magic == 0x10B) { // PE32
        directoryCountAt = optional + 92;
        directories = optional + 96;
    } else if (magic == 0x20B) { // PE32+
        directoryCountAt = optional + 108;
        directories = optional + 112;
    } else {
        return Error{"unknown PE optional header"};
    }
    if (r.u32At(directoryCountAt) <= kResourceDirectory) {
        return Error{"no resources"};
    }
    const u32 resourceRva = r.u32At(directories + kResourceDirectory * 8);
    if (resourceRva == 0) {
        return Error{"no resources"};
    }

    std::vector<Section> sections;
    const u64 table = optional + optionalSize;
    for (u32 i = 0; i < sectionCount; ++i) {
        const u64 at = table + u64{i} * 40;
        if (!r.has(at, 40)) {
            return Error{"truncated section table"};
        }
        sections.push_back({r.u32At(at + 12), r.u32At(at + 8), r.u32At(at + 20), r.u32At(at + 16)});
    }
    // The file offset of `size` bytes at `rva`, or 0 when no section holds them.
    auto offsetOf = [&](u32 rva, u32 size) -> u64 {
        for (const Section& s : sections) {
            const u32 span = s.virtualSize > s.rawSize ? s.virtualSize : s.rawSize;
            if (rva >= s.virtualAddress && rva - s.virtualAddress < span) {
                const u64 delta = rva - s.virtualAddress;
                if (delta + size > s.rawSize) {
                    return 0;
                }
                const u64 offset = s.rawOffset + delta;
                return r.has(offset, size) ? offset : 0;
            }
        }
        return 0;
    };
    const u64 root = offsetOf(resourceRva, 16);
    if (root == 0) {
        return Error{"resources outside the file"};
    }
    // One level of IMAGE_RESOURCE_DIRECTORY: the entry with `id` (or the first one when id is 0), as the
    // offset of its target from the start of the resources, with the subdirectory bit.
    auto entryOf = [&](u64 directory, u32 id, u32& target) -> bool {
        const u32 named = r.u16At(directory + 12);
        const u32 ids = r.u16At(directory + 14);
        for (u32 i = 0; i < named + ids; ++i) {
            const u64 entry = directory + 16 + u64{i} * 8;
            if (!r.has(entry, 8)) {
                return false;
            }
            const u32 name = r.u32At(entry);
            if (id == 0 || (!(name & 0x80000000u) && name == id)) {
                target = r.u32At(entry + 4);
                return true;
            }
        }
        return false;
    };
    u32 target = 0;
    u64 directory = root;
    for (const u32 id : {kRtVersion, 0u, 0u}) { // type, name, language: subdirectories, then the data entry
        if (!entryOf(directory, id, target)) {
            return Error{"no version resource"};
        }
        if (target & 0x80000000u) {
            directory = offsetOf(resourceRva + (target & 0x7FFFFFFFu), 16);
            if (directory == 0) {
                return Error{"damaged resources"};
            }
        } else {
            break;
        }
    }
    if (target & 0x80000000u) {
        return Error{"damaged resources"};
    }
    const u64 dataEntry = offsetOf(resourceRva + target, 16);
    if (dataEntry == 0) {
        return Error{"damaged resources"};
    }
    const u32 dataRva = r.u32At(dataEntry);
    const u32 dataSize = r.u32At(dataEntry + 4);
    const u64 data = offsetOf(dataRva, dataSize);
    if (data == 0 || dataSize < 40 + 52) {
        return Error{"damaged version resource"};
    }
    // VS_VERSIONINFO: wLength, wValueLength, wType, L"VS_VERSION_INFO", padding to 4 bytes, VS_FIXEDFILEINFO.
    u64 fixed = data + 40;
    if (r.u32At(fixed) != kFixedInfoSignature) {
        fixed = 0;
        for (u64 at = data; at + 52 <= data + dataSize; at += 4) {
            if (r.u32At(at) == kFixedInfoSignature) {
                fixed = at;
                break;
            }
        }
        if (fixed == 0) {
            return Error{"no VS_FIXEDFILEINFO"};
        }
    }
    return versionFrom(r.u32At(fixed + 8), r.u32At(fixed + 12));
}

Result<FileVersion> readPeFileVersionFile(const std::filesystem::path& exe) {
    std::error_code error;
    const u64 size = std::filesystem::file_size(exe, error);
    if (error) {
        return Error{"cannot read the file"};
    }
    if (size > kMaxImageSize) {
        return Error{"too large for a 1.12 client"};
    }
    std::vector<u8> bytes(static_cast<usize>(size));
    std::ifstream in(exe, std::ios::binary);
    if (!in || !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return Error{"cannot read the file"};
    }
    return readPeFileVersion(bytes);
}

std::string plistString(std::string_view plist, std::string_view key) {
    const std::string needle = "<key>" + std::string(key) + "</key>";
    const usize at = plist.find(needle);
    if (at == std::string_view::npos) {
        return {};
    }
    const usize open = plist.find("<string>", at + needle.size());
    const usize nextKey = plist.find("<key>", at + needle.size());
    if (open == std::string_view::npos || (nextKey != std::string_view::npos && nextKey < open)) {
        return {};
    }
    const usize begin = open + 8;
    const usize close = plist.find("</string>", begin);
    return close == std::string_view::npos ? std::string() : std::string(plist.substr(begin, close - begin));
}

} // namespace vd::setup
