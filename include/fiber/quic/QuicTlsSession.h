#ifndef FIBER_QUIC_QUIC_TLS_SESSION_H
#define FIBER_QUIC_QUIC_TLS_SESSION_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../net/TlsParams.h"
#include "../net/detail/TlsHandshakeState.h"
#include "../net/detail/TlsSessionOps.h"

struct ssl_st;
typedef struct ssl_st SSL;
struct ssl_session_st;
typedef struct ssl_session_st SSL_SESSION;

namespace fiber::quic {

enum class QuicEncryptionLevel : std::uint8_t;
class QuicConnection;

class QuicTlsSession : public common::NonCopyable, public common::NonMovable {
public:
    QuicTlsSession() noexcept = default;
    ~QuicTlsSession();

    [[nodiscard]] common::IoResult<void> init_server(const net::TlsServerParam &options,
                                                     QuicConnection &connection) noexcept;
    [[nodiscard]] common::IoResult<void> init_client(const net::TlsClientParam &param, QuicConnection &connection,
                                                     bool allow_insecure, SSL_SESSION *session = nullptr) noexcept;
    [[nodiscard]] common::IoResult<void> provide_crypto_data(QuicEncryptionLevel level, const std::uint8_t *data,
                                                             std::size_t len) noexcept;
    [[nodiscard]] common::IoResult<void> drive_handshake() noexcept;
    [[nodiscard]] common::IoResult<void> process_post_handshake() noexcept;

    [[nodiscard]] bool initialized() const noexcept { return ssl_ != nullptr; }
    [[nodiscard]] bool handshake_done() const noexcept;
    [[nodiscard]] bool session_reused() const noexcept;
    [[nodiscard]] std::string_view selected_alpn() const noexcept;
    [[nodiscard]] long peer_verify_result() const noexcept;
    [[nodiscard]] std::optional<std::uint8_t> last_alert() const noexcept { return last_alert_; }
    // Capture a TLS alert raised by BoringSSL's send_alert callback during the
    // handshake. drive_handshake() consumes it via take_pending_alert() to emit
    // a CRYPTO_ERROR close (RFC 9000 §20.1).
    void record_alert(std::uint8_t alert) noexcept;
    [[nodiscard]] std::optional<std::uint8_t> take_pending_alert() noexcept;

private:
    SSL *ssl_ = nullptr;
    // Alert stashed by record_alert() (set from within the TLS stack) and
    // drained by drive_handshake() once SSL_do_handshake returns, so connection
    // close state is never mutated re-entrantly from inside BoringSSL.
    std::optional<std::uint8_t> pending_alert_;
    std::optional<std::uint8_t> last_alert_;
    bool client_mode_ = false;
    bool verify_peer_ = false;
    net::detail::TlsNewSessionOps new_session_ops_{};
    // Server handshake borrow pair: the param copy (with the connection-level
    // early-data switch merged in) and the state borrowing it. Unlike TCP, the
    // QUIC handshake spans many drive_handshake() calls with no owning
    // coroutine frame, so both live here for the session's lifetime.
    net::TlsServerParam server_param_{};
    net::detail::TlsServerHandshakeState server_handshake_state_{};
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_TLS_SESSION_H
