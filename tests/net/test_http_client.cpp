// Phase 22 (pairing): the app's little HTTP/1.1 client (net/HttpClient): response heads, byte ranges, chunked
// bodies, local network addresses. The transfers over sockets are tested with VanillaDroid Setup's server
// (tests/setup/test_pairing_download.cpp).
#include "net/HttpClient.h"

#include <doctest/doctest.h>

#include <ostream> // doctest prints std::string values through operator<<
#include <string>
#include <vector>

using namespace vd;
using namespace vd::net;

namespace {

std::span<const u8> bytesOf(std::string_view text) {
    return {reinterpret_cast<const u8*>(text.data()), text.size()};
}

// Feeds `body` to a decoder in pieces of `step` bytes; the decoded text, or "<error>".
std::string decodeChunked(std::string_view body, usize step, bool* done = nullptr) {
    ChunkedDecoder decoder;
    std::string out;
    for (usize i = 0; i < body.size(); i += step) {
        const bool ok = decoder.feed(bytesOf(body.substr(i, step)), [&](std::span<const u8> piece) {
            out.append(reinterpret_cast<const char*>(piece.data()), piece.size());
            return true;
        });
        if (!ok) {
            return "<error>";
        }
    }
    if (done) {
        *done = decoder.done();
    }
    return out;
}

} // namespace

TEST_CASE("HTTP client: response heads") {
    Result<HttpResponseHead> head = parseResponseHead("HTTP/1.1 206 Partial Content\r\nContent-Length: 90\r\n"
                                                      "Content-Range:  bytes 10-99/100 \r\nETag: \"abc\"\r\n"
                                                      "X-Odd:value:with:colons\r\n");
    REQUIRE(head.ok());
    CHECK(head.value().status == 206);
    CHECK(head.value().contentLength == 90);
    CHECK_FALSE(head.value().chunked);
    CHECK(head.value().header("content-range") == "bytes 10-99/100");
    CHECK(head.value().header("etag") == "\"abc\"");
    CHECK(head.value().header("x-odd") == "value:with:colons");
    CHECK(head.value().header("missing").empty());

    // No reason phrase, HTTP/1.0, chunked wins over a length.
    CHECK(parseResponseHead("HTTP/1.0 200").value().status == 200);
    CHECK(parseResponseHead("HTTP/1.1 503 Service Unavailable\r\nRetry-After: 2")
              .value()
              .header("retry-after") == "2");
    Result<HttpResponseHead> chunked =
        parseResponseHead("HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\nContent-Length: 5\r\n");
    REQUIRE(chunked.ok());
    CHECK(chunked.value().chunked);
    CHECK(chunked.value().contentLength == -1);
    CHECK(parseResponseHead("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n").value().chunked);
    CHECK(parseResponseHead("HTTP/1.1 200 OK\r\n").value().contentLength == -1);
    // Beyond 4 GB (the WoW's archives are 2 GB; a pack could pass 4 GB one day).
    CHECK(parseResponseHead("HTTP/1.1 200 OK\r\nContent-Length: 6000000000\r\n").value().contentLength ==
          6000000000);

    CHECK_FALSE(parseResponseHead("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n").ok());
    CHECK_FALSE(parseResponseHead("HTTP/1.1 200 OK\r\nContent-Length: -1\r\n").ok());
    CHECK_FALSE(parseResponseHead("HTTP/1.1 200 OK\r\nContent-Length: 12x\r\n").ok());
    CHECK_FALSE(parseResponseHead("HTTP/2 200").ok());
    CHECK_FALSE(parseResponseHead("HTTP/1.1 20 OK").ok());
    CHECK_FALSE(parseResponseHead("HTTP/1.1 2000 OK").ok());
    CHECK_FALSE(parseResponseHead("SSH-2.0-OpenSSH").ok());
    CHECK_FALSE(parseResponseHead("HTTP/1.1 200 OK\r\nno colon here\r\n").ok());
}

TEST_CASE("HTTP client: content ranges") {
    ContentRange range;
    REQUIRE(parseContentRange("bytes 100-199/1000", range));
    CHECK(range.first == 100);
    CHECK(range.last == 199);
    CHECK(range.total == 1000);
    CHECK_FALSE(range.unsatisfied);
    REQUIRE(parseContentRange("bytes */1000", range));
    CHECK(range.unsatisfied);
    CHECK(range.total == 1000);
    REQUIRE(parseContentRange("bytes 5000000000-5999999999/6000000000", range));
    CHECK(range.first == 5000000000ull);

    CHECK_FALSE(parseContentRange("bytes 200-100/1000", range)); // backwards
    CHECK_FALSE(parseContentRange("bytes 0-1000/1000", range));  // past the end
    CHECK_FALSE(parseContentRange("bytes 0-9/*", range));        // no total: nothing to resume against
    CHECK_FALSE(parseContentRange("items 0-9/10", range));
    CHECK_FALSE(parseContentRange("bytes 0-9", range));
    CHECK_FALSE(parseContentRange("", range));
}

TEST_CASE("HTTP client: chunked bodies") {
    const std::string body =
        "4\r\nWiki\r\n6;name=value\r\npedia \r\nE\r\nin \r\n\r\nchunks.\r\n0\r\nX-Trailer: 1\r\n\r\n";
    for (const usize step : {usize{1}, usize{2}, usize{3}, usize{7}, body.size()}) {
        bool done = false;
        CHECK(decodeChunked(body, step, &done) == "Wikipedia in \r\n\r\nchunks.");
        CHECK(done);
    }
    bool done = false;
    CHECK(decodeChunked("0\r\n\r\n", 1, &done).empty());
    CHECK(done);
    CHECK(decodeChunked("a\r\n0123456789\r\n0\r\n\r\n", 4) == "0123456789");
    CHECK(decodeChunked("A\r\n0123456789\r\n", 5, &done) == "0123456789");
    CHECK_FALSE(done); // the last chunk has not come
    CHECK(decodeChunked("x\r\n", 1) == "<error>");
    CHECK(decodeChunked("\r\n", 1) == "<error>");
    CHECK(decodeChunked("3\r\nabcX\r\n", 1) == "<error>"); // a chunk longer than it said
    CHECK(decodeChunked("1000000000000000\r\n", 64) == "<error>");

    // The output may stop the decoding.
    ChunkedDecoder decoder;
    CHECK_FALSE(decoder.feed(bytesOf("3\r\nabc\r\n"), [](std::span<const u8>) { return false; }));
}

TEST_CASE("HTTP client: addresses of the local network") {
    u32 address = 0;
    REQUIRE(parseIpv4("192.168.1.20", address));
    CHECK(address == 0xC0A80114u);
    CHECK(ipv4Text(address) == "192.168.1.20");
    CHECK(parseIpv4("010.000.000.001", address)); // decimal, never octal
    CHECK(address == 0x0A000001u);
    CHECK_FALSE(parseIpv4("256.1.1.1", address));
    CHECK_FALSE(parseIpv4("1.2.3", address));
    CHECK_FALSE(parseIpv4("1.2.3.4.5", address));
    CHECK_FALSE(parseIpv4("1..3.4", address));
    CHECK_FALSE(parseIpv4("1.2.3.4 ", address));
    CHECK_FALSE(parseIpv4("pc.local", address));
    CHECK_FALSE(parseIpv4("1234.1.1.1", address));

    CHECK(isLocalNetworkAddress(0x7F000001u));       // 127.0.0.1: adb reverse, the desktop
    CHECK(isLocalNetworkAddress(0x0A0B0C0Du));       // 10.11.12.13
    CHECK(isLocalNetworkAddress(0xAC1F0001u));       // 172.31.0.1
    CHECK_FALSE(isLocalNetworkAddress(0xAC200001u)); // 172.32.0.1
    CHECK(isLocalNetworkAddress(0xA9FE0102u));       // 169.254.1.2
    CHECK(isLocalNetworkAddress(0x64400001u));       // 100.64.0.1
    CHECK_FALSE(isLocalNetworkAddress(0x08080808u)); // 8.8.8.8
    CHECK_FALSE(isLocalNetworkAddress(0xC0A90101u)); // 192.169.1.1

    HttpEndpoint endpoint;
    REQUIRE(parseLocalEndpoint(" 192.168.1.20:47801 ", 47800, endpoint));
    CHECK(endpoint.host == "192.168.1.20");
    CHECK(endpoint.port == 47801);
    CHECK(endpoint.text() == "192.168.1.20:47801");
    REQUIRE(parseLocalEndpoint("192.168.001.020", 47800, endpoint));
    CHECK(endpoint.text() == "192.168.1.20:47800"); // canonical: what is connected to
    CHECK_FALSE(parseLocalEndpoint("8.8.8.8:47800", 47800, endpoint));
    CHECK_FALSE(parseLocalEndpoint("192.168.1.0:47800", 47800, endpoint));   // the network itself
    CHECK_FALSE(parseLocalEndpoint("192.168.1.255:47800", 47800, endpoint)); // its broadcast
    CHECK_FALSE(parseLocalEndpoint("192.168.1.20:0", 47800, endpoint));
    CHECK_FALSE(parseLocalEndpoint("192.168.1.20:65536", 47800, endpoint));
    CHECK_FALSE(parseLocalEndpoint("192.168.1.20:", 47800, endpoint));
    CHECK_FALSE(parseLocalEndpoint("192.168.1.20:47800/x", 47800, endpoint));
    CHECK_FALSE(parseLocalEndpoint("my-pc:47800", 47800, endpoint));
    CHECK_FALSE(parseLocalEndpoint("", 47800, endpoint));
}

TEST_CASE("HTTP client: the request") {
    HttpEndpoint at;
    at.host = "192.168.1.20";
    at.port = 47800;
    HttpGet get;
    get.target = "/packs/models.vdpk";
    get.headers.emplace_back("X-VD-Key", "0123");
    get.headers.emplace_back("Range", "bytes=100-");
    const std::string text = httpRequestText(at, get);
    CHECK(text.rfind("GET /packs/models.vdpk HTTP/1.1\r\nHost: 192.168.1.20:47800\r\n", 0) == 0);
    CHECK(text.find("\r\nConnection: close\r\n") != std::string::npos);
    CHECK(text.find("\r\nX-VD-Key: 0123\r\n") != std::string::npos);
    CHECK(text.find("\r\nRange: bytes=100-\r\n") != std::string::npos);
    CHECK(text.substr(text.size() - 4) == "\r\n\r\n");
}

TEST_CASE("HTTP client: nobody listening") {
    // A port of the loopback where nothing listens: Unreachable at once (refused), never Cut.
    HttpEndpoint at;
    at.host = "127.0.0.1";
    at.port = 9; // discard: closed on any development PC
    HttpGet get;
    get.target = "/info.json";
    get.connectTimeoutMs = 2000;
    std::string body;
    const HttpOutcome outcome = httpGetText(at, get, 1024, body);
    CHECK(outcome.kind == HttpOutcome::Kind::Unreachable);
    CHECK_FALSE(outcome.hasHead);
}
