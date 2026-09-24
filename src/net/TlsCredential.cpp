#include <fiber/net/TlsCredential.h>

#include <cstdio>
#include <new>
#include <string>

namespace fiber::net {

namespace {

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

common::IoResult<std::unique_ptr<TlsCredential>> TlsCredential::create(const TlsCredentialOptions &options) noexcept {
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
    // The tls setters do not check key/chain pairing, so validate here —
    // BoringSSL's SSL_CREDENTIAL_set1_private_key rejects a key that does not
    // match the chain; same create-time rejection.
    auto paired = chain->leaf().matches_private_key(*key);
    if (!paired || !*paired) {
        return std::unexpected(common::IoErr::Invalid);
    }

    result->chain_ = std::move(*chain);
    result->key_ = std::move(*key);
    return result;
}

} // namespace fiber::net
