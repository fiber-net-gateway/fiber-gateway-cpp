#ifndef FIBER_NET_TLS_SERVER_HANDSHAKE_CONFIG_H
#define FIBER_NET_TLS_SERVER_HANDSHAKE_CONFIG_H

#include <cstddef>
#include <span>

#include "../common/IoError.h"
#include "TlsParams.h"

namespace fiber::tls {
struct TlsServerConfig;
}

namespace fiber::quic {
class QuicTlsSession;
}

namespace fiber::net {

class TlsCredential;
class TrustStore;
namespace detail {
class TlsStreamFd;
} // namespace detail

// A synchronous, callback-duration view for configuring the current server
// handshake (09 §5): it stages into a caller-held tls::TlsServerConfig whose
// spans borrow the caller's material for the handshake's duration. It owns
// and exposes neither the config nor the material.
class TlsServerHandshakeConfig {
public:
    [[nodiscard]] common::IoErr clear_credentials() noexcept;
    // The staged config borrows the credential's tls material; the credential
    // must outlive the handshake (the documented param contract).
    [[nodiscard]] common::IoErr add_credential(const TlsCredential &credential) noexcept;
    // The staged config borrows the store's anchors; the store must outlive
    // the handshake.
    [[nodiscard]] common::IoErr set_trust_store(const TrustStore &trust_store) noexcept;
    // Nothing to scope — the TCP path has no session-id cache (resumption is
    // stateless tickets only); accepted and validated for callback
    // portability, then ignored.
    [[nodiscard]] common::IoErr set_session_id_context(std::span<const std::uint8_t> context) noexcept;
    // Sets both bounds; a value of 0 leaves that bound unchanged. The engine
    // domain is {TLS 1.2, TLS 1.3}.
    [[nodiscard]] common::IoErr set_protocol_versions(int min_version, int max_version) noexcept;
    // Client-certificate verification needs a trust store: install one via
    // TlsServerParam::trust_store or set_trust_store before requesting a mode
    // other than None, otherwise the handshake fails verification.
    [[nodiscard]] common::IoErr set_client_certificate_mode(TlsClientCertificateMode mode) noexcept;
    [[nodiscard]] common::IoErr set_early_data_enabled(bool enabled) noexcept;

private:
    friend class detail::TlsStreamFd;
    friend class fiber::quic::QuicTlsSession;

    // The caller owns `engine` (borrowed for the callback's duration and the
    // handshake it feeds) and watches `credential_count` for the
    // must-add-one rule.
    TlsServerHandshakeConfig(fiber::tls::TlsServerConfig &engine, std::size_t &credential_count) noexcept :
        engine_(&engine), credential_count_(&credential_count) {}

    [[nodiscard]] std::size_t credential_count() const noexcept { return *credential_count_; }

    fiber::tls::TlsServerConfig *engine_ = nullptr;
    std::size_t count_storage_ = 0;
    std::size_t *credential_count_ = &count_storage_;
};

// Convenience callback for static single-certificate servers. Dynamic servers
// can use the same callback contract to configure credentials and other SSL
// policy from ClientHello.
inline common::IoErr configure_tls_with_credential(void *ctx, TlsServerHandshakeConfig &config,
                                                   const TlsClientHelloView &) noexcept {
    if (!ctx) {
        return common::IoErr::Invalid;
    }
    return config.add_credential(*static_cast<const TlsCredential *>(ctx));
}

} // namespace fiber::net

#endif // FIBER_NET_TLS_SERVER_HANDSHAKE_CONFIG_H
