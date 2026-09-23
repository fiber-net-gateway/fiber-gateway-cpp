#include <fiber/net/TlsServerHandshakeConfig.h>

#include <openssl/ssl.h>

#include <fiber/net/TlsCredential.h>
#include <fiber/net/TrustStore.h>
#include <fiber/tls/TlsConfig.h>

namespace fiber::net {

common::IoErr TlsServerHandshakeConfig::clear_credentials() noexcept {
    if (ssl_ != nullptr) {
        SSL_certs_clear(ssl_);
    } else if (engine_ != nullptr) {
        engine_->chain = nullptr;
        engine_->key = nullptr;
    } else {
        return common::IoErr::Invalid;
    }
    *credential_count_ = 0;
    session_id_context_set_ = false;
    return common::IoErr::None;
}

common::IoErr TlsServerHandshakeConfig::add_credential(const TlsCredential &credential) noexcept {
    if (ssl_ != nullptr) {
        SSL_CREDENTIAL *bridge = credential.ssl_credential();
        if (bridge == nullptr || SSL_add1_credential(ssl_, bridge) != 1) {
            return common::IoErr::Invalid;
        }
        ++*credential_count_;
        if (!session_id_context_set_ && SSL_set_session_id_context(ssl_, credential.session_identity_.data(),
                                                                   credential.session_identity_.size()) != 1) {
            return common::IoErr::Invalid;
        }
        session_id_context_set_ = true;
        return common::IoErr::None;
    }
    if (engine_ != nullptr) {
        if (credential.tls_chain().empty() || credential.tls_key().empty()) {
            return common::IoErr::Invalid;
        }
        // Staged by pointer: the credential must outlive the handshake (the
        // documented param contract — server options hold their material).
        engine_->chain = &credential.tls_chain();
        engine_->key = &credential.tls_key();
        ++*credential_count_;
        return common::IoErr::None;
    }
    return common::IoErr::Invalid;
}

common::IoErr TlsServerHandshakeConfig::set_trust_store(const TrustStore &trust_store) noexcept {
    if (ssl_ != nullptr) {
        if (SSL_set1_verify_cert_store(ssl_, trust_store.x509_store()) != 1) {
            return common::IoErr::Invalid;
        }
        return common::IoErr::None;
    }
    if (engine_ != nullptr) {
        // Engine semantics: a non-null client_trust REQUESTS client
        // certificates (optional unless require_client_cert).
        engine_->client_trust = &trust_store.tls_store();
        return common::IoErr::None;
    }
    return common::IoErr::Invalid;
}

common::IoErr TlsServerHandshakeConfig::set_session_id_context(std::span<const std::uint8_t> context) noexcept {
    if (context.empty() || context.size() > SSL_MAX_SID_CTX_LENGTH) {
        return common::IoErr::Invalid;
    }
    if (ssl_ != nullptr && SSL_set_session_id_context(ssl_, context.data(), context.size()) != 1) {
        return common::IoErr::Invalid;
    }
    // Engine mode: nothing to scope — accepted for callback portability.
    session_id_context_set_ = true;
    return common::IoErr::None;
}

common::IoErr TlsServerHandshakeConfig::set_protocol_versions(int min_version, int max_version) noexcept {
    const auto in_domain = [](int version) noexcept { return version == 0 || version == 0x0303 || version == 0x0304; };
    if (min_version > max_version || !in_domain(min_version) || !in_domain(max_version)) {
        return common::IoErr::Invalid;
    }
    if (ssl_ != nullptr) {
        if ((min_version > 0 && SSL_set_min_proto_version(ssl_, static_cast<std::uint16_t>(min_version)) != 1) ||
            (max_version > 0 && SSL_set_max_proto_version(ssl_, static_cast<std::uint16_t>(max_version)) != 1)) {
            return common::IoErr::Invalid;
        }
        return common::IoErr::None;
    }
    if (engine_ != nullptr) {
        if (min_version > 0) {
            engine_->min_version = static_cast<std::uint16_t>(min_version);
        }
        if (max_version > 0) {
            engine_->max_version = static_cast<std::uint16_t>(max_version);
        }
        return common::IoErr::None;
    }
    return common::IoErr::Invalid;
}

common::IoErr TlsServerHandshakeConfig::set_client_certificate_mode(TlsClientCertificateMode mode) noexcept {
    if (ssl_ != nullptr) {
        int verify_mode = SSL_VERIFY_NONE;
        if (mode != TlsClientCertificateMode::None) {
            verify_mode = SSL_VERIFY_PEER;
            if (mode == TlsClientCertificateMode::Required) {
                verify_mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
            }
        }
        SSL_set_verify(ssl_, verify_mode, nullptr);
        return common::IoErr::None;
    }
    if (engine_ != nullptr) {
        // The request itself is implied by a non-null client_trust; the mode
        // only decides whether an empty client chain is fatal.
        engine_->require_client_cert = mode == TlsClientCertificateMode::Required;
        return common::IoErr::None;
    }
    return common::IoErr::Invalid;
}

common::IoErr TlsServerHandshakeConfig::set_early_data_enabled(bool enabled) noexcept {
    if (ssl_ != nullptr) {
        SSL_set_early_data_enabled(ssl_, enabled ? 1 : 0);
        return common::IoErr::None;
    }
    if (engine_ != nullptr) {
        engine_->enable_early_data = enabled;
        return common::IoErr::None;
    }
    return common::IoErr::Invalid;
}

} // namespace fiber::net
