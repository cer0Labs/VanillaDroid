#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <string>
#include <string_view>
#include <vector>

// Phase 22 (ADR-0005 §5): the QR code the phone's camera scans, drawn in the terminal (qrcodegen, MIT). Two
// rows of modules per line of text with the Unicode half blocks; with ANSI colours black on white whatever
// the terminal's theme, without them light modules drawn as blocks (a dark terminal's usual colours).
namespace vd::setup {

struct QrMatrix {
    int size = 0;            // modules per side
    std::vector<u8> modules; // size * size, 1 = dark, row by row

    bool dark(int x, int y) const;
};

// Low error correction (raised when it costs no size): the URL fits a small code a camera reads from a
// screen.
Result<QrMatrix> encodeQr(std::string_view text);

// `quiet`: the light border in modules (the standard asks for 4). `ansi`: colour escapes (VT terminals).
std::string renderQrHalfBlocks(const QrMatrix& qr, int quiet, bool ansi);

} // namespace vd::setup
