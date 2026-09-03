#ifndef FIBER_HTTP_HTTP_SERVER_TLS_OPTIONS_H
#define FIBER_HTTP_HTTP_SERVER_TLS_OPTIONS_H

#include "../net/TlsParams.h"

namespace fiber::http {

// TLS policy shared by HttpServer/Http1Server/Http3Server. ALPN is not
// configurable here: each server advertises the fixed protocol set it
// implements (see TlsAlpn.h's make_*_server_tls_param helpers).
struct HttpServerTlsOptions {
    net::ConfigureTlsCallback configure_callback = nullptr;
    void *configure_ctx = nullptr;
    const net::TrustStore *trust_store = nullptr;
    net::TlsClientCertificateMode client_certificate_mode = net::TlsClientCertificateMode::None;
    int min_version = 0x0303; // TLS 1.2
    int max_version = 0x0304; // TLS 1.3
    bool enable_early_data = false;

    [[nodiscard]] bool enabled() const noexcept { return configure_callback != nullptr; }
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_SERVER_TLS_OPTIONS_H
