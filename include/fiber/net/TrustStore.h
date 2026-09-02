#ifndef FIBER_NET_TRUST_STORE_H
#define FIBER_NET_TRUST_STORE_H

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"

struct x509_store_st;
typedef struct x509_store_st X509_STORE;

namespace fiber::net {

class TlsServerHandshakeConfig;
namespace detail {
class TlsSslFactory;
}

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

// Immutable, reusable peer trust anchors. Peer-verification policy remains per
// connection and is not a property of this object. Once installed with a set1
// API, the SSL retains its own reference to the underlying store.
class TrustStore : public common::NonCopyable, public common::NonMovable {
public:
    ~TrustStore();

    [[nodiscard]] static common::IoResult<std::unique_ptr<TrustStore>>
    create(const TrustStoreOptions &options = {}) noexcept;

    [[nodiscard]] static const std::string &system_ca_bundle_path() noexcept;

private:
    friend class TlsServerHandshakeConfig;
    friend class detail::TlsSslFactory;

    TrustStore() noexcept = default;

    X509_STORE *store_ = nullptr;
};

} // namespace fiber::net

#endif // FIBER_NET_TRUST_STORE_H
