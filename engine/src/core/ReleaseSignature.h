#pragma once

#include "core/Types.h"

#include <span>
#include <string_view>

// Phase 22 (distribution, ADR-0005 §6): the signature of a release's manifest.json. The release workflow
// signs the exact bytes of the manifest with our ECDSA P-256 key (`openssl dgst -sha256 -sign`) and publishes
// the DER signature, in standard base64 on one line, as manifest.json.sig. The public half is built into
// every program from release/release-key.pub.hex (the uncompressed point 04 || X || Y); the private half
// never is in the repository. Nothing from a release is installed unless its manifest verifies here AND each
// file's SHA-256 is the manifest's.
namespace vd {

inline constexpr usize kReleaseKeySize = 65; // 0x04, X, Y (32 bytes each)

// The release public key built into this program.
std::span<const u8, kReleaseKeySize> releasePublicKey();

// True when `signatureBase64` (whitespace around it is ignored) is a valid signature of `manifest` by the
// built-in key; false for anything else (a damaged or truncated signature, other bytes, another key).
bool verifyRelease(std::span<const u8> manifest, std::string_view signatureBase64);
// The same against another public key (tests, tools): an uncompressed P-256 point; any other key never
// verifies.
bool verifyRelease(std::span<const u8> manifest, std::string_view signatureBase64,
                   std::span<const u8> publicKey);
// The manifest as text read from a file or a download (its bytes as they are).
inline bool verifyRelease(std::string_view manifest, std::string_view signatureBase64) {
    return verifyRelease(std::span(reinterpret_cast<const u8*>(manifest.data()), manifest.size()),
                         signatureBase64);
}

} // namespace vd
