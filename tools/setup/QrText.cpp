#include "QrText.h"

#include <qrcodegen.h>

namespace vd::setup {

bool QrMatrix::dark(int x, int y) const {
    return x >= 0 && y >= 0 && x < size && y < size && modules[static_cast<usize>(y * size + x)] != 0;
}

Result<QrMatrix> encodeQr(std::string_view text) {
    constexpr int kMaxVersion = 10; // 57 x 57: anything longer is not a URL to scan from a terminal
    const std::string terminated(text);
    if (terminated.find('\0') != std::string::npos) {
        return Error{"NUL in the QR text"};
    }
    std::vector<u8> temp(qrcodegen_BUFFER_LEN_FOR_VERSION(kMaxVersion));
    std::vector<u8> code(qrcodegen_BUFFER_LEN_FOR_VERSION(kMaxVersion));
    if (!qrcodegen_encodeText(terminated.c_str(), temp.data(), code.data(), qrcodegen_Ecc_LOW,
                              qrcodegen_VERSION_MIN, kMaxVersion, qrcodegen_Mask_AUTO, true)) {
        return Error{"text too long for a QR code"};
    }
    QrMatrix qr;
    qr.size = qrcodegen_getSize(code.data());
    qr.modules.resize(static_cast<usize>(qr.size * qr.size));
    for (int y = 0; y < qr.size; ++y) {
        for (int x = 0; x < qr.size; ++x) {
            qr.modules[static_cast<usize>(y * qr.size + x)] = qrcodegen_getModule(code.data(), x, y) ? 1 : 0;
        }
    }
    return qr;
}

std::string renderQrHalfBlocks(const QrMatrix& qr, int quiet, bool ansi) {
    // U+2580 upper half, U+2584 lower half, U+2588 full block, in UTF-8.
    static constexpr const char* kUpper = "\xE2\x96\x80";
    static constexpr const char* kLower = "\xE2\x96\x84";
    static constexpr const char* kFull = "\xE2\x96\x88";
    const int first = -quiet;
    const int last = qr.size + quiet; // exclusive
    std::string out;
    for (int y = first; y < last; y += 2) {
        if (ansi) {
            out += "\x1B[0m"; // the line's colours are set per character below
        }
        for (int x = first; x < last; ++x) {
            const bool top = qr.dark(x, y);
            const bool bottom = y + 1 < last && qr.dark(x, y + 1);
            if (ansi) {
                // The upper half in the foreground colour, the lower in the background: black (30/40), white
                // (97/107).
                out += "\x1B[";
                out += top ? "30" : "97";
                out += ';';
                out += (y + 1 < last && bottom) ? "40" : "107";
                out += 'm';
                out += kUpper;
            } else {
                // Light modules drawn (the terminal's text colour), dark ones left as the background.
                const bool topLight = !top;
                const bool bottomLight = y + 1 < last ? !bottom : false;
                out += topLight && bottomLight ? kFull : topLight ? kUpper : bottomLight ? kLower : " ";
            }
        }
        if (ansi) {
            out += "\x1B[0m";
        }
        out += '\n';
    }
    return out;
}

} // namespace vd::setup
