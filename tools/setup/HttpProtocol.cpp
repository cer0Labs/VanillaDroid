#include "HttpProtocol.h"

#include "SetupCommon.h"

#include <cstdlib>

namespace vd::setup {
namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

int hexValue(char c) {
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

// A decimal number that fits in 64 bits; false otherwise.
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

} // namespace

std::string_view HttpRequest::header(std::string_view name) const {
    for (const auto& [key, value] : headers) {
        if (key == name) {
            return value;
        }
    }
    return {};
}

Result<HttpRequest> parseRequestHead(std::string_view head) {
    HttpRequest request;
    usize lineEnd = head.find('\n');
    const std::string_view requestLine = trim(head.substr(0, lineEnd));
    const usize firstSpace = requestLine.find(' ');
    const usize lastSpace = requestLine.rfind(' ');
    if (firstSpace == std::string_view::npos || lastSpace == firstSpace) {
        return Error{"bad request line"};
    }
    request.method = std::string(requestLine.substr(0, firstSpace));
    request.target = std::string(trim(requestLine.substr(firstSpace + 1, lastSpace - firstSpace - 1)));
    const std::string_view version = requestLine.substr(lastSpace + 1);
    if (version != "HTTP/1.1" && version != "HTTP/1.0") {
        return Error{"unsupported HTTP version"};
    }
    if (request.target.empty() || request.target.front() != '/') {
        return Error{"bad target"};
    }
    const usize question = request.target.find('?');
    const std::string_view rawPath = std::string_view(request.target).substr(0, question);
    if (question != std::string::npos) {
        request.query = request.target.substr(question + 1);
    }
    if (!percentDecode(rawPath, request.path)) {
        return Error{"bad path"};
    }
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
        request.headers.emplace_back(toLowerAscii(trim(line.substr(0, colon))),
                                     std::string(trim(line.substr(colon + 1))));
    }
    const std::string connection = toLowerAscii(request.header("connection"));
    request.keepAlive = version == "HTTP/1.1" ? connection.find("close") == std::string::npos
                                              : connection.find("keep-alive") != std::string::npos;
    return request;
}

bool percentDecode(std::string_view in, std::string& out, bool plusIsSpace) {
    out.clear();
    for (usize i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '%') {
            if (i + 2 >= in.size()) {
                return false;
            }
            const int high = hexValue(in[i + 1]);
            const int low = hexValue(in[i + 2]);
            if (high < 0 || low < 0) {
                return false;
            }
            c = static_cast<char>(high * 16 + low);
            i += 2;
        } else if (c == '+' && plusIsSpace) {
            c = ' ';
        }
        if (c == '\0') {
            return false;
        }
        out.push_back(c);
    }
    return true;
}

std::string queryValue(std::string_view query, std::string_view name) {
    while (!query.empty()) {
        const usize amp = query.find('&');
        const std::string_view pair = query.substr(0, amp);
        const usize equals = pair.find('=');
        std::string key;
        if (percentDecode(pair.substr(0, equals), key, true) && key == name) {
            std::string value;
            if (equals != std::string_view::npos && percentDecode(pair.substr(equals + 1), value, true)) {
                return value;
            }
            return {};
        }
        if (amp == std::string_view::npos) {
            break;
        }
        query.remove_prefix(amp + 1);
    }
    return {};
}

std::string percentEncodePath(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '-' ||
            u == '_' || u == '.' || u == '~' || u == '/') {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(kHex[u >> 4]);
            out.push_back(kHex[u & 15]);
        }
    }
    return out;
}

RangeStatus parseRange(std::string_view header, u64 size, ByteRange& range) {
    header = trim(header);
    if (header.empty()) {
        return RangeStatus::None;
    }
    if (header.substr(0, 6) != "bytes=") {
        return RangeStatus::None; // another unit: ignored (RFC 9110 §14.2)
    }
    const std::string_view spec = trim(header.substr(6));
    if (spec.find(',') != std::string_view::npos) {
        return RangeStatus::None; // several ranges: the whole file instead
    }
    const usize dash = spec.find('-');
    if (dash == std::string_view::npos) {
        return RangeStatus::None;
    }
    const std::string_view first = trim(spec.substr(0, dash));
    const std::string_view last = trim(spec.substr(dash + 1));
    if (first.empty()) { // the last N bytes
        u64 suffix = 0;
        if (!parseU64(last, suffix)) {
            return RangeStatus::None;
        }
        if (suffix == 0 || size == 0) {
            return RangeStatus::Unsatisfiable;
        }
        range.first = suffix >= size ? 0 : size - suffix;
        range.last = size - 1;
        return RangeStatus::Satisfiable;
    }
    u64 begin = 0;
    if (!parseU64(first, begin)) {
        return RangeStatus::None;
    }
    u64 end = size == 0 ? 0 : size - 1;
    if (!last.empty()) {
        if (!parseU64(last, end) || end < begin) {
            return RangeStatus::None; // invalid: ignored
        }
        if (size > 0 && end > size - 1) {
            end = size - 1;
        }
    }
    if (begin >= size) {
        return RangeStatus::Unsatisfiable;
    }
    range.first = begin;
    range.last = end;
    return RangeStatus::Satisfiable;
}

std::string_view statusText(int status) {
    switch (status) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 416: return "Range Not Satisfiable";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Unknown";
    }
}

std::string responseHead(const HttpResponse& response, u64 bodyLength, bool keepAlive) {
    std::string head = "HTTP/1.1 " + std::to_string(response.status) + " " +
                       std::string(statusText(response.status)) + "\r\n";
    if (!response.contentType.empty()) {
        head += "Content-Type: " + response.contentType + "\r\n";
    }
    head += "Content-Length: " + std::to_string(bodyLength) + "\r\n";
    head += "Cache-Control: no-store\r\n";
    head += keepAlive && !response.close ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    for (const auto& [name, value] : response.headers) {
        head += name + ": " + value + "\r\n";
    }
    head += "\r\n";
    return head;
}

std::string htmlEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out.push_back(c);
        }
    }
    return out;
}

bool constantTimeEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (usize i = 0; i < a.size(); ++i) {
        difference |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return difference == 0;
}

} // namespace vd::setup
