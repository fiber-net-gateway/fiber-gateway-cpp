#ifndef FIBER_HTTP_TLS_CONTEXT_H
#define FIBER_HTTP_TLS_CONTEXT_H

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "TlsOptions.h"

struct ssl_ctx_st;
typedef struct ssl_ctx_st SSL_CTX;
struct ssl_st;
typedef struct ssl_st SSL;
struct ssl_early_callback_ctx;
typedef struct ssl_early_callback_ctx SSL_CLIENT_HELLO;

namespace fiber::net {
class SocketAddress;
}

namespace fiber::http {

class TlsContext : public common::NonCopyable, public common::NonMovable {
public:
    explicit TlsContext(TlsOptions options, bool is_server = true, bool require_server_identity = true);
    ~TlsContext();

    common::IoResult<void> init();

    [[nodiscard]] SSL_CTX *raw() const noexcept { return ctx_; }
    [[nodiscard]] const TlsOptions &options() const noexcept { return options_; }
    [[nodiscard]] const std::vector<std::string> &alpn() const noexcept { return alpn_; }
    [[nodiscard]] bool is_server() const noexcept { return is_server_; }
    [[nodiscard]] bool require_server_identity() const noexcept { return require_server_identity_; }

private:
    static int alpn_select_cb(SSL *ssl, const unsigned char **out, unsigned char *outlen, const unsigned char *in,
                              unsigned int inlen, void *arg);

    SSL_CTX *ctx_ = nullptr;
    TlsOptions options_{};
    std::vector<std::string> alpn_{};
    std::vector<unsigned char> alpn_wire_{};
    bool is_server_ = true;
    bool require_server_identity_ = true;
};

class TlsServerContext : public common::NonCopyable, public common::NonMovable {
public:
    explicit TlsServerContext(TlsOptions options);
    ~TlsServerContext();

    common::IoResult<void> init();

    [[nodiscard]] SSL_CTX *raw() const noexcept;
    common::IoResult<void> bind_ssl(SSL *ssl, const net::SocketAddress *remote_addr) noexcept;
    [[nodiscard]] TlsContext *select_identity(const TlsClientHelloView &client_hello) const noexcept;
    [[nodiscard]] TlsContext *find_identity_by_name(std::string_view name) const noexcept;
    [[nodiscard]] TlsContext *default_identity() const noexcept { return default_identity_.get(); }

private:
    struct IdentityEntry {
        std::unique_ptr<TlsContext> context{};
        std::string_view name{};
    };

    [[nodiscard]] common::IoResult<void> init_base_context();
    [[nodiscard]] common::IoResult<void> init_default_identity();
    [[nodiscard]] common::IoResult<void> init_named_identities();
    [[nodiscard]] common::IoResult<void> validate_identity_options() const;

    TlsOptions options_{};
    TlsIdentitySelectorOps identity_selector_{};
    std::unique_ptr<TlsContext> base_context_{};
    std::unique_ptr<TlsContext> default_identity_{};
    std::unique_ptr<IdentityEntry[]> identities_{};
    std::size_t identity_count_ = 0;
};

} // namespace fiber::http

#endif // FIBER_HTTP_TLS_CONTEXT_H
