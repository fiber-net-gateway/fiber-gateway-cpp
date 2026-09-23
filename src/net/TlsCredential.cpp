#include <fiber/net/TlsCredential.h>

#include <cstdio>
#include <new>
#include <string>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pool.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>

namespace fiber::net {

namespace {

class OpenSslErrorQueueScope {
public:
    OpenSslErrorQueueScope() noexcept { ERR_clear_error(); }
    ~OpenSslErrorQueueScope() noexcept { ERR_clear_error(); }
};

// PEM material for one source as contiguous text. Content sources copy the
// string; File sources read the whole file (bounded sanity check included).
common::IoErr read_pem_text(const TlsPemSource &source, std::string &out) noexcept {
    switch (source.kind) {
        case TlsPemSourceKind::Content:
            if (source.value.empty() || source.value.size() > (1u << 22)) {
                return common::IoErr::Invalid;
            }
            out.assign(source.value);
            return common::IoErr::None;
        case TlsPemSourceKind::File: {
            if (source.value.empty()) {
                return common::IoErr::Invalid;
            }
            std::FILE *file = std::fopen(source.value.c_str(), "rb");
            if (file == nullptr) {
                return common::IoErr::NotFound;
            }
            char chunk[4096];
            std::size_t got = 0;
            while ((got = std::fread(chunk, 1, sizeof chunk, file)) > 0) {
                out.append(chunk, got);
                if (out.size() > (1u << 22)) {
                    break;
                }
            }
            const bool ok = std::ferror(file) == 0 && !out.empty() && out.size() <= (1u << 22);
            std::fclose(file);
            return ok ? common::IoErr::None : common::IoErr::Invalid;
        }
        case TlsPemSourceKind::None:
            return common::IoErr::Invalid;
    }
    return common::IoErr::Invalid;
}

} // namespace

TlsCredential::~TlsCredential() {
    if (ssl_bridge_ != nullptr) {
        SSL_CREDENTIAL_free(ssl_bridge_);
        ssl_bridge_ = nullptr;
    }
}

common::IoResult<std::unique_ptr<TlsCredential>> TlsCredential::create(const TlsCredentialOptions &options) noexcept {
    OpenSslErrorQueueScope error_queue_scope;
    if (options.certificate_chain.empty() || options.private_key.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::unique_ptr<TlsCredential> result(new (std::nothrow) TlsCredential());
    if (!result) {
        return std::unexpected(common::IoErr::NoMem);
    }

    std::string chain_pem;
    std::string key_pem;
    if (read_pem_text(options.certificate_chain, chain_pem) != common::IoErr::None ||
        read_pem_text(options.private_key, key_pem) != common::IoErr::None) {
        return std::unexpected(common::IoErr::Invalid);
    }
    auto chain = tls::TlsCertificateChain::parse_pem_bundle({chain_pem.data(), chain_pem.size()});
    if (!chain) {
        return std::unexpected(chain.error());
    }
    auto key = tls::TlsPrivateKey::parse_pem({key_pem.data(), key_pem.size()});
    if (!key) {
        return std::unexpected(key.error());
    }
    // Pair check the SSL path got from SSL_CREDENTIAL_set1_private_key
    // (BoringSSL rejects a key that does not match the chain); the tls
    // setters do not check, so validate here — same create-time rejection.
    auto paired = chain->leaf().matches_private_key(*key);
    if (!paired || !*paired) {
        return std::unexpected(common::IoErr::Invalid);
    }

    SHA256(chain->leaf().der().data(), chain->leaf().der().size(), result->session_identity_.data());
    result->chain_ = std::move(*chain);
    result->key_ = std::move(*key);
    return result;
}

SSL_CREDENTIAL *TlsCredential::ssl_credential() const noexcept {
    if (ssl_bridge_ != nullptr) {
        return ssl_bridge_;
    }
    SSL_CREDENTIAL *bridge = SSL_CREDENTIAL_new_x509();
    if (bridge == nullptr) {
        return nullptr;
    }
    // CRYPTO_BUFFER copies each certificate's exact DER — the same wire bytes
    // the tls chain kept — and set1_cert_chain takes its own buffer refs, so
    // the projection is independent of this object afterwards.
    CRYPTO_BUFFER *buffers[tls::TlsCertificateChain::kMaxCerts]{};
    std::size_t loaded = 0;
    bool ok = true;
    const auto append_buffer = [&buffers, &loaded, &ok](const tls::TlsCertificate &certificate) {
        if (!ok) {
            return;
        }
        const auto der = certificate.der();
        buffers[loaded] = CRYPTO_BUFFER_new(der.data(), der.size(), nullptr);
        if (buffers[loaded] == nullptr) {
            ok = false;
            return;
        }
        loaded++;
    };
    append_buffer(chain_.leaf());
    for (const auto &certificate: chain_.intermediates()) {
        append_buffer(certificate);
    }
    if (ok) {
        ok = SSL_CREDENTIAL_set1_cert_chain(bridge, buffers, chain_.size()) == 1;
    }
    for (std::size_t i = 0; i < loaded; ++i) {
        CRYPTO_BUFFER_free(buffers[i]);
    }
    if (ok) {
        ok = SSL_CREDENTIAL_set1_private_key(bridge, static_cast<EVP_PKEY *>(key_.evp_pkey_handle())) == 1;
    }
    if (!ok) {
        SSL_CREDENTIAL_free(bridge);
        return nullptr;
    }
    ssl_bridge_ = bridge;
    return bridge;
}

} // namespace fiber::net
