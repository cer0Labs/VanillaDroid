#pragma once

#include "core/Hash.h"
#include "core/Text.h"
#include "pack/UiFormat.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace vd::test {

// A strings table blob as the baker writes it (pack::UiStringsHeader + UiString sorted by hash + the texts), for
// tests that check worded texts without the packs. Keep the blob alive while it is set (text::setTable keeps a
// reference) and text::unload() at the end of the test.
inline std::vector<u8> textTable(std::vector<std::pair<std::string, std::string>> texts) {
    std::sort(texts.begin(), texts.end(),
              [](const auto& a, const auto& b) { return stringHash(a.first) < stringHash(b.first); });
    pack::UiStringsHeader header{};
    header.count = static_cast<u32>(texts.size());
    header.textOffset = static_cast<u32>(sizeof(header) + texts.size() * sizeof(pack::UiString));
    std::vector<pack::UiString> entries;
    std::string block;
    for (const auto& [key, text] : texts) {
        entries.push_back({stringHash(key), static_cast<u32>(block.size()), static_cast<u32>(text.size())});
        block += text;
        block.push_back('\0');
    }
    header.textSize = static_cast<u32>(block.size());
    std::vector<u8> blob(header.textOffset + block.size());
    std::memcpy(blob.data(), &header, sizeof(header));
    std::memcpy(blob.data() + sizeof(header), entries.data(), entries.size() * sizeof(pack::UiString));
    std::memcpy(blob.data() + header.textOffset, block.data(), block.size());
    return blob;
}

} // namespace vd::test
