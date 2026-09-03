#ifndef FIBER_NET_DETAIL_TLS_SSL_FACTORY_H
#define FIBER_NET_DETAIL_TLS_SSL_FACTORY_H

#include <fiber/common/IoError.h>
#include <fiber/net/TlsParams.h>

struct ssl_st;
typedef struct ssl_st SSL;

namespace fiber::net::detail {

struct TlsNewSessionOps;

class TlsSslFactory {
public:
    [[nodiscard]] static common::IoResult<SSL *>
    create_client(const TlsClientParam &param, bool enable_early_data = false,
                  const TlsNewSessionOps *new_session_ops = nullptr) noexcept;
    // Installs the param's static configuration (protocol versions, early
    // data, trust store, client certificate mode). The configure callback and
    // ALPN preference list stay borrowed from the param: the caller wires a
    // TlsServerHandshakeState (which borrows the param) via
    // TlsRuntime::set_server_handshake_state() for the handshake's duration.
    [[nodiscard]] static common::IoResult<SSL *> create_server(const TlsServerParam &param) noexcept;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_TLS_SSL_FACTORY_H
