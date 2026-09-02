#ifndef FIBER_NET_TLS_SERVER_HANDSHAKE_CONFIG_H
#define FIBER_NET_TLS_SERVER_HANDSHAKE_CONFIG_H

#include <cstdint>
#include <span>
#include <string_view>

#include "../common/IoError.h"
#include "TlsParams.h"

struct ssl_st;
typedef struct ssl_st SSL;

namespace fiber::net {

class TlsCredential;
class TrustStore;
namespace detail {
class TlsRuntime;
struct TlsServerHandshakeState;
} // namespace detail

// A synchronous, callback-duration view for configuring the current server
// handshake. It does not own or expose the underlying SSL object.
class TlsServerHandshakeConfig {
public:
    [[nodiscard]] common::IoErr clear_credentials() noexcept;
    [[nodiscard]] common::IoErr add_credential(const TlsCredential &credential) noexcept;
    [[nodiscard]] common::IoErr set_trust_store(const TrustStore &trust_store) noexcept;
    [[nodiscard]] common::IoErr set_session_id_context(std::span<const std::uint8_t> context) noexcept;
    [[nodiscard]] common::IoErr set_protocol_versions(int min_version, int max_version) noexcept;
    [[nodiscard]] common::IoErr set_client_certificate_mode(TlsClientCertificateMode mode) noexcept;
    [[nodiscard]] common::IoErr set_early_data_enabled(bool enabled) noexcept;
    [[nodiscard]] common::IoErr select_alpn(std::string_view protocol) noexcept;

private:
    friend class detail::TlsRuntime;

    TlsServerHandshakeConfig(SSL *ssl, detail::TlsServerHandshakeState &state) noexcept : ssl_(ssl), state_(state) {}

    SSL *ssl_;
    detail::TlsServerHandshakeState &state_;
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
