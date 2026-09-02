#ifndef FIBER_NET_DETAIL_TLS_RUNTIME_H
#define FIBER_NET_DETAIL_TLS_RUNTIME_H

#include <openssl/ssl.h>

namespace fiber::net::detail {

class TlsRuntime {
public:
    [[nodiscard]] static SSL_CTX *client_context() noexcept;
    [[nodiscard]] static SSL_CTX *server_context() noexcept;
    [[nodiscard]] static int server_handshake_state_index() noexcept;
    [[nodiscard]] static int new_session_ops_index() noexcept;
    [[nodiscard]] static enum ssl_select_cert_result_t
    select_certificate_callback(const SSL_CLIENT_HELLO *client_hello) noexcept;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_TLS_RUNTIME_H
