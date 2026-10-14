// Phase 22 (release): the release manifest's signature (core/ReleaseSignature) against signatures made by
// openssl with the test key (tests/data/release-test-key.pem), and the manifest of
// scripts/release-manifest.ps1.
#include "core/ReleaseSignature.h"

#include "core/Json.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <ostream> // doctest prints std::string values through operator<<
#include <string>
#include <vector>

namespace {

using vd::u8;

std::filesystem::path dataDir() {
    return std::filesystem::path(VD_TEST_DATA_DIR);
}

std::string readFile(const std::filesystem::path& path) {
    std::string bytes;
    std::FILE* file = nullptr;
#if defined(_WIN32)
    _wfopen_s(&file, path.c_str(), L"rb");
#else
    file = std::fopen(path.c_str(), "rb");
#endif
    if (!file) {
        return bytes;
    }
    char block[4096];
    std::size_t read = 0;
    while ((read = std::fread(block, 1, sizeof(block), file)) > 0) {
        bytes.append(block, read);
    }
    std::fclose(file);
    return bytes;
}

std::vector<u8> hexBytes(std::string_view hex) {
    std::vector<u8> bytes;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        bytes.push_back(static_cast<u8>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    }
    return bytes;
}

std::span<const u8> asBytes(const std::string& text) {
    return std::span(reinterpret_cast<const u8*>(text.data()), text.size());
}

std::vector<u8> testKey() {
    std::string hex = readFile(dataDir() / "release-test-key.pub.hex");
    while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r')) {
        hex.pop_back();
    }
    return hexBytes(hex);
}

// Another P-256 key (openssl genpkey), whose private half was thrown away.
constexpr std::string_view kOtherKey =
    "04a8326d2aadb573bc4498e816550edc0a4e5a37c575042312255e6b96f98a5698799184056ade43f9"
    "4b43864ba100c67ff805c5f8525bcf434fdfcff2c66368bd";

const std::string kVectorBase64 =
    "MEQCIGT08uotrTqCh6BnyMivJvX/dQsQt5TivwmzQLdd5KJtAiBmu30KFM7lV4QwdPADeTtAMnavbyw/21v"
    "wIGKYIy5gXQ==";

// Base64 of DER bytes (the tests build damaged signatures).
std::string base64(const std::vector<u8>& bytes) {
    static constexpr char kDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::size_t count = std::min<std::size_t>(3, bytes.size() - i);
        vd::u32 group = static_cast<vd::u32>(bytes[i]) << 16;
        if (count > 1) {
            group |= static_cast<vd::u32>(bytes[i + 1]) << 8;
        }
        if (count > 2) {
            group |= bytes[i + 2];
        }
        out += kDigits[group >> 18 & 63];
        out += kDigits[group >> 12 & 63];
        out += count > 1 ? kDigits[group >> 6 & 63] : '=';
        out += count > 2 ? kDigits[group & 63] : '=';
    }
    return out;
}

// The DER of release-test-vector.txt.sig: SEQUENCE { INTEGER r (32 bytes), INTEGER s (32 bytes) }.
const std::vector<u8> kVectorDer =
    hexBytes("30440220"
             "64f4f2ea2dad3a8287a067c8c8af26f5ff750b10b794e2bf09b340b75de4a26d"
             "0220"
             "66bb7d0a14cee557843074f003793b403276af6f2c3fdb5bf0206298232e605d");

} // namespace

TEST_CASE("Release signature: openssl's signature of the test vector verifies with the test key") {
    const std::string vector = readFile(dataDir() / "release-test-vector.txt");
    const std::string signature = readFile(dataDir() / "release-test-vector.txt.sig");
    const std::vector<u8> key = testKey();
    REQUIRE(!vector.empty());
    REQUIRE(key.size() == vd::kReleaseKeySize);
    CHECK(signature == kVectorBase64);
    CHECK(base64(kVectorDer) == kVectorBase64);
    CHECK(vd::verifyRelease(asBytes(vector), signature, key));
    // Whitespace around the base64 (a .sig saved with a line end) is ignored.
    CHECK(vd::verifyRelease(asBytes(vector), signature + "\n", key));
    CHECK(vd::verifyRelease(asBytes(vector), " \r\n" + signature + "\r\n", key));
}

TEST_CASE("Release signature: other bytes do not verify") {
    const std::string vector = readFile(dataDir() / "release-test-vector.txt");
    const std::vector<u8> key = testKey();
    REQUIRE(!vector.empty());
    std::string changed = vector;
    changed[10] ^= 0x01;
    CHECK_FALSE(vd::verifyRelease(asBytes(changed), kVectorBase64, key));
    CHECK_FALSE(vd::verifyRelease(asBytes(vector + "\n"), kVectorBase64, key));
    CHECK_FALSE(vd::verifyRelease(asBytes(vector.substr(0, vector.size() - 1)), kVectorBase64, key));
    std::string crlf; // the same text with Windows line ends: other bytes
    for (const char c : vector) {
        if (c == '\n') {
            crlf += '\r';
        }
        crlf += c;
    }
    CHECK_FALSE(vd::verifyRelease(asBytes(crlf), kVectorBase64, key));
    CHECK_FALSE(vd::verifyRelease(std::span<const u8>(), kVectorBase64, key));
}

TEST_CASE("Release signature: damaged signatures do not verify") {
    const std::string vector = readFile(dataDir() / "release-test-vector.txt");
    const std::vector<u8> key = testKey();
    REQUIRE(!vector.empty());
    const auto verifies = [&](const std::string& signature) {
        return vd::verifyRelease(asBytes(vector), signature, key);
    };
    REQUIRE(verifies(base64(kVectorDer)));

    std::vector<u8> der = kVectorDer;
    der[10] ^= 0x40; // r
    CHECK_FALSE(verifies(base64(der)));
    der = kVectorDer;
    der[der.size() - 1] ^= 0x01; // s
    CHECK_FALSE(verifies(base64(der)));
    der = kVectorDer;
    der.push_back(0x00); // bytes after the sequence
    CHECK_FALSE(verifies(base64(der)));
    der = kVectorDer;
    der.pop_back(); // truncated
    CHECK_FALSE(verifies(base64(der)));
    der = kVectorDer;
    der[0] = 0x31; // a SET, not a SEQUENCE
    CHECK_FALSE(verifies(base64(der)));
    // r and s swapped.
    der = {0x30, 0x44, 0x02, 0x20};
    der.insert(der.end(), kVectorDer.begin() + 38, kVectorDer.end());
    der.insert(der.end(), {0x02, 0x20});
    der.insert(der.end(), kVectorDer.begin() + 4, kVectorDer.begin() + 36);
    REQUIRE(der.size() == kVectorDer.size());
    CHECK_FALSE(verifies(base64(der)));

    CHECK_FALSE(verifies(""));
    CHECK_FALSE(verifies("   \n"));
    CHECK_FALSE(verifies(kVectorBase64.substr(0, kVectorBase64.size() - 4))); // a whole group short
    CHECK_FALSE(verifies(kVectorBase64.substr(0, kVectorBase64.size() - 1))); // not a multiple of 4
    CHECK_FALSE(
        verifies(kVectorBase64.substr(0, 40) + "\n" + kVectorBase64.substr(40))); // split in two lines
    CHECK_FALSE(verifies(std::string(kVectorBase64).replace(5, 1, "-")));         // base64url, not standard
    CHECK_FALSE(verifies(std::string(kVectorBase64).replace(5, 1, "=")));
    CHECK_FALSE(verifies(std::string(200, 'A')));
    // The raw DER (not base64) is not a signature either.
    CHECK_FALSE(verifies(std::string(kVectorDer.begin(), kVectorDer.end())));
}

TEST_CASE("Release signature: only the signing key verifies") {
    const std::string vector = readFile(dataDir() / "release-test-vector.txt");
    REQUIRE(!vector.empty());
    const std::vector<u8> other = hexBytes(kOtherKey);
    REQUIRE(other.size() == vd::kReleaseKeySize);
    CHECK_FALSE(vd::verifyRelease(asBytes(vector), kVectorBase64, other));

    std::vector<u8> key = testKey();
    // The compressed form of the same point (02/03 || X) is not accepted: only 04 || X || Y.
    std::vector<u8> compressed(key.begin(), key.begin() + 33);
    compressed[0] = (key[64] & 1) != 0 ? 0x03 : 0x02;
    CHECK_FALSE(vd::verifyRelease(asBytes(vector), kVectorBase64, compressed));
    // A point off the curve, a short key, no key.
    std::vector<u8> offCurve = key;
    offCurve[64] ^= 0x01;
    CHECK_FALSE(vd::verifyRelease(asBytes(vector), kVectorBase64, offCurve));
    CHECK_FALSE(vd::verifyRelease(asBytes(vector), kVectorBase64, std::span(key.data(), 64)));
    CHECK_FALSE(vd::verifyRelease(asBytes(vector), kVectorBase64, std::span<const u8>()));
    std::vector<u8> zeros(vd::kReleaseKeySize, 0);
    zeros[0] = 0x04;
    CHECK_FALSE(vd::verifyRelease(asBytes(vector), kVectorBase64, zeros));
}

TEST_CASE("Release signature: the built-in key (release/release-key.pub.hex)") {
    const std::span<const u8, vd::kReleaseKeySize> key = vd::releasePublicKey();
    CHECK(key[0] == 0x04);
    const std::string vector = readFile(dataDir() / "release-test-vector.txt");
    REQUIRE(!vector.empty());
    const std::vector<u8> test = testKey();
    const bool isTestKey = std::equal(key.begin(), key.end(), test.begin(), test.end());
    // Until scripts/release-key.ps1 makes the real pair, the built-in key is the test key (and releases
    // refuse it).
    CHECK(vd::verifyRelease(asBytes(vector), kVectorBase64) == isTestKey);
    CHECK(vd::verifyRelease(std::string_view(vector), kVectorBase64) == isTestKey);
    CHECK_FALSE(vd::verifyRelease(asBytes(vector + " "), kVectorBase64));
    // A valid point: a signature by another key is rejected by the math, not by a malformed key.
    std::array<u8, vd::kReleaseKeySize> copy{};
    std::copy(key.begin(), key.end(), copy.begin());
    CHECK(vd::verifyRelease(asBytes(vector), kVectorBase64, copy) == isTestKey);
}

// scripts/release-manifest.ps1's output (fake files, signed with the test key): what the release workflow
// publishes.
TEST_CASE("Release signature: the manifest written and signed by scripts/release-manifest.ps1") {
    const std::string manifest = readFile(dataDir() / "release-test-manifest.json");
    const std::string signature = readFile(dataDir() / "release-test-manifest.json.sig");
    const std::vector<u8> key = testKey();
    REQUIRE(!manifest.empty());
    REQUIRE(!signature.empty());
    CHECK(signature.find('\n') == std::string::npos); // one line, no line end
    CHECK(vd::verifyRelease(asBytes(manifest), signature, key));

    // The release contract (format 1), read with core/Json as the Setup and the app read it.
    const vd::Result<vd::json::Value> parsed = vd::json::parse(manifest);
    REQUIRE(parsed.ok());
    const vd::json::Value& root = parsed.value();
    CHECK(root.get("format").asInt() == 1);
    CHECK(root.get("version").asString() == "0.2.0");
    CHECK(root.get("versionCode").asInt() == 2000);
    CHECK(root.get("date").asString() == "2026-10-20");
    CHECK(root.get("notes").asString() == "https://github.com/cer0Labs/VanillaDroid/releases/tag/v0.2.0");
    const vd::json::Value& apk = root.get("apk");
    CHECK(apk.get("url").asString() ==
          "https://github.com/cer0Labs/VanillaDroid/releases/download/v0.2.0/VanillaDroid-0.2.0.apk");
    CHECK(apk.get("size").asInt() == 300000);
    CHECK(apk.get("sha256").asString() == "e4b57a66b275e0c48a154bac263ec49265508bb4eeb0ac2671e9b1dd94f268be");
    CHECK(root.get("setup").get("windows").get("url").asString() ==
          "https://github.com/cer0Labs/VanillaDroid/releases/download/v0.2.0/"
          "VanillaDroidSetup-0.2.0-windows.zip");
    CHECK(root.get("setup").get("macos").get("size").asInt() == 250000);
    const vd::json::Value& packs = root.get("packs");
    REQUIRE(packs.size() == 3);
    CHECK(packs.members()[0].key == "actors.vdpk");
    CHECK(packs.get("world_azeroth.vdpk").asString() == "89abcdef01234567");

    // Any change to the published bytes breaks the signature, even one that parses the same.
    std::string bigger = manifest;
    bigger.replace(bigger.find("300000"), 6, "300001");
    CHECK_FALSE(vd::verifyRelease(asBytes(bigger), signature, key));
    std::string reindented = manifest;
    reindented.replace(reindented.find("\"format\": 1"), 11, "\"format\":1");
    CHECK(vd::json::parse(reindented).ok());
    CHECK_FALSE(vd::verifyRelease(asBytes(reindented), signature, key));
}
