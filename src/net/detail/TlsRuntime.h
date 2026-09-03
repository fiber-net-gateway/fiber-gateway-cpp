#ifndef FIBER_NET_DETAIL_TLS_RUNTIME_H
#define FIBER_NET_DETAIL_TLS_RUNTIME_H

#include <openssl/ssl.h>

namespace fiber::net::detail {

struct TlsServerHandshakeState;

class TlsRuntime {
public:
    [[nodiscard]] static SSL_CTX *client_context() noexcept;
    [[nodiscard]] static SSL_CTX *server_context() noexcept;
    [[nodiscard]] static int server_handshake_state_index() noexcept;
    [[nodiscard]] static int new_session_ops_index() noexcept;
    // Wires (or, with a null state, unwires) the handshake state owned by the
    // coroutine or session driving a server handshake. The SSL callbacks read
    // it through ex_data; when unset they fail safe (handshake error, ALPN
    // no-ack) instead of touching stale memory.
    static void set_server_handshake_state(SSL *ssl, TlsServerHandshakeState *state) noexcept;
    [[nodiscard]] static enum ssl_select_cert_result_t
    select_certificate_callback(const SSL_CLIENT_HELLO *client_hello) noexcept;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_TLS_RUNTIME_H
