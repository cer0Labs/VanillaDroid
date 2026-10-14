#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <array>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

// Phase 22 (distribution): SHA-256 (Mbed TLS) of the files that travel to the phone (packs, the WoW's MPQ,
// the APK) and of the release downloads, and the content digests of the packs (their recipes).
namespace vd {

class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void update(std::span<const u8> data);
    void update(const void* data, usize size) { update(std::span(static_cast<const u8*>(data), size)); }
    std::array<u8, 32> finish(); // the object starts over afterwards

    static std::string hex(std::span<const u8> bytes); // lowercase

private:
    struct Context;
    std::unique_ptr<Context> m_context;
};

std::string sha256Hex(std::span<const u8> data);
// The file's digest in lowercase hex, read in 1 MB blocks.
Result<std::string> sha256File(const std::filesystem::path& path);

} // namespace vd
