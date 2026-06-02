#ifndef FIBER_QUIC_QUIC_TLS_SESSION_H
#define FIBER_QUIC_QUIC_TLS_SESSION_H

#include <cstddef>
#include <cstdint>

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

private:
    SSL *ssl_ = nullptr;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_TLS_SESSION_H
