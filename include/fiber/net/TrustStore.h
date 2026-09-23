#ifndef FIBER_NET_TRUST_STORE_H
#define FIBER_NET_TRUST_STORE_H

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../tls/crypto/TlsCertificate.h"

struct x509_store_st;
typedef struct x509_store_st X509_STORE;

namespace fiber::net {

class TlsServerHandshakeConfig;
namespace detail {
class TlsSslFactory;
class TlsStreamFd;
} // namespace detail

enum class TrustStoreSourceKind : std::uint8_t {
    System,
    File,
    Content,
};

struct TrustStoreOptions {
    TrustStoreSourceKind kind = TrustStoreSourceKind::System;
    std::string value{};

    [[nodiscard]] static TrustStoreOptions system() noexcept { return {}; }

    [[nodiscard]] static TrustStoreOptions from_file(std::string path) {
        return {.kind = TrustStoreSourceKind::File, .value = std::move(path)};
    }

    [[nodiscard]] static TrustStoreOptions from_content(std::string pem) {
        return {.kind = TrustStoreSourceKind::Content, .value = std::move(pem)};
    }
};

// Immutable, reusable peer trust anchors (09 §4.3): the anchors live in the
// tls layer's TlsTrustStore. Peer-verification policy remains per connection
// and is not a property of this object. System stores borrow the tls layer's
// process-wide cache (never destroyed, like the pre-09 holder); File/Content
// stores own theirs.
class TrustStore : public common::NonCopyable, public common::NonMovable {
public:
    ~TrustStore();

    [[nodiscard]] static common::IoResult<std::unique_ptr<TrustStore>>
    create(const TrustStoreOptions &options = {}) noexcept;

    [[nodiscard]] static const std::string &system_ca_bundle_path() noexcept;

    // Process-wide system trust store, resolved on first use and cached for
    // the lifetime of the process — including a failed resolution, so systems
    // without a CA bundle do not re-probe the filesystem per connection. The
    // store is intentionally never destroyed; the backing tls cache lives
    // forever regardless.
    [[nodiscard]] static common::IoResult<const TrustStore *> system_default() noexcept;

private:
    friend class TlsServerHandshakeConfig;
    friend class detail::TlsSslFactory;
    friend class detail::TlsStreamFd;

    TrustStore() noexcept = default;

    // The anchor set for both paths.
    [[nodiscard]] const tls::TlsTrustStore &tls_store() const noexcept {
        return shared_ != nullptr ? *shared_ : owned_;
    }
    // Borrowed X509_STORE* for the QUIC-side BoringSSL glue. Null when the
    // store is empty (never on an object create() returned successfully).
    [[nodiscard]] X509_STORE *x509_store() const noexcept;

    tls::TlsTrustStore owned_{};
    const tls::TlsTrustStore *shared_ = nullptr;
};

} // namespace fiber::net

#endif // FIBER_NET_TRUST_STORE_H
