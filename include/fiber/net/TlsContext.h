#ifndef FIBER_NET_TLS_CONTEXT_H
#define FIBER_NET_TLS_CONTEXT_H

#include <memory>
#include <string>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "TlsConnectionOptions.h"
#include "TlsOptions.h"

struct ssl_ctx_st;
typedef struct ssl_ctx_st SSL_CTX;
struct ssl_st;
typedef struct ssl_st SSL;

namespace fiber::net {

class SocketAddress;

// An immutable, role-neutral certificate identity and trust store. A context
// may create both client and server SSL connections and must outlive them.
class TlsContext : public common::NonCopyable, public common::NonMovable {
public:
    ~TlsContext();

    [[nodiscard]] static common::IoResult<std::unique_ptr<TlsContext>> create(const TlsOptions &options) noexcept;

    [[nodiscard]] common::IoResult<SSL *> create_client_ssl(const TlsClientConnectionOptions &options) const noexcept;
    [[nodiscard]] static common::IoResult<SSL *> create_server_ssl(const TlsServerConnectionOptions &options,
                                                                   const SocketAddress *local_addr,
                                                                   const SocketAddress *remote_addr,
                                                                   TlsTransportKind transport) noexcept;

    [[nodiscard]] bool has_identity() const noexcept { return has_identity_; }
    [[nodiscard]] bool has_trust_store() const noexcept { return has_trust_store_; }

    // Native escape hatch for interoperability and white-box tests. Callers
    // must not mutate the handle after create() publishes the context.
    [[nodiscard]] SSL_CTX *raw() const noexcept { return ctx_; }

    // Resolves a system CA bundle path once per process. Empty means the TLS
    // library's default verify paths must be used.
    [[nodiscard]] static const std::string &system_ca_bundle_path() noexcept;

private:
    TlsContext() noexcept = default;

    SSL_CTX *ctx_ = nullptr;
    bool has_identity_ = false;
    bool has_trust_store_ = false;
};

} // namespace fiber::net

#endif // FIBER_NET_TLS_CONTEXT_H
