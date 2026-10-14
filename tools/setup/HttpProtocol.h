#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Phase 22 (ADR-0005 §5): the little HTTP/1.1 the local server speaks with the phone. Only GET and HEAD, no
// request bodies, persistent connections, single byte ranges ("bytes=N-", "bytes=N-M", "bytes=-N") for
// resuming. Pure code: the sockets are in HttpServer.
namespace vd::setup {

struct HttpRequest {
    std::string method;
    std::string target; // as sent: "/packs/models.vdpk?k=..."
    std::string path;   // percent-decoded, without the query: "/packs/models.vdpk"
    std::string query;  // without the '?', still encoded
    std::vector<std::pair<std::string, std::string>> headers; // names in lowercase
    bool keepAlive = true; // HTTP/1.1 unless "Connection: close"; HTTP/1.0 only with "Connection: keep-alive"

    std::string_view header(std::string_view name) const; // `name` in lowercase; empty when absent
};

// The request line and headers, up to (not including) the blank line.
Result<HttpRequest> parseRequestHead(std::string_view head);

// %XX decoding; false for a malformed escape or a NUL byte. `plusIsSpace` for query values.
bool percentDecode(std::string_view in, std::string& out, bool plusIsSpace = false);
// The decoded value of a query parameter ("k" in "k=abc&x=1"), empty when absent.
std::string queryValue(std::string_view query, std::string_view name);
// Percent-encodes everything but unreserved characters and '/' (for paths in links and files.json).
std::string percentEncodePath(std::string_view text);

struct ByteRange {
    u64 first = 0;
    u64 last = 0; // inclusive
    u64 length() const { return last - first + 1; }
};
enum class RangeStatus : u8 {
    None,          // no usable Range header (absent, not bytes, several ranges): the whole file
    Satisfiable,   // `range` holds it, clamped to the file
    Unsatisfiable, // 416
};
RangeStatus parseRange(std::string_view header, u64 size, ByteRange& range);

struct HttpResponse {
    int status = 200;
    std::string contentType;
    std::vector<std::pair<std::string, std::string>> headers; // extra ones
    std::string body;
    // A file body instead (when `file` is set): `length` bytes from `offset`.
    std::filesystem::path file;
    u64 offset = 0;
    u64 length = 0;
    // Told how many body bytes went out and whether all did (a file transfer's end, for the console).
    std::function<void(u64 sent, bool complete)> onSent;
    // Phase 22 (gui): told after each block of a file body, with the bytes sent so far (a phone's progress bar).
    std::function<void(u64 sent)> onProgress;
    bool close = false; // close the connection after it
};

std::string_view statusText(int status);
// The status line and headers, ending with the blank line. `head` true for HEAD: the same headers, no body.
std::string responseHead(const HttpResponse& response, u64 bodyLength, bool keepAlive);

std::string htmlEscape(std::string_view text);
// Two strings compared in a time that does not depend on where they differ (keys, codes).
bool constantTimeEquals(std::string_view a, std::string_view b);

} // namespace vd::setup
