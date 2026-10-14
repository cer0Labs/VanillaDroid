#include "net/HttpClient.h"

#include "core/Version.h"
#include "net/Socket.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace vd::net {
namespace {

constexpr usize kMaxHeadBytes = 16 * 1024;
constexpr usize kReceiveBlock = 64 * 1024;
constexpr u32 kPollMs = 250;

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// A decimal number that fits in 64 bits.
bool parseU64(std::string_view text, u64& value) {
    if (text.empty() || text.size() > 19) {
        return false;
    }
    value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<u64>(c - '0');
    }
    return true;
}

int hexDigit(u8 c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool cancelled(const HttpGet& request) {
    return request.cancel && request.cancel->load(std::memory_order_relaxed);
}

enum class Wait : u8 { Ready, Cancelled, Quiet };

// Waits for bytes (or the end of the stream), looking at the cancel flag every quarter of a second.
Wait waitForData(Socket& socket, const HttpGet& request) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        if (cancelled(request)) {
            return Wait::Cancelled;
        }
        if (socket.waitReadable(kPollMs)) {
            return Wait::Ready;
        }
        const auto waited =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        if (waited.count() >= static_cast<i64>(request.idleTimeoutMs)) {
            return Wait::Quiet;
        }
    }
}

} // namespace

std::string_view HttpResponseHead::header(std::string_view lowercaseName) const {
    for (const auto& [name, value] : headers) {
        if (name == lowercaseName) {
            return value;
        }
    }
    return {};
}

Result<HttpResponseHead> parseResponseHead(std::string_view head) {
    HttpResponseHead response;
    usize lineEnd = head.find('\n');
    const std::string_view statusLine = trim(head.substr(0, lineEnd));
    // "HTTP/1.1 206 Partial Content": the version, the code, any reason.
    if (statusLine.substr(0, 7) != "HTTP/1." || statusLine.size() < 12 || statusLine[8] != ' ' ||
        (statusLine[7] != '0' && statusLine[7] != '1')) {
        return Error{"not an HTTP/1.x response"};
    }
    u64 status = 0;
    const std::string_view code = statusLine.substr(9, 3);
    if (!parseU64(code, status) || status < 100 || status > 599 ||
        (statusLine.size() > 12 && statusLine[12] != ' ')) {
        return Error{"bad status line"};
    }
    response.status = static_cast<int>(status);
    while (lineEnd != std::string_view::npos) {
        const usize begin = lineEnd + 1;
        lineEnd = head.find('\n', begin);
        const std::string_view line =
            trim(head.substr(begin, lineEnd == std::string_view::npos ? head.npos : lineEnd - begin));
        if (line.empty()) {
            continue;
        }
        const usize colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            return Error{"bad header"};
        }
        response.headers.emplace_back(lower(trim(line.substr(0, colon))),
                                      std::string(trim(line.substr(colon + 1))));
    }
    if (const std::string_view length = response.header("content-length"); !length.empty()) {
        u64 value = 0;
        if (!parseU64(length, value) || value > static_cast<u64>(INT64_MAX)) {
            return Error{"bad Content-Length"};
        }
        response.contentLength = static_cast<i64>(value);
    }
    const std::string encoding = lower(response.header("transfer-encoding"));
    if (!encoding.empty()) {
        // Only "chunked" (the last coding) is understood: any other one could not be read.
        const usize comma = encoding.rfind(',');
        if (trim(std::string_view(encoding).substr(comma == std::string::npos ? 0 : comma + 1)) !=
            "chunked") {
            return Error{"unsupported Transfer-Encoding"};
        }
        response.chunked = true;
        response.contentLength = -1; // RFC 9112 §6.3: the chunks rule
    }
    return response;
}

bool parseContentRange(std::string_view value, ContentRange& range) {
    value = trim(value);
    if (value.substr(0, 6) != "bytes ") {
        return false;
    }
    value = trim(value.substr(6));
    const usize slash = value.find('/');
    if (slash == std::string_view::npos) {
        return false;
    }
    range = {};
    if (!parseU64(trim(value.substr(slash + 1)), range.total)) {
        return false; // "*" totals are of no use to resume a file of known size
    }
    const std::string_view span = trim(value.substr(0, slash));
    if (span == "*") {
        range.unsatisfied = true;
        return true;
    }
    const usize dash = span.find('-');
    if (dash == std::string_view::npos || !parseU64(span.substr(0, dash), range.first) ||
        !parseU64(span.substr(dash + 1), range.last)) {
        return false;
    }
    return range.first <= range.last && range.last < range.total;
}

bool ChunkedDecoder::feed(std::span<const u8> in, const std::function<bool(std::span<const u8>)>& out) {
    usize i = 0;
    while (i < in.size()) {
        const u8 c = in[i];
        switch (m_state) {
        case State::Size: {
            const int digit = hexDigit(c);
            if (digit >= 0) {
                if (m_digits >= 15) {
                    return false; // a chunk past 2^60 bytes: nonsense
                }
                m_left = m_left * 16 + static_cast<u64>(digit);
                ++m_digits;
            } else if (m_digits > 0 && (c == ';' || c == ' ' || c == '\t')) {
                m_state = State::Extension;
            } else if (m_digits > 0 && c == '\r') {
                m_state = State::SizeLf;
            } else {
                return false;
            }
            ++i;
            break;
        }
        case State::Extension: // ignored up to the line's end
            if (c == '\r') {
                m_state = State::SizeLf;
            }
            ++i;
            break;
        case State::SizeLf:
            if (c != '\n') {
                return false;
            }
            m_state = m_left == 0 ? State::TrailerStart : State::Data;
            ++i;
            break;
        case State::Data: {
            const usize take = static_cast<usize>(std::min<u64>(m_left, in.size() - i));
            if (!out(in.subspan(i, take))) {
                return false;
            }
            m_left -= take;
            i += take;
            if (m_left == 0) {
                m_state = State::DataCr;
            }
            break;
        }
        case State::DataCr:
            if (c != '\r') {
                return false;
            }
            m_state = State::DataLf;
            ++i;
            break;
        case State::DataLf:
            if (c != '\n') {
                return false;
            }
            m_state = State::Size;
            m_digits = 0;
            ++i;
            break;
        case State::TrailerStart: // an empty line ends the body, anything else is a trailer field
            m_state = c == '\r' ? State::EndLf : State::Trailer;
            ++i;
            break;
        case State::Trailer:
            if (c == '\r') {
                m_state = State::TrailerLf;
            }
            ++i;
            break;
        case State::TrailerLf:
            if (c != '\n') {
                return false;
            }
            m_state = State::TrailerStart;
            ++i;
            break;
        case State::EndLf:
            if (c != '\n') {
                return false;
            }
            m_state = State::Done;
            ++i;
            break;
        case State::Done: return true; // whatever follows is not this body's
        }
    }
    return true;
}

bool parseIpv4(std::string_view text, u32& address) {
    u32 parts[4] = {};
    usize part = 0;
    usize digits = 0;
    for (const char c : text) {
        if (c >= '0' && c <= '9') {
            if (++digits > 3) {
                return false;
            }
            parts[part] = parts[part] * 10 + static_cast<u32>(c - '0');
            if (parts[part] > 255) {
                return false;
            }
        } else if (c == '.' && digits > 0 && part < 3) {
            ++part;
            digits = 0;
        } else {
            return false;
        }
    }
    if (part != 3 || digits == 0) {
        return false;
    }
    address = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

std::string ipv4Text(u32 address) {
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", (address >> 24) & 255, (address >> 16) & 255,
                  (address >> 8) & 255, address & 255);
    return text;
}

bool isLocalNetworkAddress(u32 address) {
    const u32 a = address >> 24;
    const u32 b = (address >> 16) & 255;
    return a == 127 || a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
           (a == 169 && b == 254) || (a == 100 && b >= 64 && b <= 127);
}

std::string HttpEndpoint::text() const {
    return host + ":" + std::to_string(port);
}

bool parseLocalEndpoint(std::string_view text, u16 defaultPort, HttpEndpoint& endpoint) {
    text = trim(text);
    const usize colon = text.find(':');
    u32 address = 0;
    if (!parseIpv4(text.substr(0, colon), address) || !isLocalNetworkAddress(address) ||
        (address & 255) == 0 || (address & 255) == 255) {
        return false; // the network's own address or its broadcast: never a PC
    }
    u64 port = defaultPort;
    if (colon != std::string_view::npos && (!parseU64(text.substr(colon + 1), port) || port > 65535)) {
        return false;
    }
    if (port == 0) {
        return false;
    }
    endpoint.host = ipv4Text(address);
    endpoint.port = static_cast<u16>(port);
    return true;
}

std::string httpRequestText(const HttpEndpoint& at, const HttpGet& request) {
    std::string text = "GET " + request.target + " HTTP/1.1\r\n";
    text += "Host: " + at.text() + "\r\n";
    text += std::string("User-Agent: VanillaDroid/") + kVersion + "\r\n";
    text += "Accept-Encoding: identity\r\n";
    text += "Connection: close\r\n";
    for (const auto& [name, value] : request.headers) {
        text += name + ": " + value + "\r\n";
    }
    text += "\r\n";
    return text;
}

HttpOutcome httpGet(const HttpEndpoint& at, const HttpGet& request,
                    const std::function<bool(const HttpResponseHead&)>& onHead,
                    const std::function<bool(std::span<const u8>)>& onBody) {
    HttpOutcome outcome;
    if (cancelled(request)) {
        outcome.kind = HttpOutcome::Kind::Cancelled;
        return outcome;
    }
    Socket socket;
    if (Result<void> connected = socket.connect(at.host, at.port, request.connectTimeoutMs); !connected) {
        outcome.kind = HttpOutcome::Kind::Unreachable;
        outcome.error = connected.error().message;
        return outcome;
    }
    const std::string text = httpRequestText(at, request);
    if (Result<void> sent = socket.sendAll(std::span(reinterpret_cast<const u8*>(text.data()), text.size()));
        !sent) {
        outcome.kind = HttpOutcome::Kind::Unreachable;
        outcome.error = sent.error().message;
        return outcome;
    }

    std::vector<u8> block(kReceiveBlock);
    std::string headText;
    usize headEnd = std::string::npos;
    usize bodyStart = 0; // in the block, after the head
    usize received = 0;
    // The head: up to the blank line.
    while (headEnd == std::string::npos) {
        const Wait wait = waitForData(socket, request);
        if (wait != Wait::Ready) {
            outcome.kind = wait == Wait::Cancelled ? HttpOutcome::Kind::Cancelled : HttpOutcome::Kind::Cut;
            outcome.error = "no answer";
            return outcome;
        }
        Result<usize> got = socket.receive(block.data(), block.size());
        if (!got || got.value() == 0) {
            outcome.kind = HttpOutcome::Kind::Cut;
            outcome.error = got ? std::string("closed before the answer") : got.error().message;
            return outcome;
        }
        const usize before = headText.size();
        headText.append(reinterpret_cast<const char*>(block.data()), got.value());
        headEnd = headText.find("\r\n\r\n");
        if (headEnd == std::string::npos && headText.size() > kMaxHeadBytes) {
            outcome.kind = HttpOutcome::Kind::Malformed;
            outcome.error = "head too long";
            return outcome;
        }
        if (headEnd != std::string::npos) {
            bodyStart = headEnd + 4 - before; // where the body begins in this block
            received = got.value();
        }
    }
    Result<HttpResponseHead> parsed = parseResponseHead(std::string_view(headText).substr(0, headEnd));
    if (!parsed) {
        outcome.kind = HttpOutcome::Kind::Malformed;
        outcome.error = parsed.error().message;
        return outcome;
    }
    outcome.head = std::move(parsed.value());
    outcome.hasHead = true;
    if (!onHead(outcome.head)) {
        outcome.kind = HttpOutcome::Kind::Refused;
        return outcome;
    }

    // The body.
    const HttpResponseHead& head = outcome.head;
    ChunkedDecoder chunks;
    bool refused = false;
    u64 left = head.contentLength >= 0 ? static_cast<u64>(head.contentLength) : 0;
    const auto deliver = [&](std::span<const u8> piece) {
        if (piece.empty()) {
            return true;
        }
        outcome.bodyBytes += piece.size();
        if (!onBody(piece)) {
            refused = true;
            return false;
        }
        return true;
    };
    // Some bytes; true while more of the body is wanted.
    const auto take = [&](std::span<const u8> bytes) -> bool {
        if (head.chunked) {
            if (!chunks.feed(bytes, deliver)) {
                if (!refused) {
                    outcome.kind = HttpOutcome::Kind::Malformed;
                    outcome.error = "bad chunked body";
                }
                return false;
            }
            return !chunks.done();
        }
        if (head.contentLength >= 0) {
            const usize used = static_cast<usize>(std::min<u64>(left, bytes.size()));
            left -= used;
            return deliver(bytes.subspan(0, used)) && left > 0;
        }
        return deliver(bytes); // up to the end of the connection
    };
    const bool noBody = head.status == 204 || head.status == 304 || head.status < 200 ||
                        (!head.chunked && head.contentLength == 0);
    bool more = !noBody;
    if (more && received > bodyStart) {
        more = take(std::span<const u8>(block.data() + bodyStart, received - bodyStart));
    }
    while (more) {
        const Wait wait = waitForData(socket, request);
        if (wait != Wait::Ready) {
            outcome.kind = wait == Wait::Cancelled ? HttpOutcome::Kind::Cancelled : HttpOutcome::Kind::Cut;
            outcome.error = "the PC went quiet";
            return outcome;
        }
        Result<usize> got = socket.receive(block.data(), block.size());
        if (!got) {
            outcome.kind = HttpOutcome::Kind::Cut;
            outcome.error = got.error().message;
            return outcome;
        }
        if (got.value() == 0) {
            // The end of the connection: the end of a body without a length, else it was cut.
            if (!head.chunked && head.contentLength < 0) {
                break;
            }
            outcome.kind = HttpOutcome::Kind::Cut;
            outcome.error = "connection closed";
            return outcome;
        }
        more = take(std::span<const u8>(block.data(), got.value()));
    }
    if (refused) {
        outcome.kind = HttpOutcome::Kind::Refused;
        return outcome;
    }
    if (outcome.kind == HttpOutcome::Kind::Malformed) {
        return outcome;
    }
    outcome.kind = HttpOutcome::Kind::Complete;
    return outcome;
}

HttpOutcome httpGetText(const HttpEndpoint& at, const HttpGet& request, usize maxBytes, std::string& body) {
    body.clear();
    return httpGet(
        at, request, [](const HttpResponseHead&) { return true; },
        [&](std::span<const u8> piece) {
            if (body.size() + piece.size() > maxBytes) {
                return false;
            }
            body.append(reinterpret_cast<const char*>(piece.data()), piece.size());
            return true;
        });
}

} // namespace vd::net
