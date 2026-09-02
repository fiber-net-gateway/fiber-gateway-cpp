#ifndef FIBER_HTTP_TLS_ALPN_H
#define FIBER_HTTP_TLS_ALPN_H

#include <fiber/net/TlsParams.h>

namespace fiber::http {

void normalize_http1_alpn(net::TlsClientParam &options);
void normalize_http1_alpn(net::TlsServerParam &options);
void normalize_http_server_alpn(net::TlsServerParam &options);
void normalize_http3_alpn(net::TlsClientParam &options);
void normalize_http3_alpn(net::TlsServerParam &options);

} // namespace fiber::http

#endif // FIBER_HTTP_TLS_ALPN_H
