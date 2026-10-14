#include "ApkInfo.h"

#include "SetupCommon.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>
#include <vector>

namespace vd::setup {
namespace {

constexpr u32 kEndOfDirectory = 0x06054b50;
constexpr u32 kDirectoryEntry = 0x02014b50;
constexpr u32 kLocalHeader = 0x04034b50;
constexpr u32 kMaxManifest = 4u << 20;

constexpr u16 kXmlFile = 0x0003;
constexpr u16 kStringPool = 0x0001;
constexpr u16 kResourceMap = 0x0180;
constexpr u16 kStartElement = 0x0102;
constexpr u32 kUtf8Flag = 1u << 8;
constexpr u32 kVersionCodeId = 0x0101021b; // android:versionCode
constexpr u32 kVersionNameId = 0x0101021c; // android:versionName
constexpr u8 kTypeString = 0x03;
constexpr u8 kTypeIntDec = 0x10;
constexpr u8 kTypeIntHex = 0x11;

u16 u16At(std::span<const u8> b, usize at) {
    return at + 2 <= b.size() ? static_cast<u16>(b[at] | (b[at + 1] << 8)) : 0;
}
u32 u32At(std::span<const u8> b, usize at) {
    return at + 4 <= b.size() ? static_cast<u32>(b[at]) | (static_cast<u32>(b[at + 1]) << 8) |
                                    (static_cast<u32>(b[at + 2]) << 16) | (static_cast<u32>(b[at + 3]) << 24)
                              : 0;
}

// Reads `size` bytes at `offset` of the archive (empty on failure).
using ReadAt = std::function<std::vector<u8>(u64 offset, u64 size)>;

Result<std::vector<u8>> manifestBytes(const ReadAt& read, u64 fileSize) {
    // The end-of-directory record is in the last 64 KB + 22 bytes (its comment is at most 64 KB).
    const u64 tailSize = std::min<u64>(fileSize, 65536 + 22);
    const std::vector<u8> tail = read(fileSize - tailSize, tailSize);
    if (tail.size() < 22) {
        return Error{"not a zip file"};
    }
    usize end = tail.size() - 22 + 1;
    bool found = false;
    while (end-- > 0) {
        if (u32At(tail, end) == kEndOfDirectory) {
            found = true;
            break;
        }
    }
    if (!found) {
        return Error{"not a zip file"};
    }
    const u16 entries = u16At(tail, end + 10);
    const u32 directorySize = u32At(tail, end + 12);
    const u32 directoryOffset = u32At(tail, end + 16);
    const std::vector<u8> directory = read(directoryOffset, directorySize);
    if (directory.size() != directorySize) {
        return Error{"damaged zip directory"};
    }
    usize at = 0;
    for (u32 i = 0; i < entries; ++i) {
        if (u32At(directory, at) != kDirectoryEntry) {
            return Error{"damaged zip directory"};
        }
        const u16 method = u16At(directory, at + 10);
        const u32 compressed = u32At(directory, at + 20);
        const u32 uncompressed = u32At(directory, at + 24);
        const u16 nameLength = u16At(directory, at + 28);
        const u16 extraLength = u16At(directory, at + 30);
        const u16 commentLength = u16At(directory, at + 32);
        const u32 localOffset = u32At(directory, at + 42);
        if (at + 46 + nameLength > directory.size()) {
            return Error{"damaged zip directory"};
        }
        const std::string_view name(reinterpret_cast<const char*>(directory.data() + at + 46), nameLength);
        at += 46u + nameLength + extraLength + commentLength;
        if (name != "AndroidManifest.xml") {
            continue;
        }
        if (uncompressed > kMaxManifest || compressed > kMaxManifest) {
            return Error{"AndroidManifest.xml too large"};
        }
        const std::vector<u8> local = read(localOffset, 30);
        if (u32At(local, 0) != kLocalHeader) {
            return Error{"damaged zip entry"};
        }
        const u64 dataOffset = u64{localOffset} + 30 + u16At(local, 26) + u16At(local, 28);
        const std::vector<u8> data = read(dataOffset, compressed);
        if (data.size() != compressed) {
            return Error{"damaged zip entry"};
        }
        if (method == 0) {
            return data;
        }
        if (method != 8) {
            return Error{"unknown zip compression"};
        }
        std::vector<u8> out(uncompressed);
        z_stream stream{};
        if (inflateInit2(&stream, -15) != Z_OK) { // raw deflate
            return Error{"inflate failed"};
        }
        stream.next_in = const_cast<Bytef*>(data.data());
        stream.avail_in = static_cast<uInt>(data.size());
        stream.next_out = out.data();
        stream.avail_out = static_cast<uInt>(out.size());
        const int status = inflate(&stream, Z_FINISH);
        inflateEnd(&stream);
        if (status != Z_STREAM_END || stream.total_out != uncompressed) {
            return Error{"inflate failed"};
        }
        return out;
    }
    return Error{"no AndroidManifest.xml (not an APK)"};
}

} // namespace

Result<ApkInfo> parseBinaryManifest(std::span<const u8> xml) {
    if (u16At(xml, 0) != kXmlFile || u32At(xml, 4) > xml.size()) {
        return Error{"AndroidManifest.xml is not binary XML"};
    }
    std::vector<std::string> strings;
    std::vector<u32> resourceIds;
    usize at = u16At(xml, 2); // the file's header size
    while (at + 8 <= xml.size()) {
        const u16 type = u16At(xml, at);
        const u16 headerSize = u16At(xml, at + 2);
        const u32 size = u32At(xml, at + 4);
        if (size < 8 || at + size > xml.size()) {
            return Error{"damaged binary XML"};
        }
        const std::span<const u8> chunk = xml.subspan(at, size);
        if (type == kStringPool) {
            const u32 count = u32At(chunk, 8);
            const u32 flags = u32At(chunk, 16);
            const u32 stringsStart = u32At(chunk, 20);
            const bool utf8 = (flags & kUtf8Flag) != 0;
            for (u32 i = 0; i < count; ++i) {
                usize s = stringsStart + u32At(chunk, headerSize + usize{i} * 4);
                std::string text;
                if (utf8) {
                    // UTF-16 length then UTF-8 length, each one or two bytes.
                    s += (s < chunk.size() && (chunk[s] & 0x80)) ? 2 : 1;
                    usize length = s < chunk.size() ? chunk[s] : 0;
                    if (length & 0x80) {
                        length = ((length & 0x7F) << 8) | (s + 1 < chunk.size() ? chunk[s + 1] : 0);
                        s += 2;
                    } else {
                        s += 1;
                    }
                    if (s + length <= chunk.size()) {
                        text.assign(reinterpret_cast<const char*>(chunk.data() + s), length);
                    }
                } else {
                    usize length = u16At(chunk, s);
                    s += 2;
                    if (length & 0x8000) {
                        length = ((length & 0x7FFF) << 16) | u16At(chunk, s);
                        s += 2;
                    }
                    for (usize c = 0; c < length && s + c * 2 + 2 <= chunk.size(); ++c) {
                        const u16 unit = u16At(chunk, s + c * 2);
                        if (unit <
                            0x80) { // manifest names and versions are ASCII; anything else is kept as '?'
                            text.push_back(static_cast<char>(unit));
                        } else {
                            text.push_back('?');
                        }
                    }
                }
                strings.push_back(std::move(text));
            }
        } else if (type == kResourceMap) {
            for (usize i = headerSize; i + 4 <= size; i += 4) {
                resourceIds.push_back(u32At(chunk, i));
            }
        } else if (type == kStartElement) {
            const u32 name = u32At(chunk, headerSize + 4u); // ResXMLTree_attrExt: ns, name, attributeStart...
            if (name >= strings.size() || strings[name] != "manifest") {
                return Error{"AndroidManifest.xml does not start with <manifest>"};
            }
            const u16 attributeStart = u16At(chunk, headerSize + 8u);
            const u16 attributeSize = u16At(chunk, headerSize + 10u);
            const u16 attributeCount = u16At(chunk, headerSize + 12u);
            ApkInfo info;
            for (u32 i = 0; i < attributeCount; ++i) {
                const usize a =
                    usize{headerSize} + attributeStart + usize{i} * attributeSize; // after attrExt's start
                const u32 attributeName = u32At(chunk, a + 4);
                const u32 rawValue = u32At(chunk, a + 8);
                const u8 dataType = a + 15 < chunk.size() ? chunk[a + 15] : 0;
                const u32 data = u32At(chunk, a + 16);
                const u32 id = attributeName < resourceIds.size() ? resourceIds[attributeName] : 0;
                const std::string_view label = attributeName < strings.size()
                                                   ? std::string_view(strings[attributeName])
                                                   : std::string_view();
                auto stringValue = [&]() -> std::string {
                    const u32 index =
                        rawValue != 0xFFFFFFFFu ? rawValue : (dataType == kTypeString ? data : ~0u);
                    return index < strings.size() ? strings[index] : std::string();
                };
                if (label == "package") {
                    info.package = stringValue();
                } else if (id == kVersionCodeId || label == "versionCode") {
                    if (dataType == kTypeIntDec || dataType == kTypeIntHex) {
                        info.versionCode = data;
                    }
                } else if (id == kVersionNameId || label == "versionName") {
                    info.versionName = stringValue();
                }
            }
            if (info.package.empty()) {
                return Error{"AndroidManifest.xml has no package"};
            }
            return info;
        }
        at += size;
    }
    return Error{"AndroidManifest.xml has no <manifest>"};
}

Result<ApkInfo> readApkInfo(std::span<const u8> apk) {
    const ReadAt read = [&](u64 offset, u64 size) {
        if (offset > apk.size() || size > apk.size() - offset) {
            return std::vector<u8>();
        }
        return std::vector<u8>(apk.begin() + static_cast<std::ptrdiff_t>(offset),
                               apk.begin() + static_cast<std::ptrdiff_t>(offset + size));
    };
    Result<std::vector<u8>> manifest = manifestBytes(read, apk.size());
    if (!manifest) {
        return manifest.error();
    }
    return parseBinaryManifest(manifest.value());
}

Result<ApkInfo> readApkInfo(const std::filesystem::path& apk) {
    std::error_code error;
    const u64 size = std::filesystem::file_size(apk, error);
    if (error) {
        return Error{"cannot read " + utf8FromPath(apk)};
    }
    std::ifstream in(apk, std::ios::binary);
    const ReadAt read = [&](u64 offset, u64 count) {
        std::vector<u8> bytes;
        if (offset > size || count > size - offset || count > (64u << 20)) {
            return bytes;
        }
        bytes.resize(static_cast<usize>(count));
        in.clear();
        in.seekg(static_cast<std::streamoff>(offset));
        if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(count))) {
            bytes.clear();
        }
        return bytes;
    };
    Result<std::vector<u8>> manifest = manifestBytes(read, size);
    if (!manifest) {
        return manifest.error();
    }
    return parseBinaryManifest(manifest.value());
}

} // namespace vd::setup
