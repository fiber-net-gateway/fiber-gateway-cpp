#ifndef FIBER_HTTP_TLS_ALPN_H
#define FIBER_HTTP_TLS_ALPN_H

#include <fiber/http/HttpClientTlsOptions.h>
#include <fiber/http/HttpServerTlsOptions.h>
#include <fiber/net/TlsParams.h>

namespace fiber::http {

[[nodiscard]] net::TlsClientParam make_http1_client_tls_param(const HttpClientTlsOptions &options) noexcept;
[[nodiscard]] net::TlsClientParam make_http2_client_tls_param(const HttpClientTlsOptions &options) noexcept;

// Each server type advertises a fixed ALPN protocol set; it is not
// caller-configurable (see HttpServerTlsOptions).
[[nodiscard]] net::TlsServerParam make_http1_server_tls_param(const HttpServerTlsOptions &options) noexcept;
[[nodiscard]] net::TlsServerParam make_http_server_tls_param(const HttpServerTlsOptions &options) noexcept;
[[nodiscard]] net::TlsServerParam make_http3_server_tls_param(const HttpServerTlsOptions &options) noexcept;

} // namespace fiber::http

#endif // FIBER_HTTP_TLS_ALPN_H
