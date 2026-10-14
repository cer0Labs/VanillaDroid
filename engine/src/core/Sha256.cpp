#include "core/Sha256.h"

#include <mbedtls/sha256.h>

#include <cstdio>
#include <vector>

namespace vd {

struct Sha256::Context {
    mbedtls_sha256_context sha;
};

Sha256::Sha256() : m_context(std::make_unique<Context>()) {
    mbedtls_sha256_init(&m_context->sha);
    mbedtls_sha256_starts(&m_context->sha, 0);
}

Sha256::~Sha256() {
    mbedtls_sha256_free(&m_context->sha);
}

void Sha256::update(std::span<const u8> data) {
    mbedtls_sha256_update(&m_context->sha, data.data(), data.size());
}

std::array<u8, 32> Sha256::finish() {
    std::array<u8, 32> digest{};
    mbedtls_sha256_finish(&m_context->sha, digest.data());
    mbedtls_sha256_starts(&m_context->sha, 0);
    return digest;
}

std::string Sha256::hex(std::span<const u8> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const u8 byte : bytes) {
        out += kDigits[byte >> 4];
        out += kDigits[byte & 15];
    }
    return out;
}

std::string sha256Hex(std::span<const u8> data) {
    Sha256 sha;
    sha.update(data);
    const std::array<u8, 32> digest = sha.finish();
    return Sha256::hex(digest);
}

Result<std::string> sha256File(const std::filesystem::path& path) {
    std::FILE* file = nullptr;
#if defined(_WIN32)
    _wfopen_s(&file, path.c_str(), L"rb");
#else
    file = std::fopen(path.c_str(), "rb");
#endif
    if (!file) {
        return Error{"Cannot open " + path.string()};
    }
    Sha256 sha;
    std::vector<u8> block(1u << 20);
    usize read = 0;
    while ((read = std::fread(block.data(), 1, block.size(), file)) > 0) {
        sha.update(std::span(block.data(), read));
    }
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed) {
        return Error{"Cannot read " + path.string()};
    }
    const std::array<u8, 32> digest = sha.finish();
    return Sha256::hex(digest);
}

} // namespace vd
