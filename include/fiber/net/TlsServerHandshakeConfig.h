#ifndef FIBER_NET_TLS_SERVER_HANDSHAKE_CONFIG_H
#define FIBER_NET_TLS_SERVER_HANDSHAKE_CONFIG_H

#include <cstddef>
#include <span>

#include "../common/IoError.h"
#include "TlsParams.h"

struct ssl_st;
typedef struct ssl_st SSL;

namespace fiber::net {

class TlsCredential;
class TrustStore;
namespace detail {
class TlsRuntime;
class TlsSslFactory;
} // namespace detail

// A synchronous, callback-duration view for configuring the current server
// handshake. It does not own or expose the underlying SSL object.
class TlsServerHandshakeConfig {
public:
    [[nodiscard]] common::IoErr clear_credentials() noexcept;
    // On success, the current SSL retains its own BoringSSL reference.
    [[nodiscard]] common::IoErr add_credential(const TlsCredential &credential) noexcept;
    // On success, the current SSL retains its own X509_STORE reference.
    [[nodiscard]] common::IoErr set_trust_store(const TrustStore &trust_store) noexcept;
    [[nodiscard]] common::IoErr set_session_id_context(std::span<const std::uint8_t> context) noexcept;
    [[nodiscard]] common::IoErr set_protocol_versions(int min_version, int max_version) noexcept;
    // Client-certificate verification needs a trust store: install one via
    // TlsServerParam::trust_store or set_trust_store before requesting a mode
    // other than None, otherwise the handshake fails verification.
    [[nodiscard]] common::IoErr set_client_certificate_mode(TlsClientCertificateMode mode) noexcept;
    [[nodiscard]] common::IoErr set_early_data_enabled(bool enabled) noexcept;

private:
    friend class detail::TlsRuntime;
    friend class detail::TlsSslFactory;

    TlsServerHandshakeConfig(SSL *ssl) noexcept : ssl_(ssl) {}

    [[nodiscard]] std::size_t credential_count() const noexcept { return credential_count_; }

    SSL *ssl_;
    std::size_t credential_count_ = 0;
    bool session_id_context_set_ = false;
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
