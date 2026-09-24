#ifndef FIBER_NET_TLS_CREDENTIAL_H
#define FIBER_NET_TLS_CREDENTIAL_H

#include <array>
#include <cstdint>
#include <memory>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../tls/crypto/TlsCertificate.h"
#include "../tls/crypto/TlsSignature.h"
#include "TlsPemSource.h"

struct ssl_credential_st;
typedef struct ssl_credential_st SSL_CREDENTIAL;

namespace fiber::quic {
class QuicTlsSession;
}

namespace fiber::net {

class TlsServerHandshakeConfig;
namespace detail {
class TlsSslFactory;
class TlsStreamFd;
} // namespace detail

struct TlsCredentialOptions {
    // PEM leaf certificate followed by optional intermediate certificates.
    TlsPemSource certificate_chain{};
    // PEM private key matching the leaf certificate.
    TlsPemSource private_key{};
};

// Immutable, reusable certificate chain and private key. The material lives
// in the tls layer's TlsCertificateChain/TlsPrivateKey (09 §4.3): the TCP
// engine path stages borrowed pointers to it for each handshake. A lazily
// built BoringSSL SSL_CREDENTIAL projection (ssl_credential()) serves the
// QUIC-side glue: after SSL_add1_credential succeeds the SSL holds its own
// reference, so this object only needs to stay alive until that call.
class TlsCredential : public common::NonCopyable, public common::NonMovable {
public:
    ~TlsCredential();

    [[nodiscard]] static common::IoResult<std::unique_ptr<TlsCredential>>
    create(const TlsCredentialOptions &options) noexcept;

private:
    friend class TlsServerHandshakeConfig;
    friend class detail::TlsSslFactory;
    friend class detail::TlsStreamFd;
    friend class quic::QuicTlsSession;

    TlsCredential() noexcept = default;

    // Borrowed tls material for the engine (TCP) path. Never empty on an
    // object create() returned successfully.
    [[nodiscard]] const tls::TlsCertificateChain &tls_chain() const noexcept { return chain_; }
    [[nodiscard]] const tls::TlsPrivateKey &tls_key() const noexcept { return key_; }
    // Lazily built BoringSSL projection for the SSL (QUIC) path; null only on
    // allocation failure. Borrowed for this object's lifetime — SSL_add1_
    // credential retains across it.
    [[nodiscard]] SSL_CREDENTIAL *ssl_credential() const noexcept;

    tls::TlsCertificateChain chain_;
    tls::TlsPrivateKey key_;
    mutable SSL_CREDENTIAL *ssl_bridge_ = nullptr;
    // SHA-256 of the leaf DER — the session-id context the SSL path installs
    // on first add_credential (the engine path has no session-id cache).
    std::array<std::uint8_t, 32> session_identity_{};
};

} // namespace fiber::net

#endif // FIBER_NET_TLS_CREDENTIAL_H
