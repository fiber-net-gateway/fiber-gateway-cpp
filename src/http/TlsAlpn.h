#ifndef FIBER_HTTP_TLS_ALPN_H
#define FIBER_HTTP_TLS_ALPN_H

#include <fiber/net/TlsConnectionOptions.h>

namespace fiber::http {

void normalize_http1_alpn(net::TlsClientConnectionOptions &options);
void normalize_http1_alpn(net::TlsServerConnectionOptions &options);
void normalize_http_server_alpn(net::TlsServerConnectionOptions &options);
void normalize_http3_alpn(net::TlsClientConnectionOptions &options);
void normalize_http3_alpn(net::TlsServerConnectionOptions &options);

} // namespace fiber::http

#endif // FIBER_HTTP_TLS_ALPN_H
