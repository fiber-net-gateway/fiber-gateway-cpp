#include <fiber/net/TlsCredential.h>

#include <cstddef>
#include <limits>
#include <new>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/mem.h>
#include <openssl/pem.h>
#include <openssl/pool.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace fiber::net {

namespace {

class OpenSslErrorQueueScope {
public:
    OpenSslErrorQueueScope() noexcept { ERR_clear_error(); }
    ~OpenSslErrorQueueScope() noexcept { ERR_clear_error(); }
};

BIO *open_pem_bio(const TlsPemSource &source) noexcept {
    switch (source.kind) {
        case TlsPemSourceKind::File:
            return source.value.empty() ? nullptr : BIO_new_file(source.value.c_str(), "rb");
        case TlsPemSourceKind::Content:
            if (source.value.empty() ||
                source.value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                return nullptr;
            }
            return BIO_new_mem_buf(source.value.data(), static_cast<int>(source.value.size()));
        case TlsPemSourceKind::None:
            return nullptr;
    }
    return nullptr;
}

void free_crypto_buffers(CRYPTO_BUFFER **buffers, std::size_t count) noexcept {
    if (!buffers) {
        return;
    }
    for (std::size_t i = 0; i < count; ++i) {
        CRYPTO_BUFFER_free(buffers[i]);
    }
    OPENSSL_free(buffers);
}

common::IoErr load_certificate_chain(SSL_CREDENTIAL *credential, const TlsPemSource &source,
                                     std::array<std::uint8_t, 32> &session_identity) noexcept {
    BIO *bio = open_pem_bio(source);
    if (!bio) {
        return common::IoErr::Invalid;
    }
    STACK_OF(X509_INFO) *infos = PEM_X509_INFO_read_bio(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!infos) {
        return common::IoErr::Invalid;
    }

    const std::size_t count = sk_X509_INFO_num(infos);
    if (count == 0 || count > std::numeric_limits<std::size_t>::max() / sizeof(CRYPTO_BUFFER *)) {
        sk_X509_INFO_pop_free(infos, X509_INFO_free);
        return common::IoErr::Invalid;
    }
    auto **buffers = static_cast<CRYPTO_BUFFER **>(OPENSSL_zalloc(count * sizeof(CRYPTO_BUFFER *)));
    if (!buffers) {
        sk_X509_INFO_pop_free(infos, X509_INFO_free);
        return common::IoErr::NoMem;
    }

    common::IoErr error = common::IoErr::None;
    std::size_t loaded = 0;
    for (; loaded < count; ++loaded) {
        X509_INFO *info = sk_X509_INFO_value(infos, loaded);
        if (!info || !info->x509 || info->crl || info->x_pkey) {
            error = common::IoErr::Invalid;
            break;
        }
        const int der_len = i2d_X509(info->x509, nullptr);
        if (der_len <= 0) {
            error = common::IoErr::Invalid;
            break;
        }
        auto *der = static_cast<std::uint8_t *>(OPENSSL_malloc(static_cast<std::size_t>(der_len)));
        if (!der) {
            error = common::IoErr::NoMem;
            break;
        }
        std::uint8_t *cursor = der;
        if (i2d_X509(info->x509, &cursor) != der_len) {
            OPENSSL_free(der);
            error = common::IoErr::Invalid;
            break;
        }
        buffers[loaded] = CRYPTO_BUFFER_new(der, static_cast<std::size_t>(der_len), nullptr);
        if (loaded == 0) {
            SHA256(der, static_cast<std::size_t>(der_len), session_identity.data());
        }
        OPENSSL_free(der);
        if (!buffers[loaded]) {
            error = common::IoErr::NoMem;
            break;
        }
    }

    if (error == common::IoErr::None && SSL_CREDENTIAL_set1_cert_chain(credential, buffers, count) != 1) {
        error = common::IoErr::Invalid;
    }
    free_crypto_buffers(buffers, loaded);
    sk_X509_INFO_pop_free(infos, X509_INFO_free);
    return error;
}

common::IoErr load_private_key(SSL_CREDENTIAL *credential, const TlsPemSource &source) noexcept {
    BIO *bio = open_pem_bio(source);
    if (!bio) {
        return common::IoErr::Invalid;
    }
    EVP_PKEY *key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!key) {
        return common::IoErr::Invalid;
    }
    const bool ok = SSL_CREDENTIAL_set1_private_key(credential, key) == 1;
    EVP_PKEY_free(key);
    return ok ? common::IoErr::None : common::IoErr::Invalid;
}

} // namespace

TlsCredential::~TlsCredential() {
    SSL_CREDENTIAL_free(credential_);
    credential_ = nullptr;
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
    result->credential_ = SSL_CREDENTIAL_new_x509();
    if (!result->credential_) {
        return std::unexpected(common::IoErr::NoMem);
    }
    common::IoErr error =
            load_certificate_chain(result->credential_, options.certificate_chain, result->session_identity_);
    if (error == common::IoErr::None) {
        error = load_private_key(result->credential_, options.private_key);
    }
    if (error != common::IoErr::None) {
        return std::unexpected(error);
    }
    return result;
}

} // namespace fiber::net
