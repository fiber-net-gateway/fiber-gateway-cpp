#include <fiber/net/TlsContext.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/tls1.h>
#include <openssl/x509.h>

#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>

namespace fiber::net {

namespace {

constexpr std::uint16_t kAlpnExtensionType = 16;
constexpr std::size_t kMaxAlpnWireSize = 4096;

class OpenSslErrorQueueScope {
public:
    OpenSslErrorQueueScope() noexcept { ERR_clear_error(); }
    ~OpenSslErrorQueueScope() noexcept { ERR_clear_error(); }

    OpenSslErrorQueueScope(const OpenSslErrorQueueScope &) = delete;
    OpenSslErrorQueueScope &operator=(const OpenSslErrorQueueScope &) = delete;
};

bool valid_pem_source(const TlsPemSource &source) noexcept {
    return source.kind == TlsPemSourceKind::None ? source.value.empty() : !source.value.empty();
}

bool valid_trust_source(const TlsTrustStoreSource &source) noexcept {
    switch (source.kind) {
        case TlsTrustStoreKind::None:
        case TlsTrustStoreKind::System:
            return source.value.empty();
        case TlsTrustStoreKind::File:
        case TlsTrustStoreKind::Content:
            return !source.value.empty();
    }
    return false;
}

common::IoResult<void> load_certificate_chain_content(SSL_CTX *ctx, std::string_view pem) noexcept {
    if (pem.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(common::IoErr::Invalid);
    }
    BIO *bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) {
        return std::unexpected(common::IoErr::NoMem);
    }
    STACK_OF(X509_INFO) *infos = PEM_X509_INFO_read_bio(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!infos) {
        return std::unexpected(common::IoErr::Invalid);
    }

    bool loaded_leaf = false;
    bool valid = true;
    const std::size_t count = sk_X509_INFO_num(infos);
    for (std::size_t i = 0; i < count; ++i) {
        X509_INFO *info = sk_X509_INFO_value(infos, i);
        if (!info || !info->x509 || info->crl || info->x_pkey) {
            valid = false;
            break;
        }
        if (!loaded_leaf) {
            valid = SSL_CTX_use_certificate(ctx, info->x509) == 1;
            loaded_leaf = valid;
        } else if (SSL_CTX_add1_chain_cert(ctx, info->x509) != 1) {
            valid = false;
            break;
        }
    }
    sk_X509_INFO_pop_free(infos, X509_INFO_free);
    return valid && loaded_leaf ? common::IoResult<void>{} : std::unexpected(common::IoErr::Invalid);
}

common::IoResult<void> load_private_key_content(SSL_CTX *ctx, std::string_view pem) noexcept {
    if (pem.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(common::IoErr::Invalid);
    }
    BIO *bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) {
        return std::unexpected(common::IoErr::NoMem);
    }
    EVP_PKEY *key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!key) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const bool loaded = SSL_CTX_use_PrivateKey(ctx, key) == 1;
    EVP_PKEY_free(key);
    return loaded ? common::IoResult<void>{} : std::unexpected(common::IoErr::Invalid);
}

common::IoResult<void> load_trust_store_content(SSL_CTX *ctx, std::string_view pem) noexcept {
    if (pem.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(common::IoErr::Invalid);
    }
    BIO *bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) {
        return std::unexpected(common::IoErr::NoMem);
    }
    STACK_OF(X509_INFO) *infos = PEM_X509_INFO_read_bio(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!infos) {
        return std::unexpected(common::IoErr::Invalid);
    }

    X509_STORE *store = SSL_CTX_get_cert_store(ctx);
    bool loaded_any = false;
    bool valid = store != nullptr;
    const std::size_t count = sk_X509_INFO_num(infos);
    for (std::size_t i = 0; valid && i < count; ++i) {
        X509_INFO *info = sk_X509_INFO_value(infos, i);
        if (!info || !info->x509 || info->crl || info->x_pkey || X509_STORE_add_cert(store, info->x509) != 1) {
            valid = false;
            break;
        }
        loaded_any = true;
    }
    sk_X509_INFO_pop_free(infos, X509_INFO_free);
    return valid && loaded_any ? common::IoResult<void>{} : std::unexpected(common::IoErr::Invalid);
}

common::IoResult<void> load_identity(SSL_CTX *ctx, const TlsOptions &options) noexcept {
    common::IoResult<void> certificate_result;
    switch (options.certificate_chain.kind) {
        case TlsPemSourceKind::File:
            certificate_result = SSL_CTX_use_certificate_chain_file(ctx, options.certificate_chain.value.c_str()) == 1
                                         ? common::IoResult<void>{}
                                         : std::unexpected(common::IoErr::Invalid);
            break;
        case TlsPemSourceKind::Content:
            certificate_result = load_certificate_chain_content(ctx, options.certificate_chain.value);
            break;
        case TlsPemSourceKind::None:
            return std::unexpected(common::IoErr::Invalid);
    }
    if (!certificate_result) {
        return std::unexpected(certificate_result.error());
    }

    common::IoResult<void> key_result;
    switch (options.private_key.kind) {
        case TlsPemSourceKind::File:
            key_result = SSL_CTX_use_PrivateKey_file(ctx, options.private_key.value.c_str(), SSL_FILETYPE_PEM) == 1
                                 ? common::IoResult<void>{}
                                 : std::unexpected(common::IoErr::Invalid);
            break;
        case TlsPemSourceKind::Content:
            key_result = load_private_key_content(ctx, options.private_key.value);
            break;
        case TlsPemSourceKind::None:
            return std::unexpected(common::IoErr::Invalid);
    }
    if (!key_result) {
        return std::unexpected(key_result.error());
    }
    return SSL_CTX_check_private_key(ctx) == 1 ? common::IoResult<void>{} : std::unexpected(common::IoErr::Invalid);
}

common::IoResult<void> load_trust_store(SSL_CTX *ctx, const TlsTrustStoreSource &source) noexcept {
    switch (source.kind) {
        case TlsTrustStoreKind::None:
            return {};
        case TlsTrustStoreKind::System: {
            const std::string &system_ca = TlsContext::system_ca_bundle_path();
            const int loaded = !system_ca.empty() ? SSL_CTX_load_verify_locations(ctx, system_ca.c_str(), nullptr)
                                                  : SSL_CTX_set_default_verify_paths(ctx);
            return loaded == 1 ? common::IoResult<void>{} : std::unexpected(common::IoErr::Invalid);
        }
        case TlsTrustStoreKind::File:
            return SSL_CTX_load_verify_locations(ctx, source.value.c_str(), nullptr) == 1
                           ? common::IoResult<void>{}
                           : std::unexpected(common::IoErr::Invalid);
        case TlsTrustStoreKind::Content:
            return load_trust_store_content(ctx, source.value);
    }
    return std::unexpected(common::IoErr::Invalid);
}

int ssl_server_options_ex_index() noexcept {
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

int ssl_remote_addr_ex_index() noexcept {
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

int ssl_local_addr_ex_index() noexcept {
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

int ssl_transport_kind_ex_index() noexcept {
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

int ssl_new_session_ops_ex_index() noexcept {
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

TlsAlpnProtocolsView parse_client_hello_alpn(const SSL_CLIENT_HELLO *client_hello) noexcept {
    if (!client_hello) {
        return {};
    }
    const std::uint8_t *data = nullptr;
    std::size_t len = 0;
    if (!SSL_early_callback_ctx_extension_get(client_hello, kAlpnExtensionType, &data, &len) || !data || len == 0) {
        return {};
    }
    return {data, len};
}

common::IoResult<void> set_session_identity(SSL *ssl, const TlsContext &context) noexcept {
    const auto value = reinterpret_cast<std::uintptr_t>(&context);
    std::array<std::uint8_t, sizeof(value)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(value));
    return SSL_set_session_id_context(ssl, bytes.data(), bytes.size()) == 1 ? common::IoResult<void>{}
                                                                            : std::unexpected(common::IoErr::Invalid);
}

common::IoResult<void> install_verify_store(SSL *ssl, const TlsContext &context) noexcept {
    if (!context.has_trust_store()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    X509_STORE *store = SSL_CTX_get_cert_store(context.raw());
    return store != nullptr && SSL_set1_verify_cert_store(ssl, store) == 1 ? common::IoResult<void>{}
                                                                           : std::unexpected(common::IoErr::Invalid);
}

int alpn_select_cb(SSL *ssl, const unsigned char **out, unsigned char *outlen, const unsigned char *in,
                   unsigned int inlen, void *) noexcept {
    auto *options =
            static_cast<const TlsServerConnectionOptions *>(SSL_get_ex_data(ssl, ssl_server_options_ex_index()));
    if (!options || options->alpn.empty() || !in || inlen == 0) {
        return SSL_TLSEXT_ERR_NOACK;
    }

    for (const auto &protocol: options->alpn) {
        if (protocol.empty() || protocol.size() > 255) {
            continue;
        }
        const unsigned char *cur = in;
        unsigned int remaining = inlen;
        while (remaining > 0) {
            const unsigned int len = cur[0];
            if (len + 1 > remaining) {
                return SSL_TLSEXT_ERR_NOACK;
            }
            if (len == protocol.size() && std::memcmp(cur + 1, protocol.data(), protocol.size()) == 0) {
                *out = cur + 1;
                *outlen = static_cast<unsigned char>(len);
                return SSL_TLSEXT_ERR_OK;
            }
            cur += len + 1;
            remaining -= len + 1;
        }
    }
    return SSL_TLSEXT_ERR_NOACK;
}

enum ssl_select_cert_result_t select_server_certificate_cb(const SSL_CLIENT_HELLO *client_hello) noexcept {
    if (!client_hello || !client_hello->ssl) {
        return ssl_select_cert_error;
    }
    SSL *ssl = client_hello->ssl;
    auto *options =
            static_cast<const TlsServerConnectionOptions *>(SSL_get_ex_data(ssl, ssl_server_options_ex_index()));
    if (!options || !options->default_context) {
        return ssl_select_cert_error;
    }

    const TlsContext *selected = options->default_context;
    if (options->select_tls_ctx_callback) {
        const char *server_name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
        const auto transport_value =
                reinterpret_cast<std::uintptr_t>(SSL_get_ex_data(ssl, ssl_transport_kind_ex_index()));
        TlsClientHelloView hello{
                .server_name = server_name ? std::string_view(server_name) : std::string_view{},
                .offered_alpn = parse_client_hello_alpn(client_hello),
                .remote_addr = static_cast<const SocketAddress *>(SSL_get_ex_data(ssl, ssl_remote_addr_ex_index())),
                .local_addr = static_cast<const SocketAddress *>(SSL_get_ex_data(ssl, ssl_local_addr_ex_index())),
                .transport = transport_value == static_cast<std::uintptr_t>(TlsTransportKind::Quic)
                                     ? TlsTransportKind::Quic
                                     : TlsTransportKind::Tcp,
        };
        if (const TlsContext *candidate = options->select_tls_ctx_callback(options->select_tls_ctx_ctx, hello)) {
            selected = candidate;
        }
    }

    if (!selected->has_identity() || SSL_set_SSL_CTX(ssl, selected->raw()) == nullptr ||
        !set_session_identity(ssl, *selected)) {
        return ssl_select_cert_error;
    }
    int verify_mode = SSL_VERIFY_NONE;
    if (options->client_certificate_mode != TlsClientCertificateMode::None) {
        if (!install_verify_store(ssl, *selected)) {
            return ssl_select_cert_error;
        }
        verify_mode = SSL_VERIFY_PEER;
        if (options->client_certificate_mode == TlsClientCertificateMode::Required) {
            verify_mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
        }
    }
    SSL_set_verify(ssl, verify_mode, nullptr);
    return ssl_select_cert_success;
}

int new_session_cb(SSL *ssl, SSL_SESSION *session) noexcept {
    auto *ops = static_cast<const TlsNewSessionOps *>(SSL_get_ex_data(ssl, ssl_new_session_ops_ex_index()));
    return ops && ops->store && ops->store(ops->ctx, session) ? 1 : 0;
}

common::IoResult<void> configure_versions(SSL *ssl, int min_version, int max_version) noexcept {
    if ((min_version > 0 && SSL_set_min_proto_version(ssl, static_cast<std::uint16_t>(min_version)) != 1) ||
        (max_version > 0 && SSL_set_max_proto_version(ssl, static_cast<std::uint16_t>(max_version)) != 1)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
}

common::IoResult<void> configure_client_alpn(SSL *ssl, const std::vector<std::string> &protocols) noexcept {
    std::array<std::uint8_t, kMaxAlpnWireSize> wire{};
    std::size_t size = 0;
    for (const auto &protocol: protocols) {
        if (protocol.empty()) {
            continue;
        }
        if (protocol.size() > 255 || size + protocol.size() + 1 > wire.size()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        wire[size++] = static_cast<std::uint8_t>(protocol.size());
        std::memcpy(wire.data() + size, protocol.data(), protocol.size());
        size += protocol.size();
    }
    return SSL_set_alpn_protos(ssl, wire.data(), size) == 0 ? common::IoResult<void>{}
                                                            : std::unexpected(common::IoErr::Invalid);
}

} // namespace

TlsContext::~TlsContext() {
    if (ctx_) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }
}

common::IoResult<std::unique_ptr<TlsContext>> TlsContext::create(const TlsOptions &options) noexcept {
    OpenSslErrorQueueScope error_queue_scope;
    if (!valid_pem_source(options.certificate_chain) || !valid_pem_source(options.private_key) ||
        !valid_trust_source(options.trust_store) ||
        (options.certificate_chain.empty() != options.private_key.empty())) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::unique_ptr<TlsContext> context(new (std::nothrow) TlsContext());
    if (!context) {
        return std::unexpected(common::IoErr::NoMem);
    }
    context->ctx_ = SSL_CTX_new(TLS_method());
    if (!context->ctx_) {
        return std::unexpected(common::IoErr::NoMem);
    }

    if (!options.certificate_chain.empty()) {
        auto identity = load_identity(context->ctx_, options);
        if (!identity) {
            return std::unexpected(identity.error());
        }
        context->has_identity_ = true;
    }
    if (!options.trust_store.empty()) {
        auto trust = load_trust_store(context->ctx_, options.trust_store);
        if (!trust) {
            return std::unexpected(trust.error());
        }
        context->has_trust_store_ = true;
    }

    SSL_CTX_set_verify(context->ctx_, SSL_VERIFY_NONE, nullptr);
    SSL_CTX_set_alpn_select_cb(context->ctx_, &alpn_select_cb, nullptr);
    SSL_CTX_set_select_certificate_cb(context->ctx_, &select_server_certificate_cb);
    SSL_CTX_set_session_cache_mode(context->ctx_, SSL_SESS_CACHE_BOTH | SSL_SESS_CACHE_NO_INTERNAL);
    SSL_CTX_sess_set_new_cb(context->ctx_, &new_session_cb);
    return context;
}

common::IoResult<SSL *> TlsContext::create_client_ssl(const TlsClientConnectionOptions &options) const noexcept {
    OpenSslErrorQueueScope error_queue_scope;
    if (!ctx_ || options.context != this || (options.verify_peer && !has_trust_store_)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    SSL *ssl = SSL_new(ctx_);
    if (!ssl) {
        return std::unexpected(common::IoErr::NoMem);
    }
    auto fail = [ssl](common::IoErr error) -> common::IoResult<SSL *> {
        SSL_free(ssl);
        return std::unexpected(error);
    };

    auto versions = configure_versions(ssl, options.min_version, options.max_version);
    if (!versions) {
        return fail(versions.error());
    }
    SSL_set_connect_state(ssl);
    SSL_set_early_data_enabled(ssl, options.enable_early_data ? 1 : 0);

    IpAddress sni_ip{};
    const bool sni_is_ip = !options.sni_name.empty() && IpAddress::parse(options.sni_name, sni_ip);
    if (!options.sni_name.empty() && !sni_is_ip && SSL_set_tlsext_host_name(ssl, options.sni_name.c_str()) != 1) {
        return fail(common::IoErr::Invalid);
    }

    if (options.verify_peer) {
        auto store = install_verify_store(ssl, *this);
        if (!store) {
            return fail(store.error());
        }
        SSL_set_verify(ssl, SSL_VERIFY_PEER, nullptr);
        const std::string &verify_name = options.verify_name.empty() ? options.sni_name : options.verify_name;
        if (verify_name.empty()) {
            return fail(common::IoErr::Invalid);
        }
        IpAddress verify_ip{};
        if (IpAddress::parse(verify_name, verify_ip)) {
            X509_VERIFY_PARAM *params = SSL_get0_param(ssl);
            if (!params || X509_VERIFY_PARAM_set1_ip(params, verify_ip.data(), verify_ip.byte_size()) != 1) {
                return fail(common::IoErr::Invalid);
            }
        } else if (SSL_set1_host(ssl, verify_name.c_str()) != 1) {
            return fail(common::IoErr::Invalid);
        }
    } else {
        SSL_set_verify(ssl, SSL_VERIFY_NONE, nullptr);
    }

    auto alpn = configure_client_alpn(ssl, options.alpn);
    if (!alpn) {
        return fail(alpn.error());
    }
    if (SSL_set_ex_data(ssl, ssl_new_session_ops_ex_index(), const_cast<TlsNewSessionOps *>(options.new_session_ops)) !=
        1) {
        return fail(common::IoErr::Invalid);
    }
    return ssl;
}

common::IoResult<SSL *> TlsContext::create_server_ssl(const TlsServerConnectionOptions &options,
                                                      const SocketAddress *local_addr, const SocketAddress *remote_addr,
                                                      TlsTransportKind transport) noexcept {
    OpenSslErrorQueueScope error_queue_scope;
    const TlsContext *context = options.default_context;
    if (!context || !context->ctx_ || !context->has_identity_ ||
        (options.client_certificate_mode != TlsClientCertificateMode::None && !context->has_trust_store_)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    SSL *ssl = SSL_new(context->ctx_);
    if (!ssl) {
        return std::unexpected(common::IoErr::NoMem);
    }
    auto fail = [ssl](common::IoErr error) -> common::IoResult<SSL *> {
        SSL_free(ssl);
        return std::unexpected(error);
    };

    auto versions = configure_versions(ssl, options.min_version, options.max_version);
    if (!versions) {
        return fail(versions.error());
    }
    SSL_set_accept_state(ssl);
    SSL_set_early_data_enabled(ssl, options.enable_early_data ? 1 : 0);

    int verify_mode = SSL_VERIFY_NONE;
    if (options.client_certificate_mode != TlsClientCertificateMode::None) {
        auto store = install_verify_store(ssl, *context);
        if (!store) {
            return fail(store.error());
        }
        verify_mode = SSL_VERIFY_PEER;
        if (options.client_certificate_mode == TlsClientCertificateMode::Required) {
            verify_mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
        }
    }
    SSL_set_verify(ssl, verify_mode, nullptr);
    if (!set_session_identity(ssl, *context) ||
        SSL_set_ex_data(ssl, ssl_server_options_ex_index(), const_cast<TlsServerConnectionOptions *>(&options)) != 1 ||
        SSL_set_ex_data(ssl, ssl_remote_addr_ex_index(), const_cast<SocketAddress *>(remote_addr)) != 1 ||
        SSL_set_ex_data(ssl, ssl_local_addr_ex_index(), const_cast<SocketAddress *>(local_addr)) != 1 ||
        SSL_set_ex_data(ssl, ssl_transport_kind_ex_index(),
                        reinterpret_cast<void *>(static_cast<std::uintptr_t>(transport))) != 1) {
        return fail(common::IoErr::Invalid);
    }
    return ssl;
}

const std::string &TlsContext::system_ca_bundle_path() noexcept {
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

} // namespace fiber::net
