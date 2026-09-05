#ifndef FIBER_HTTP_HTTP_CLIENT_TLS_OPTIONS_H
#define FIBER_HTTP_HTTP_CLIENT_TLS_OPTIONS_H

#include <chrono>
#include <string_view>

#include "../net/TlsParams.h"

namespace fiber::http {

// TLS settings for an HTTP/1 or HTTP/2 client connection: TlsClientParam minus
// ALPN, plus the handshake timeout. ALPN is not configurable here because each
// HTTP implementation advertises the protocol it actually speaks (see
// TlsAlpn.h's make_*_client_tls_param helpers); the handshake timeout is
// operation policy for the transport driving the handshake rather than a TLS
// parameter, which is why TlsClientParam deliberately omits it.
//
// Non-owning, like TlsClientParam: server_name, verify_name, and the pointees
// in security are borrowed for the duration of the connect() call that takes
// this struct, and must stay valid until it completes. Copying is free and
// extends the borrow to the copy.
struct HttpClientTlsOptions {
    net::TlsClientSecurity security{};
    std::chrono::milliseconds handshake_timeout{net::kDefaultTlsHandshakeTimeout};
    int min_version = 0x0303; // TLS 1.2
    int max_version = 0x0304; // TLS 1.3
    std::string_view server_name{};
    std::string_view verify_name{};
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_CLIENT_TLS_OPTIONS_H
