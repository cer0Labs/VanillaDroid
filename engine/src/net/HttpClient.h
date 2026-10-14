#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <atomic>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Phase 22 (pairing, ADR-0005 §5): the little HTTP/1.1 the app speaks with VanillaDroid Setup on the PC
// (tools/setup/PhoneServer.h): GET only, one request per connection ("Connection: close"), no TLS (the local
// network only: the pairing key authenticates the phone, the SHA-256 of each file protects the data). Bodies
// with a Content-Length, chunked or up to the connection's end; single byte ranges for resuming. The parsing
// is pure (tests); httpGet() blocks the calling thread (vd-download, never the game's).
namespace vd::net {

struct HttpResponseHead {
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers; // names in lowercase, values trimmed
    i64 contentLength = -1;                                   // -1: not given
    bool chunked = false;                                     // Transfer-Encoding: chunked

    std::string_view header(std::string_view lowercaseName) const; // empty when absent
};

// The status line and the headers, up to (not including) the blank line. Only HTTP/1.0 and HTTP/1.1.
Result<HttpResponseHead> parseResponseHead(std::string_view head);

// Content-Range: "bytes 100-199/1000" (first, last, total) or a 416's "bytes */1000" (unsatisfied, total).
struct ContentRange {
    u64 first = 0;
    u64 last = 0;
    u64 total = 0;
    bool unsatisfied = false;
};
bool parseContentRange(std::string_view value, ContentRange& range);

// A chunked body (RFC 9112 §7.1) decoded as it arrives, in pieces of any size: hex sizes with optional
// extensions, the data and its CRLF, the trailers after the last (empty) chunk.
class ChunkedDecoder {
public:
    // Each piece of the body in `in` goes to `out`. False for malformed input or when `out` returns false.
    bool feed(std::span<const u8> in, const std::function<bool(std::span<const u8>)>& out);
    bool done() const { return m_state == State::Done; }

private:
    enum class State : u8 {
        Size,
        Extension,
        SizeLf,
        Data,
        DataCr,
        DataLf,
        TrailerStart,
        Trailer,
        TrailerLf,
        EndLf,
        Done,
    };
    State m_state = State::Size;
    u64 m_left = 0;
    u32 m_digits = 0;
};

// Dotted IPv4 ("192.168.1.20" -> 0xC0A80114): four decimal numbers up to 255, nothing else.
bool parseIpv4(std::string_view text, u32& address);
std::string ipv4Text(u32 address);
// Where VanillaDroid Setup may be: 10/8, 172.16/12, 192.168/16, 169.254/16 (link-local), 100.64/10
// (carrier-grade NAT, some hotspots) and 127/8 (loopback: `adb reverse`, the desktop's tests), as the
// server's own check (tools/setup/HttpServer.h, isLocalNetworkAddress). Never a public address: a link from
// anywhere must not make the app download from the Internet.
bool isLocalNetworkAddress(u32 address);

struct HttpEndpoint {
    std::string host; // dotted IPv4, canonical (ipv4Text)
    u16 port = 0;

    bool valid() const { return !host.empty() && port != 0; }
    std::string text() const; // "192.168.1.20:47800"
};
// "192.168.1.20:47800" or "192.168.1.20" (then `defaultPort`), spaces around ignored: an endpoint on the
// local network; false for anything else (names, public addresses, port 0, garbage).
bool parseLocalEndpoint(std::string_view text, u16 defaultPort, HttpEndpoint& endpoint);

struct HttpGet {
    std::string target; // "/files.json", already percent-encoded
    std::vector<std::pair<std::string, std::string>>
        headers; // more of them ("X-VD-Key", "Range", "If-Range")
    u32 connectTimeoutMs = 5000;
    u32 idleTimeoutMs = 20000;                 // no byte for this long: the connection is given up
    const std::atomic<bool>* cancel = nullptr; // looked at every quarter of a second
};

struct HttpOutcome {
    enum class Kind : u8 {
        Complete,    // the whole response arrived
        Unreachable, // no connection (refused, timed out, no route) or the request could not be sent
        Cut,         // the connection ended or went quiet before the response's end
        Malformed,   // not an HTTP/1.x response
        Cancelled,
        Refused, // onHead or onBody returned false
    };
    Kind kind = Kind::Unreachable;
    HttpResponseHead head;
    bool hasHead = false;
    u64 bodyBytes = 0; // body bytes handed to onBody
    std::string error; // what happened, for the log
};

// The request's bytes (tests): the request line, Host, User-Agent, Connection: close, the extra headers.
std::string httpRequestText(const HttpEndpoint& at, const HttpGet& request);

// One GET on a connection of its own: the head goes to `onHead` (false stops before the body: a status the
// caller does not want), the body in pieces to `onBody` (false stops it).
HttpOutcome httpGet(const HttpEndpoint& at, const HttpGet& request,
                    const std::function<bool(const HttpResponseHead&)>& onHead,
                    const std::function<bool(std::span<const u8>)>& onBody);
// The same with the body kept in `body` (more than `maxBytes` is Refused).
HttpOutcome httpGetText(const HttpEndpoint& at, const HttpGet& request, usize maxBytes, std::string& body);

} // namespace vd::net
