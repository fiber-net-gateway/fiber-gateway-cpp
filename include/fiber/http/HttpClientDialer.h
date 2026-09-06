#ifndef FIBER_HTTP_HTTP_CLIENT_DIALER_H
#define FIBER_HTTP_HTTP_CLIENT_DIALER_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../event/EventLoop.h"
#include "../net/HappyEyeballs.h"
#include "../net/SocketAddress.h"
#include "../net/TcpSocketOptions.h"
#include "HttpClientTlsOptions.h"
#include "HttpCommon.h"

namespace fiber::http {

class HttpTransport;

enum class HttpProtocol : std::uint8_t {
    Http1,
    Http2,
    Http3,
};

[[nodiscard]] constexpr std::string_view http_protocol_alpn(HttpProtocol protocol) noexcept {
    switch (protocol) {
        case HttpProtocol::Http1:
            return "http/1.1";
        case HttpProtocol::Http2:
            return "h2";
        case HttpProtocol::Http3:
            return "h3";
    }
    return {};
}

[[nodiscard]] constexpr HttpVersion http_protocol_version(HttpProtocol protocol) noexcept {
    switch (protocol) {
        case HttpProtocol::Http1:
            return HttpVersion::HTTP_1_1;
        case HttpProtocol::Http2:
            return HttpVersion::HTTP_2_0;
        case HttpProtocol::Http3:
            return HttpVersion::HTTP_3_0;
    }
    return HttpVersion::HTTP_1_1;
}

// Maps a negotiated ALPN protocol name onto the HTTP implementation that speaks it. An empty view
// means the peer selected nothing (plaintext, or a TLS server without ALPN) and yields nullopt, so
// callers fall back to HttpClientDialRequest::default_protocol rather than guessing.
[[nodiscard]] std::optional<HttpProtocol> http_protocol_from_alpn(std::string_view alpn) noexcept;

// One TCP (optionally TLS) dial, protocol-agnostic. This is the shared body of
// Http1ClientConnection::connect and Http2ClientConnection::connect, and the only place that turns
// a negotiated ALPN name into a protocol choice, which is what lets a caller decide between
// HTTP/1 and HTTP/2 *after* the handshake instead of committing before it.
//
// Every argument is borrowed until the returned task completes, including the storage behind
// `addresses`, `alpn`, and the views inside `*tls`.
struct HttpClientDialRequest {
    // An engaged `peer` selects the single-address path; otherwise `addresses` is raced, and an
    // empty set there is a failure rather than a fallback.
    std::optional<net::SocketAddress> peer{};
    std::span<const net::SocketAddress> addresses{};
    net::HappyEyeballsOptions happy{};
    net::TcpSocketOptions tcp = net::kNoDelayTcpSocketOptions;
    // Null dials in the clear. `happy.total_timeout` covers the TCP phase only; the TLS handshake
    // has its own timeout in HttpClientTlsOptions.
    const HttpClientTlsOptions *tls = nullptr;
    // Protocols to offer. Empty leaves ALPN unset, which is also the only sensible choice for a
    // plaintext dial.
    std::span<const std::string_view> alpn{};
    // Reported when nothing was negotiated: plaintext prior-knowledge dials pass the protocol they
    // intend to speak, TLS dials that offered no ALPN get HTTP/1.
    HttpProtocol default_protocol = HttpProtocol::Http1;
    // getsockname() after the TCP phase. Only HTTP/2 needs it, so it is off by default.
    bool need_local_addr = false;
};

struct HttpClientDialResult {
    std::unique_ptr<HttpTransport> transport;
    // The address actually reached, which for a raced address set is the winner of the TCP phase.
    net::SocketAddress peer{};
    std::optional<net::SocketAddress> local{};
    HttpProtocol protocol = HttpProtocol::Http1;
};

// On failure nothing is left open: a transport created before the failing step is closed here.
[[nodiscard]] fiber::async::Task<common::IoResult<HttpClientDialResult>>
http_client_dial(event::EventLoop &loop, HttpClientDialRequest request) noexcept;

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_CLIENT_DIALER_H
