#ifndef FIBER_NET_TLS_CREDENTIAL_H
#define FIBER_NET_TLS_CREDENTIAL_H

#include <cstdint>
#include <memory>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../tls/crypto/TlsCertificate.h"
#include "../tls/crypto/TlsSignature.h"
#include "TlsPemSource.h"

namespace fiber::quic {
class QuicTlsSession;
}

namespace fiber::net {

class TlsServerHandshakeConfig;
namespace detail {
class TlsStreamFd;
} // namespace detail

struct TlsCredentialOptions {
    // PEM leaf certificate followed by optional intermediate certificates.
    TlsPemSource certificate_chain{};
    // PEM private key matching the leaf certificate.
    TlsPemSource private_key{};
};

// Immutable, reusable certificate chain and private key. The material lives
// in the tls layer's TlsCertificateChain/TlsPrivateKey (09 §4.3) and both
// faces (TCP engine path, QUIC glue) stage borrowed pointers to it for each
// handshake: the credential must outlive the handshakes it serves (the
// documented param contract).
class TlsCredential : public common::NonCopyable, public common::NonMovable {
public:
    ~TlsCredential() = default;

    [[nodiscard]] static common::IoResult<std::unique_ptr<TlsCredential>>
    create(const TlsCredentialOptions &options) noexcept;

private:
    friend class TlsServerHandshakeConfig;
    friend class detail::TlsStreamFd;
    friend class quic::QuicTlsSession;

    TlsCredential() noexcept = default;

    // Borrowed tls material for the engine paths. Never empty on an object
    // create() returned successfully.
    [[nodiscard]] const tls::TlsCertificateChain &tls_chain() const noexcept { return chain_; }
    [[nodiscard]] const tls::TlsPrivateKey &tls_key() const noexcept { return key_; }

    tls::TlsCertificateChain chain_;
    tls::TlsPrivateKey key_;
};

} // namespace fiber::net

#endif // FIBER_NET_TLS_CREDENTIAL_H
