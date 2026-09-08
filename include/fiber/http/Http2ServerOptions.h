#ifndef FIBER_HTTP_HTTP2_SERVER_OPTIONS_H
#define FIBER_HTTP_HTTP2_SERVER_OPTIONS_H

#include <chrono>

namespace fiber::http {

// HTTP/2 endpoint policy; other Http2Connection::Options retain their defaults.
struct Http2ServerOptions {
    // Inbound-idle deadline. Expiry sends a keepalive PING first and only
    // closes if the peer stays silent (see Http2Connection::Options).
    std::chrono::milliseconds read_timeout{std::chrono::seconds(70)};
    std::chrono::milliseconds write_timeout{std::chrono::seconds(30)};
    // Advertise SETTINGS_ENABLE_CONNECT_PROTOCOL (RFC 8441), which WebSocket
    // over HTTP/2 needs.
    bool enable_connect_protocol = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_SERVER_OPTIONS_H
