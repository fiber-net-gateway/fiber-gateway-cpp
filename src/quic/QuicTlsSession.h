#ifndef FIBER_QUIC_QUIC_TLS_SESSION_H
#define FIBER_QUIC_QUIC_TLS_SESSION_H

#include <cstddef>
#include <cstdint>
#include <optional>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"

struct ssl_st;
typedef struct ssl_st SSL;

namespace fiber::net {
class TlsServerContext;
} // namespace fiber::net

namespace fiber::quic {

enum class QuicEncryptionLevel : std::uint8_t;
class QuicConnection;

class QuicTlsSession : public common::NonCopyable, public common::NonMovable {
public:
    QuicTlsSession() noexcept = default;
    ~QuicTlsSession();

    [[nodiscard]] common::IoResult<void> init_server(net::TlsServerContext &context,
                                                     QuicConnection &connection) noexcept;
    [[nodiscard]] common::IoResult<void> provide_crypto_data(QuicEncryptionLevel level, const std::uint8_t *data,
                                                             std::size_t len) noexcept;
    [[nodiscard]] common::IoResult<void> drive_handshake() noexcept;

    [[nodiscard]] bool initialized() const noexcept { return ssl_ != nullptr; }
    [[nodiscard]] bool handshake_done() const noexcept;
    [[nodiscard]] SSL *raw() const noexcept { return ssl_; }

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
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_TLS_SESSION_H
