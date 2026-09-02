#include <fiber/net/TlsServerHandshakeConfig.h>

#include <cstring>
#include <limits>

#include <openssl/ssl.h>

#include <fiber/net/TlsCredential.h>
#include <fiber/net/TrustStore.h>
#include <fiber/net/detail/TlsHandshakeState.h>

namespace fiber::net {

common::IoErr TlsServerHandshakeConfig::clear_credentials() noexcept {
    if (!ssl_) {
        return common::IoErr::Invalid;
    }
    SSL_certs_clear(ssl_);
    state_.credential_count = 0;
    state_.session_id_context_set = false;
    return common::IoErr::None;
}

common::IoErr TlsServerHandshakeConfig::add_credential(const TlsCredential &credential) noexcept {
    if (!ssl_ || !credential.credential_ || SSL_add1_credential(ssl_, credential.credential_) != 1) {
        return common::IoErr::Invalid;
    }
    ++state_.credential_count;
    if (!state_.session_id_context_set && SSL_set_session_id_context(ssl_, credential.session_identity_.data(),
                                                                     credential.session_identity_.size()) != 1) {
        return common::IoErr::Invalid;
    }
    state_.session_id_context_set = true;
    return common::IoErr::None;
}

common::IoErr TlsServerHandshakeConfig::set_trust_store(const TrustStore &trust_store) noexcept {
    if (!ssl_ || !trust_store.store_ || SSL_set1_verify_cert_store(ssl_, trust_store.store_) != 1) {
        return common::IoErr::Invalid;
    }
    state_.trust_store_set = true;
    return common::IoErr::None;
}

common::IoErr TlsServerHandshakeConfig::set_session_id_context(std::span<const std::uint8_t> context) noexcept {
    if (!ssl_ || context.empty() || context.size() > SSL_MAX_SID_CTX_LENGTH ||
        SSL_set_session_id_context(ssl_, context.data(), context.size()) != 1) {
        return common::IoErr::Invalid;
    }
    state_.session_id_context_set = true;
    return common::IoErr::None;
}

common::IoErr TlsServerHandshakeConfig::set_protocol_versions(int min_version, int max_version) noexcept {
    if (!ssl_ || (min_version > 0 && SSL_set_min_proto_version(ssl_, static_cast<std::uint16_t>(min_version)) != 1) ||
        (max_version > 0 && SSL_set_max_proto_version(ssl_, static_cast<std::uint16_t>(max_version)) != 1)) {
        return common::IoErr::Invalid;
    }
    return common::IoErr::None;
}

common::IoErr TlsServerHandshakeConfig::set_client_certificate_mode(TlsClientCertificateMode mode) noexcept {
    if (!ssl_) {
        return common::IoErr::Invalid;
    }
    int verify_mode = SSL_VERIFY_NONE;
    if (mode != TlsClientCertificateMode::None) {
        verify_mode = SSL_VERIFY_PEER;
        if (mode == TlsClientCertificateMode::Required) {
            verify_mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
        }
    }
    SSL_set_verify(ssl_, verify_mode, nullptr);
    state_.client_certificate_mode = mode;
    return common::IoErr::None;
}

common::IoErr TlsServerHandshakeConfig::set_early_data_enabled(bool enabled) noexcept {
    if (!ssl_) {
        return common::IoErr::Invalid;
    }
    SSL_set_early_data_enabled(ssl_, enabled ? 1 : 0);
    return common::IoErr::None;
}

common::IoErr TlsServerHandshakeConfig::select_alpn(std::string_view protocol) noexcept {
    if (!ssl_ || protocol.empty() || protocol.size() > state_.selected_alpn.size()) {
        return common::IoErr::Invalid;
    }
    std::memcpy(state_.selected_alpn.data(), protocol.data(), protocol.size());
    state_.selected_alpn_size = static_cast<std::uint8_t>(protocol.size());
    return common::IoErr::None;
}

} // namespace fiber::net
