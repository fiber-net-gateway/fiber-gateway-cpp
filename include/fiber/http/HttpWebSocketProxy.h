#ifndef FIBER_HTTP_WEB_SOCKET_PROXY_H
#define FIBER_HTTP_WEB_SOCKET_PROXY_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../async/Task.h"

namespace fiber::http {

class ClientHttp1Exchange;
class HttpExchange;
class HttpHeaders;
struct Http1ResponseHead;

namespace proxy_core {

enum class WebSocketDownstream : std::uint8_t {
    None,
    Http1Upgrade,
    ExtendedConnect,
};

// Fixed-size handshake fields: base64(RAND 16B) is always 24 chars and
// base64(SHA1 20B) always 28 — both exceed std::string SSO, so heap staging
// under noexcept was a terminate risk. Fixed arrays make the handshake
// trivially copyable and allocation-free.
inline constexpr std::size_t kWebSocketKeyB64Len = 24;
inline constexpr std::size_t kWebSocketAcceptB64Len = 28;

struct WebSocketHandshake {
    WebSocketDownstream downstream = WebSocketDownstream::None;
    bool key_generated = false; // set by prepare_websocket_handshake (extended connect only)
    bool accept_ready = false; // set by prepare_upstream_websocket_headers on success
    std::array<char, kWebSocketKeyB64Len> generated_key{};
    std::array<char, kWebSocketAcceptB64Len> expected_accept{};

    [[nodiscard]] bool active() const noexcept { return downstream != WebSocketDownstream::None; }
    [[nodiscard]] bool extended_connect() const noexcept { return downstream == WebSocketDownstream::ExtendedConnect; }
    // Views are empty until the corresponding flag is set, matching the old
    // std::string fields' unset-state semantics.
    [[nodiscard]] std::string_view key_view() const noexcept {
        return key_generated ? std::string_view{generated_key.data(), generated_key.size()} : std::string_view{};
    }
    [[nodiscard]] std::string_view accept_view() const noexcept {
        return accept_ready ? std::string_view{expected_accept.data(), expected_accept.size()} : std::string_view{};
    }
};

[[nodiscard]] WebSocketDownstream detect_websocket_downstream(const HttpExchange &exchange) noexcept;

// Generates the HTTP/1.1 key needed when translating an HTTP/2 or HTTP/3
// Extended CONNECT request. A regular HTTP/1.1 Upgrade keeps the downstream key.
[[nodiscard]] bool prepare_websocket_handshake(WebSocketHandshake &handshake) noexcept;

// Finalizes a pre-built upstream request header block. Call this after caller-specific
// header overrides so protocol-required fields cannot be accidentally removed. It also
// computes the expected Sec-WebSocket-Accept used to validate the upstream 101 response.
[[nodiscard]] bool prepare_upstream_websocket_headers(const HttpExchange &exchange, WebSocketHandshake &handshake,
                                                      HttpHeaders &headers) noexcept;

[[nodiscard]] bool valid_websocket_upgrade_response(const Http1ResponseHead &head,
                                                    const WebSocketHandshake &handshake) noexcept;

// Copies the successful upstream handshake to the downstream protocol. HTTP/1.1 receives
// a 101-style Upgrade header block; Extended CONNECT receives an HTTP/2/3-safe header block.
void build_downstream_websocket_headers(const Http1ResponseHead &upstream_head, HttpHeaders &headers,
                                        const WebSocketHandshake &handshake) noexcept;

// Reasserts protocol-required response fields after caller-specific response overrides.
void finalize_downstream_websocket_headers(HttpHeaders &headers, const WebSocketHandshake &handshake) noexcept;

// Relays raw WebSocket bytes in both directions until either side closes or fails. Both
// exchanges and the upstream connection lease must outlive this call.
fiber::async::Task<void>
relay_websocket_tunnel(HttpExchange &downstream, ClientHttp1Exchange &upstream,
                       std::chrono::milliseconds read_timeout = std::chrono::milliseconds::max(),
                       std::chrono::milliseconds write_timeout = std::chrono::milliseconds::max()) noexcept;

} // namespace proxy_core
} // namespace fiber::http

#endif // FIBER_HTTP_WEB_SOCKET_PROXY_H
