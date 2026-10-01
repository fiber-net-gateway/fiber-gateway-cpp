#ifndef FIBER_NET_TLS_CREDENTIAL_H
#define FIBER_NET_TLS_CREDENTIAL_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "../common/Assert.h"
#include "../common/IoError.h"
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
class TlsClientStager;
} // namespace detail

struct TlsCredentialOptions {
    // PEM leaf certificate followed by optional intermediate certificates.
    TlsPemSource certificate_chain{};
    // PEM private key matching the leaf certificate.
    TlsPemSource private_key{};
};

// Immutable, reusable certificate chain and private key behind a refcounted
// handle — one reference count for the whole credential, like BoringSSL's
// SSL_CREDENTIAL. The material lives in the tls layer's
// TlsCertificateChain/TlsPrivateKey (09 §4.3) and both faces (TCP engine path,
// QUIC glue) stage borrowed pointers into it for each handshake, so a handle
// move or copy never relocates what a handshake reads. Copying a handle takes
// one atomic reference; handles to the same material may be copied and
// destroyed concurrently on different threads, while a single handle object
// follows the usual one-writer rule. A handle is empty only when
// default-constructed, moved from or reset.
class TlsCredential {
public:
    TlsCredential() noexcept = default;
    TlsCredential(const TlsCredential &other) noexcept;
    TlsCredential &operator=(const TlsCredential &other) noexcept;
    TlsCredential(TlsCredential &&other) noexcept : impl_(other.impl_) { other.impl_ = nullptr; }
    TlsCredential &operator=(TlsCredential &&other) noexcept;
    ~TlsCredential() { release(); }

    [[nodiscard]] static common::IoResult<TlsCredential> create(const TlsCredentialOptions &options) noexcept;

    [[nodiscard]] bool empty() const noexcept { return impl_ == nullptr; }
    // Handles currently sharing this material, this one included; 0 when
    // empty. A diagnostic snapshot: other threads may change it at any time.
    [[nodiscard]] std::size_t use_count() const noexcept;
    // Drops this handle's reference and leaves it empty.
    void reset() noexcept;

private:
    friend class TlsServerHandshakeConfig;
    friend class detail::TlsStreamFd;
    friend class detail::TlsClientStager;
    friend class quic::QuicTlsSession;

    struct Impl {
        std::atomic<std::uint32_t> refs{1};
        tls::TlsCertificateChain chain;
        tls::TlsPrivateKey key;
    };

    explicit TlsCredential(Impl *impl) noexcept : impl_(impl) {}

    void release() noexcept;

    // Borrowed tls material for the engine paths; valid while any handle to
    // this material lives. The edges reject empty handles before staging.
    [[nodiscard]] const tls::TlsCertificateChain &tls_chain() const noexcept {
        FIBER_ASSERT(impl_ != nullptr);
        return impl_->chain;
    }
    [[nodiscard]] const tls::TlsPrivateKey &tls_key() const noexcept {
        FIBER_ASSERT(impl_ != nullptr);
        return impl_->key;
    }

    Impl *impl_ = nullptr;
};

} // namespace fiber::net

#endif // FIBER_NET_TLS_CREDENTIAL_H
