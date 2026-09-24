#include <fiber/net/TrustStore.h>

#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <unistd.h>

namespace fiber::net {

namespace {

// Reads a File-kind PEM bundle (bounded sanity check; at least one readable
// byte required — the tls parser then demands at least one CERTIFICATE
// block, matching the old loader's loaded_any rule).
common::IoErr read_file_bundle(const std::string &path, std::string &out) noexcept {
    if (path.empty()) {
        return common::IoErr::Invalid;
    }
    std::FILE *file = std::fopen(path.c_str(), "rb");
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

} // namespace

TrustStore::~TrustStore() = default;

common::IoResult<std::unique_ptr<TrustStore>> TrustStore::create(const TrustStoreOptions &options) noexcept {
    if ((options.kind == TrustStoreSourceKind::System && !options.value.empty()) ||
        (options.kind != TrustStoreSourceKind::System && options.value.empty())) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::unique_ptr<TrustStore> result(new (std::nothrow) TrustStore());
    if (!result) {
        return std::unexpected(common::IoErr::NoMem);
    }

    switch (options.kind) {
        case TrustStoreSourceKind::System:
            // Borrow the tls layer's process-wide cache: env override →
            // distribution bundles → OpenSSL default paths, resolved once,
            // failures cached. A null result means this host has no usable
            // system roots at all.
            result->shared_ = tls::TlsTrustStore::system_default();
            if (result->shared_ == nullptr) {
                return std::unexpected(common::IoErr::NotFound);
            }
            return result;
        case TrustStoreSourceKind::File: {
            std::string pem;
            if (read_file_bundle(options.value, pem) != common::IoErr::None) {
                return std::unexpected(common::IoErr::Invalid);
            }
            auto store = tls::TlsTrustStore::from_pem_bundle({pem.data(), pem.size()});
            if (!store) {
                return std::unexpected(store.error());
            }
            result->owned_ = std::move(*store);
            return result;
        }
        case TrustStoreSourceKind::Content: {
            if (options.value.empty() || options.value.size() > (1u << 22)) {
                return std::unexpected(common::IoErr::Invalid);
            }
            auto store = tls::TlsTrustStore::from_pem_bundle({options.value.data(), options.value.size()});
            if (!store) {
                return std::unexpected(store.error());
            }
            result->owned_ = std::move(*store);
            return result;
        }
    }
    return std::unexpected(common::IoErr::Invalid);
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
            TrustStore *created = new (std::nothrow) TrustStore();
            if (created == nullptr) {
                error = common::IoErr::NoMem;
                return;
            }
            created->shared_ = tls::TlsTrustStore::system_default();
            if (created->shared_ == nullptr) {
                error = common::IoErr::NotFound;
            }
            store = created; // never destroyed, success or failure
        }
    };
    // Function-local static init is thread-safe; the holder caches both the
    // resolved store and the failure so neither the filesystem nor the PEM
    // parser runs more than once per process.
    static const SystemStoreHolder holder{};
    if (holder.store == nullptr || holder.error != common::IoErr::None) {
        return std::unexpected(holder.error);
    }
    return holder.store;
}

} // namespace fiber::net
