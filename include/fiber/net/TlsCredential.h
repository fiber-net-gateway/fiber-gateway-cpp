#ifndef FIBER_NET_TLS_CREDENTIAL_H
#define FIBER_NET_TLS_CREDENTIAL_H

#include <array>
#include <cstdint>
#include <memory>
#include <span>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "TlsPemSource.h"

struct ssl_credential_st;
typedef struct ssl_credential_st SSL_CREDENTIAL;

namespace fiber::net {

class TlsServerHandshakeConfig;
namespace detail {
class TlsSslFactory;
}

struct TlsCredentialOptions {
    // PEM leaf certificate followed by optional intermediate certificates.
    TlsPemSource certificate_chain{};
    // PEM private key matching the leaf certificate.
    TlsPemSource private_key{};
};

// Immutable, reusable certificate chain and private key. This object owns one
// BoringSSL credential reference. SSL_add1_credential retains a separate
// reference, so this object only needs to remain alive until that call succeeds.
class TlsCredential : public common::NonCopyable, public common::NonMovable {
public:
    ~TlsCredential();

    [[nodiscard]] static common::IoResult<std::unique_ptr<TlsCredential>>
    create(const TlsCredentialOptions &options) noexcept;

private:
    friend class TlsServerHandshakeConfig;
    friend class detail::TlsSslFactory;

    TlsCredential() noexcept = default;

    SSL_CREDENTIAL *credential_ = nullptr;
    std::array<std::uint8_t, 32> session_identity_{};
};

} // namespace fiber::net

#endif // FIBER_NET_TLS_CREDENTIAL_H
