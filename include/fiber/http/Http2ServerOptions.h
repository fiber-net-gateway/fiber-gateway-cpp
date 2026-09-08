#ifndef FIBER_HTTP_HTTP2_SERVER_OPTIONS_H
#define FIBER_HTTP_HTTP2_SERVER_OPTIONS_H

#include <chrono>

namespace fiber::http {

// Connection-level knobs an HTTP/2 server endpoint exposes. Split out of the
// old catch-all HttpServerOptions, which offered exactly these three for
// HTTP/2; everything else in Http2Connection::Options keeps its default. Add a
// field here (and map it in Http2Endpoint::make_connection_options) when a knob
// turns out to be worth configuring.
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
