#ifndef FIBER_HTTP_TLS_ALPN_H
#define FIBER_HTTP_TLS_ALPN_H

#include "../net/TlsOptions.h"

namespace fiber::http {

void normalize_http1_alpn(net::TlsOptions &options);
void normalize_http_server_alpn(net::TlsOptions &options);
void normalize_http3_alpn(net::TlsOptions &options);

} // namespace fiber::http

#endif // FIBER_HTTP_TLS_ALPN_H
