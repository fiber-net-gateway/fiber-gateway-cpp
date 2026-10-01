#include <fiber/net/TlsServerHandshakeConfig.h>

#include <fiber/net/TlsCredential.h>
#include <fiber/net/TrustStore.h>
#include <fiber/tls/TlsConfig.h>

namespace fiber::net {

// BoringSSL's SSL_MAX_SID_CTX_LENGTH, kept as a literal so the portability
// validation below needs no ssl.h include (10 §10: fiber_lib is crypto-only).
constexpr std::size_t kMaxSidCtxLength = 32;

common::IoErr TlsServerHandshakeConfig::clear_credentials() noexcept {
    engine_->chain = nullptr;
    engine_->key = nullptr;
    *credential_count_ = 0;
    credential_owner_->reset();
    return common::IoErr::None;
}

void TlsServerHandshakeConfig::stage(const TlsCredential &credential) noexcept {
    engine_->chain = &credential.tls_chain();
    engine_->key = &credential.tls_key();
    ++*credential_count_;
}

common::IoErr TlsServerHandshakeConfig::add_credential(TlsCredential credential) noexcept {
    if (credential.empty()) {
        return common::IoErr::Invalid;
    }
    // Staged by pointer into the shared material, kept alive by the
    // handshake's owner slot for as long as the engine may still read it (HRR
    // defers Certificate/CertificateVerify past the callback by a full round
    // trip). Moving the handle into the slot leaves those pointers intact.
    stage(credential);
    *credential_owner_ = std::move(credential);
    return common::IoErr::None;
}

common::IoErr TlsServerHandshakeConfig::add_borrowed_credential(const TlsCredential &credential) noexcept {
    if (credential.empty()) {
        return common::IoErr::Invalid;
    }
    // Staged by pointer: the caller keeps the material alive for the
    // handshake (the documented param contract — server options hold their
    // material). The single engine slot now points here, so an earlier owner
    // is dead weight.
    stage(credential);
    credential_owner_->reset();
    return common::IoErr::None;
}

common::IoErr TlsServerHandshakeConfig::set_trust_store(const TrustStore &trust_store) noexcept {
    // Engine semantics: a non-null client_trust REQUESTS client certificates
    // (optional unless require_client_cert).
    engine_->client_trust = &trust_store.tls_store();
    return common::IoErr::None;
}

common::IoErr TlsServerHandshakeConfig::set_session_id_context(std::span<const std::uint8_t> context) noexcept {
    if (context.empty() || context.size() > kMaxSidCtxLength) {
        return common::IoErr::Invalid;
    }
    // Nothing to scope — accepted for callback portability.
    return common::IoErr::None;
}

common::IoErr TlsServerHandshakeConfig::set_protocol_versions(int min_version, int max_version) noexcept {
    const auto in_domain = [](int version) noexcept { return version == 0 || version == 0x0303 || version == 0x0304; };
    if (min_version > max_version || !in_domain(min_version) || !in_domain(max_version)) {
        return common::IoErr::Invalid;
    }
    if (min_version > 0) {
        engine_->min_version = static_cast<std::uint16_t>(min_version);
    }
    if (max_version > 0) {
        engine_->max_version = static_cast<std::uint16_t>(max_version);
    }
    return common::IoErr::None;
}

common::IoErr TlsServerHandshakeConfig::set_client_certificate_mode(TlsClientCertificateMode mode) noexcept {
    // The request itself is implied by a non-null client_trust; the mode only
    // decides whether an empty client chain is fatal.
    engine_->require_client_cert = mode == TlsClientCertificateMode::Required;
    return common::IoErr::None;
}

common::IoErr TlsServerHandshakeConfig::set_early_data_enabled(bool enabled) noexcept {
    engine_->enable_early_data = enabled;
    return common::IoErr::None;
}

} // namespace fiber::net
