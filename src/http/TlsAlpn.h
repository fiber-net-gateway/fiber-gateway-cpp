#ifndef FIBER_HTTP_TLS_ALPN_H
#define FIBER_HTTP_TLS_ALPN_H

#include <fiber/http/HttpClientTlsOptions.h>
#include <fiber/net/TlsParams.h>

namespace fiber::http {

[[nodiscard]] net::TlsClientParam make_http1_client_tls_param(const HttpClientTlsOptions &options) noexcept;
[[nodiscard]] net::TlsClientParam make_http2_client_tls_param(const HttpClientTlsOptions &options) noexcept;
// Rebuilds alpn with the standard protocols prepended (custom entries kept,
// deduplicated) and repoints options.alpn at the list's storage. The list must
// outlive every handshake started with options (e.g. live in HttpServerOptions).
void normalize_http1_alpn(net::TlsServerParam &options, net::TlsAlpnList &alpn);
void normalize_http_server_alpn(net::TlsServerParam &options, net::TlsAlpnList &alpn);
void normalize_http3_alpn(net::TlsServerParam &options, net::TlsAlpnList &alpn);

} // namespace fiber::http

#endif // FIBER_HTTP_TLS_ALPN_H
