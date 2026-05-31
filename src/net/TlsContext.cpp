#include "TlsContext.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include <openssl/ssl.h>
#include <openssl/tls1.h>

#include "SocketAddress.h"

namespace fiber::net {

namespace {

constexpr std::uint16_t kAlpnExtensionType = 16;

common::IoResult<void> configure_common_context(SSL_CTX *ctx, const TlsOptions &options, bool is_server) {
    if (!ctx) {
        return std::unexpected(common::IoErr::Invalid);
    }

    if (options.min_version > 0) {
        SSL_CTX_set_min_proto_version(ctx, options.min_version);
    }
    if (options.max_version > 0) {
        SSL_CTX_set_max_proto_version(ctx, options.max_version);
    }

    if (is_server) {
        if (options.verify_client) {
            if (options.ca_file.empty()) {
                return std::unexpected(common::IoErr::Invalid);
            }
            if (SSL_CTX_load_verify_locations(ctx, options.ca_file.c_str(), nullptr) != 1) {
                return std::unexpected(common::IoErr::Invalid);
            }
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
        }
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    }

    return {};
}

common::IoResult<void> load_server_identity(SSL_CTX *ctx, const TlsOptions &options) {
    if (!ctx) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (options.cert_file.empty() || options.key_file.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (SSL_CTX_use_certificate_file(ctx, options.cert_file.c_str(), SSL_FILETYPE_PEM) != 1) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, options.key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
}

int ssl_server_context_ex_index() {
    static int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

int ssl_remote_addr_ex_index() {
    static int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

TlsAlpnProtocolsView parse_client_hello_alpn(const SSL_CLIENT_HELLO *client_hello) noexcept {
    if (!client_hello) {
        return {};
    }
    const uint8_t *data = nullptr;
    size_t len = 0;
    if (!SSL_early_callback_ctx_extension_get(client_hello, kAlpnExtensionType, &data, &len) || !data || len == 0) {
        return {};
    }
    return {data, len};
}

enum ssl_select_cert_result_t select_server_certificate_cb(const SSL_CLIENT_HELLO *client_hello) {
    if (!client_hello || !client_hello->ssl) {
        return ssl_select_cert_error;
    }

    auto *server_ctx =
            static_cast<TlsServerContext *>(SSL_get_ex_data(client_hello->ssl, ssl_server_context_ex_index()));
    if (!server_ctx) {
        return ssl_select_cert_error;
    }

    auto *remote_addr =
            static_cast<const SocketAddress *>(SSL_get_ex_data(client_hello->ssl, ssl_remote_addr_ex_index()));
    const char *server_name = SSL_get_servername(client_hello->ssl, TLSEXT_NAMETYPE_host_name);
    TlsIdentitySelectInput hello_view{
            .server_name = server_name ? std::string_view(server_name) : std::string_view{},
            .alpn = parse_client_hello_alpn(client_hello),
            .selected_alpn = {},
            .remote_addr = remote_addr,
            .server_context = server_ctx,
            .transport = TlsTransportKind::Tcp,
    };

    TlsContext *selected = server_ctx->select_identity(hello_view);
    if (!selected) {
        selected = server_ctx->default_identity();
        if (!selected) {
            return ssl_select_cert_error;
        }
    }

    if (SSL_set_SSL_CTX(client_hello->ssl, selected->raw()) == nullptr) {
        return ssl_select_cert_error;
    }
    return ssl_select_cert_success;
}

} // namespace

TlsContext::TlsContext(TlsOptions options, bool is_server, bool require_server_identity) :
    options_(std::move(options)), is_server_(is_server), require_server_identity_(require_server_identity) {
    alpn_ = options_.alpn;
    alpn_.erase(std::remove_if(alpn_.begin(), alpn_.end(), [](const std::string &proto) { return proto.empty(); }),
                alpn_.end());
}

TlsContext::~TlsContext() {
    if (ctx_) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }
}

common::IoResult<void> TlsContext::init() {
    if (ctx_) {
        return {};
    }
    if (is_server_ && require_server_identity_ && (options_.cert_file.empty() || options_.key_file.empty())) {
        return std::unexpected(common::IoErr::Invalid);
    }

    SSL_CTX *ctx = SSL_CTX_new(is_server_ ? TLS_server_method() : TLS_client_method());
    if (!ctx) {
        return std::unexpected(common::IoErr::NoMem);
    }

    auto common_result = configure_common_context(ctx, options_, is_server_);
    if (!common_result) {
        SSL_CTX_free(ctx);
        return std::unexpected(common_result.error());
    }

    if (is_server_ && !options_.cert_file.empty()) {
        auto identity_result = load_server_identity(ctx, options_);
        if (!identity_result) {
            SSL_CTX_free(ctx);
            return std::unexpected(identity_result.error());
        }
    }

    if (!alpn_.empty()) {
        if (is_server_) {
            SSL_CTX_set_alpn_select_cb(ctx, &TlsContext::alpn_select_cb, this);
        } else {
            alpn_wire_.clear();
            for (const auto &proto: alpn_) {
                if (proto.size() > 255) {
                    SSL_CTX_free(ctx);
                    return std::unexpected(common::IoErr::Invalid);
                }
                alpn_wire_.push_back(static_cast<unsigned char>(proto.size()));
                alpn_wire_.insert(alpn_wire_.end(), proto.begin(), proto.end());
            }
            if (SSL_CTX_set_alpn_protos(ctx, alpn_wire_.data(), static_cast<unsigned int>(alpn_wire_.size())) != 0) {
                SSL_CTX_free(ctx);
                return std::unexpected(common::IoErr::Invalid);
            }
        }
    }

    ctx_ = ctx;
    return {};
}

int TlsContext::alpn_select_cb(SSL *, const unsigned char **out, unsigned char *outlen, const unsigned char *in,
                               unsigned int inlen, void *arg) {
    auto *self = static_cast<TlsContext *>(arg);
    if (!self || self->alpn().empty() || !in || inlen == 0) {
        return SSL_TLSEXT_ERR_NOACK;
    }

    for (const auto &proto: self->alpn()) {
        const unsigned char *ptr = in;
        unsigned int remaining = inlen;
        while (remaining > 0) {
            unsigned int len = ptr[0];
            if (len + 1 > remaining) {
                break;
            }
            if (len == proto.size() && std::memcmp(ptr + 1, proto.data(), proto.size()) == 0) {
                *out = ptr + 1;
                *outlen = static_cast<unsigned char>(len);
                return SSL_TLSEXT_ERR_OK;
            }
            ptr += len + 1;
            remaining -= len + 1;
        }
    }

    return SSL_TLSEXT_ERR_NOACK;
}

TlsServerContext::TlsServerContext(TlsOptions options) :
    options_(std::move(options)), identity_selector_(options_.identity_selector_ops) {}

TlsServerContext::~TlsServerContext() = default;

common::IoResult<void> TlsServerContext::init() {
    if (base_context_) {
        return {};
    }

    auto validate_result = validate_identity_options();
    if (!validate_result) {
        return std::unexpected(validate_result.error());
    }

    auto base_result = init_base_context();
    if (!base_result) {
        return std::unexpected(base_result.error());
    }

    auto default_result = init_default_identity();
    if (!default_result) {
        return std::unexpected(default_result.error());
    }

    auto named_result = init_named_identities();
    if (!named_result) {
        return std::unexpected(named_result.error());
    }

    if (!default_identity_ && identity_count_ == 0) {
        return std::unexpected(common::IoErr::Invalid);
    }

    SSL_CTX_set_select_certificate_cb(base_context_->raw(), &select_server_certificate_cb);
    return {};
}

SSL_CTX *TlsServerContext::raw() const noexcept { return base_context_ ? base_context_->raw() : nullptr; }

common::IoResult<void> TlsServerContext::bind_ssl(SSL *ssl, const SocketAddress *remote_addr) noexcept {
    if (!ssl || !base_context_ || !base_context_->raw()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (SSL_set_ex_data(ssl, ssl_server_context_ex_index(), this) != 1) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (SSL_set_ex_data(ssl, ssl_remote_addr_ex_index(), const_cast<net::SocketAddress *>(remote_addr)) != 1) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
}

TlsContext *TlsServerContext::select_identity(const TlsClientHelloView &client_hello) const noexcept {
    if (identity_selector_.select) {
        TlsContext *selected = identity_selector_.select(identity_selector_.ctx, client_hello);
        if (selected) {
            return selected;
        }
    }

    return default_identity();
}

common::IoResult<void> TlsServerContext::init_base_context() {
    TlsOptions base_options = options_;
    base_options.cert_file.clear();
    base_options.key_file.clear();
    auto base = std::make_unique<TlsContext>(std::move(base_options), true, false);
    auto init_result = base->init();
    if (!init_result) {
        return std::unexpected(init_result.error());
    }
    base_context_ = std::move(base);
    return {};
}

common::IoResult<void> TlsServerContext::init_default_identity() {
    if (options_.cert_file.empty() && options_.key_file.empty()) {
        return {};
    }

    TlsOptions identity_options = options_;
    identity_options.identities.clear();
    auto identity = std::make_unique<TlsContext>(std::move(identity_options), true, true);
    auto init_result = identity->init();
    if (!init_result) {
        return std::unexpected(init_result.error());
    }
    default_identity_ = std::move(identity);
    return {};
}

common::IoResult<void> TlsServerContext::init_named_identities() {
    identity_count_ = options_.identities.size();
    if (identity_count_ == 0) {
        return {};
    }

    identities_ = std::make_unique<IdentityEntry[]>(identity_count_);
    for (std::size_t i = 0; i < identity_count_; ++i) {
        TlsOptions identity_tls_options = options_;
        identity_tls_options.cert_file = options_.identities[i].cert_file;
        identity_tls_options.key_file = options_.identities[i].key_file;
        identity_tls_options.identities.clear();

        auto identity = std::make_unique<TlsContext>(std::move(identity_tls_options), true, true);
        auto init_result = identity->init();
        if (!init_result) {
            return std::unexpected(init_result.error());
        }

        identities_[i].context = std::move(identity);
        identities_[i].name = options_.identities[i].name;
    }
    return {};
}

common::IoResult<void> TlsServerContext::validate_identity_options() const {
    if (options_.cert_file.empty() != options_.key_file.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    for (std::size_t i = 0; i < options_.identities.size(); ++i) {
        const auto &identity = options_.identities[i];
        if (identity.name.empty() || identity.cert_file.empty() || identity.key_file.empty()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        for (std::size_t j = i + 1; j < options_.identities.size(); ++j) {
            if (options_.identities[j].name == identity.name) {
                return std::unexpected(common::IoErr::Invalid);
            }
        }
    }
    return {};
}

TlsContext *TlsServerContext::find_identity_by_name(std::string_view name) const noexcept {
    if (name.empty()) {
        return nullptr;
    }
    if (default_identity_ && name == "__default__") {
        return default_identity_.get();
    }
    for (std::size_t i = 0; i < identity_count_; ++i) {
        if (identities_[i].name == name) {
            return identities_[i].context.get();
        }
    }
    return nullptr;
}

} // namespace fiber::net
