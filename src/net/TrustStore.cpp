#include <fiber/net/TrustStore.h>

#include <cstdlib>
#include <limits>
#include <new>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

namespace fiber::net {

namespace {

class OpenSslErrorQueueScope {
public:
    OpenSslErrorQueueScope() noexcept { ERR_clear_error(); }
    ~OpenSslErrorQueueScope() noexcept { ERR_clear_error(); }
};

common::IoErr load_content(X509_STORE *store, const std::string &pem) noexcept {
    if (pem.empty() || pem.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return common::IoErr::Invalid;
    }
    BIO *bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) {
        return common::IoErr::NoMem;
    }
    STACK_OF(X509_INFO) *infos = PEM_X509_INFO_read_bio(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!infos) {
        return common::IoErr::Invalid;
    }

    bool loaded_any = false;
    bool valid = true;
    const std::size_t count = sk_X509_INFO_num(infos);
    for (std::size_t i = 0; i < count; ++i) {
        X509_INFO *info = sk_X509_INFO_value(infos, i);
        if (!info || !info->x509 || info->crl || info->x_pkey || X509_STORE_add_cert(store, info->x509) != 1) {
            valid = false;
            break;
        }
        loaded_any = true;
    }
    sk_X509_INFO_pop_free(infos, X509_INFO_free);
    return valid && loaded_any ? common::IoErr::None : common::IoErr::Invalid;
}

} // namespace

TrustStore::~TrustStore() {
    X509_STORE_free(store_);
    store_ = nullptr;
}

common::IoResult<std::unique_ptr<TrustStore>> TrustStore::create(const TrustStoreOptions &options) noexcept {
    OpenSslErrorQueueScope error_queue_scope;
    if ((options.kind == TrustStoreSourceKind::System && !options.value.empty()) ||
        (options.kind != TrustStoreSourceKind::System && options.value.empty())) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::unique_ptr<TrustStore> result(new (std::nothrow) TrustStore());
    if (!result) {
        return std::unexpected(common::IoErr::NoMem);
    }
    result->store_ = X509_STORE_new();
    if (!result->store_) {
        return std::unexpected(common::IoErr::NoMem);
    }

    common::IoErr error = common::IoErr::None;
    switch (options.kind) {
        case TrustStoreSourceKind::System: {
            const std::string &path = system_ca_bundle_path();
            const int loaded = path.empty() ? X509_STORE_set_default_paths(result->store_)
                                            : X509_STORE_load_locations(result->store_, path.c_str(), nullptr);
            error = loaded == 1 ? common::IoErr::None : common::IoErr::Invalid;
            break;
        }
        case TrustStoreSourceKind::File:
            error = X509_STORE_load_locations(result->store_, options.value.c_str(), nullptr) == 1
                            ? common::IoErr::None
                            : common::IoErr::Invalid;
            break;
        case TrustStoreSourceKind::Content:
            error = load_content(result->store_, options.value);
            break;
    }
    if (error != common::IoErr::None) {
        return std::unexpected(error);
    }
    return result;
}

const std::string &TrustStore::system_ca_bundle_path() noexcept {
    static const std::string cached = []() -> std::string {
        if (const char *env = std::getenv("SSL_CERT_FILE")) {
            if (env[0] != '\0' && ::access(env, R_OK) == 0) {
                return std::string(env);
            }
        }
        static constexpr const char *kCandidates[] = {
                "/etc/ssl/certs/ca-certificates.crt",     "/etc/pki/tls/cert.pem",
                "/etc/ssl/certs/ca-bundle.crt",           "/etc/ssl/cert.pem",
                "/usr/local/share/certs/ca-root-nss.crt", "/etc/openssl/certs/ca-certificates.crt",
        };
        for (const char *candidate: kCandidates) {
            if (::access(candidate, R_OK) == 0) {
                return std::string(candidate);
            }
        }
        return {};
    }();
    return cached;
}

common::IoResult<const TrustStore *> TrustStore::system_default() noexcept {
    struct SystemStoreHolder {
        const TrustStore *store = nullptr;
        common::IoErr error = common::IoErr::None;

        SystemStoreHolder() noexcept {
            auto created = TrustStore::create(TrustStoreOptions::system());
            if (created) {
                store = created->release();
            } else {
                error = created.error();
            }
        }
    };
    // Function-local static init is thread-safe; the holder caches both the
    // resolved store and the failure so neither the filesystem nor the PEM
    // parser runs more than once per process.
    static const SystemStoreHolder holder{};
    if (holder.store == nullptr) {
        return std::unexpected(holder.error);
    }
    return holder.store;
}

} // namespace fiber::net
