#ifndef FIBER_HTTP_WEB_SOCKET_PROXY_H
#define FIBER_HTTP_WEB_SOCKET_PROXY_H

#include <chrono>
#include <cstdint>
#include <string>

#include "../async/Task.h"

namespace fiber::http {

class ClientHttp1Exchange;
class HttpExchange;
class HttpHeaders;
struct ClientResponseHead;

namespace proxy_core {

enum class WebSocketDownstream : std::uint8_t {
    None,
    Http1Upgrade,
    ExtendedConnect,
};

struct WebSocketHandshake {
    WebSocketDownstream downstream = WebSocketDownstream::None;
    std::string generated_key;
    std::string expected_accept;

    [[nodiscard]] bool active() const noexcept { return downstream != WebSocketDownstream::None; }
    [[nodiscard]] bool extended_connect() const noexcept { return downstream == WebSocketDownstream::ExtendedConnect; }
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

[[nodiscard]] bool valid_websocket_upgrade_response(const ClientResponseHead &head,
                                                    const WebSocketHandshake &handshake) noexcept;

// Copies the successful upstream handshake to the downstream protocol. HTTP/1.1 receives
// a 101-style Upgrade header block; Extended CONNECT receives an HTTP/2/3-safe header block.
void build_downstream_websocket_headers(const ClientResponseHead &upstream_head, HttpHeaders &headers,
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
