#ifndef FIBER_NET_DETAIL_TLS_SSL_FACTORY_H
#define FIBER_NET_DETAIL_TLS_SSL_FACTORY_H

#include <fiber/common/IoError.h>
#include <fiber/net/TlsParams.h>
#include <fiber/net/detail/TlsHandshakeState.h>

struct ssl_st;
typedef struct ssl_st SSL;

namespace fiber::net::detail {

struct TlsNewSessionOps;

class TlsSslFactory {
public:
    [[nodiscard]] static common::IoResult<SSL *>
    create_client(const TlsClientParam &param, bool enable_early_data = false,
                  const TlsNewSessionOps *new_session_ops = nullptr) noexcept;
    [[nodiscard]] static common::IoResult<SSL *>
    create_server(const TlsServerParam &param, TlsServerHandshakeState &state, const SocketAddress *local_addr,
                  const SocketAddress *remote_addr, TlsTransportKind transport) noexcept;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_TLS_SSL_FACTORY_H
