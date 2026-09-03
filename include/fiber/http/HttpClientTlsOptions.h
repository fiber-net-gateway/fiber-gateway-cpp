#ifndef FIBER_HTTP_HTTP_CLIENT_TLS_OPTIONS_H
#define FIBER_HTTP_HTTP_CLIENT_TLS_OPTIONS_H

#include <chrono>
#include <string>

#include "../net/TlsParams.h"

namespace fiber::http {

// TLS settings that remain caller-configurable for an HTTP/1 or HTTP/2
// connection. The HTTP implementation owns ALPN and supplies the protocol it
// actually speaks.
struct HttpClientTlsOptions {
    net::TlsClientSecurity security{};
    std::chrono::milliseconds handshake_timeout{net::kDefaultTlsHandshakeTimeout};
    int min_version = 0x0303; // TLS 1.2
    int max_version = 0x0304; // TLS 1.3
    std::string server_name{};
    std::string verify_name{};
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_CLIENT_TLS_OPTIONS_H
