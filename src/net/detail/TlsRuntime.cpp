#include "TlsRuntime.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <fiber/net/SocketAddress.h>
#include <fiber/net/TlsServerHandshakeConfig.h>
#include <fiber/net/detail/TlsHandshakeState.h>
#include <fiber/net/detail/TlsSessionOps.h>

namespace fiber::net::detail {

namespace {

constexpr std::uint16_t kAlpnExtensionType = 16;

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

bool offered_alpn_contains(const unsigned char *wire, unsigned int wire_size, std::string_view protocol) noexcept {
    const unsigned char *cur = wire;
    unsigned int remaining = wire_size;
    while (cur && remaining > 0) {
        const unsigned int len = cur[0];
        if (len + 1 > remaining) {
            return false;
        }
        if (len == protocol.size() && std::memcmp(cur + 1, protocol.data(), protocol.size()) == 0) {
            return true;
        }
        cur += len + 1;
        remaining -= len + 1;
    }
    return false;
}

} // namespace

enum ssl_select_cert_result_t TlsRuntime::select_certificate_callback(const SSL_CLIENT_HELLO *client_hello) noexcept {
    if (!client_hello || !client_hello->ssl) {
        return ssl_select_cert_error;
    }
    SSL *ssl = client_hello->ssl;
    auto *state =
            static_cast<TlsServerHandshakeState *>(SSL_get_ex_data(ssl, TlsRuntime::server_handshake_state_index()));
    if (!state || !state->param || !state->param->configure_callback) {
        return ssl_select_cert_error;
    }

    state->reset_for_callback();
    TlsServerHandshakeConfig config(ssl, *state);
    common::IoErr error = config.clear_credentials();
    if (error == common::IoErr::None) {
        error = config.set_protocol_versions(state->param->min_version, state->param->max_version);
    }
    if (error == common::IoErr::None) {
        error = config.set_early_data_enabled(state->param->enable_early_data);
    }
    if (error == common::IoErr::None && state->param->trust_store) {
        error = config.set_trust_store(*state->param->trust_store);
    }
    if (error == common::IoErr::None) {
        error = config.set_client_certificate_mode(state->param->client_certificate_mode);
    }

    const char *server_name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    const TlsClientHelloView hello{
            .server_name = server_name ? std::string_view(server_name) : std::string_view{},
            .offered_alpn = parse_client_hello_alpn(client_hello),
            .remote_addr = state->remote_addr,
            .local_addr = state->local_addr,
            .transport = state->transport,
    };
    if (error == common::IoErr::None) {
        error = state->param->configure_callback(state->param->configure_ctx, config, hello);
    }
    if (error == common::IoErr::None && state->credential_count == 0) {
        error = common::IoErr::Invalid;
    }
    if (error == common::IoErr::None && state->client_certificate_mode != TlsClientCertificateMode::None &&
        !state->trust_store_set) {
        error = common::IoErr::Invalid;
    }
    state->callback_error = error;
    return error == common::IoErr::None ? ssl_select_cert_success : ssl_select_cert_error;
}

namespace {

int alpn_select_cb(SSL *ssl, const unsigned char **out, unsigned char *outlen, const unsigned char *in,
                   unsigned int inlen, void *) noexcept {
    auto *state =
            static_cast<TlsServerHandshakeState *>(SSL_get_ex_data(ssl, TlsRuntime::server_handshake_state_index()));
    if (!state || !state->param || !in || inlen == 0) {
        return SSL_TLSEXT_ERR_NOACK;
    }

    if (state->selected_alpn_size != 0) {
        const std::string_view selected(reinterpret_cast<const char *>(state->selected_alpn.data()),
                                        state->selected_alpn_size);
        if (!offered_alpn_contains(in, inlen, selected)) {
            return SSL_TLSEXT_ERR_NOACK;
        }
        *out = state->selected_alpn.data();
        *outlen = state->selected_alpn_size;
        return SSL_TLSEXT_ERR_OK;
    }

    for (const auto &protocol: state->param->alpn) {
        if (!protocol.empty() && protocol.size() <= 255 && offered_alpn_contains(in, inlen, protocol)) {
            *out = reinterpret_cast<const unsigned char *>(protocol.data());
            *outlen = static_cast<unsigned char>(protocol.size());
            return SSL_TLSEXT_ERR_OK;
        }
    }
    return SSL_TLSEXT_ERR_NOACK;
}

int new_session_cb(SSL *ssl, SSL_SESSION *session) noexcept {
    auto *ops = static_cast<const TlsNewSessionOps *>(SSL_get_ex_data(ssl, TlsRuntime::new_session_ops_index()));
    return ops && ops->store && ops->store(ops->ctx, session) ? 1 : 0;
}

struct RuntimeHolder {
    RuntimeHolder() noexcept {
        client = SSL_CTX_new(TLS_method());
        server = SSL_CTX_new(TLS_method());
        if (client) {
            SSL_CTX_set_verify(client, SSL_VERIFY_NONE, nullptr);
            SSL_CTX_set_session_cache_mode(client, SSL_SESS_CACHE_BOTH | SSL_SESS_CACHE_NO_INTERNAL);
            SSL_CTX_sess_set_new_cb(client, &new_session_cb);
        }
        if (server) {
            SSL_CTX_set_verify(server, SSL_VERIFY_NONE, nullptr);
            SSL_CTX_set_select_certificate_cb(server, &TlsRuntime::select_certificate_callback);
            SSL_CTX_set_alpn_select_cb(server, &alpn_select_cb, nullptr);
            SSL_CTX_set_session_cache_mode(server, SSL_SESS_CACHE_BOTH | SSL_SESS_CACHE_NO_INTERNAL);
            SSL_CTX_sess_set_new_cb(server, &new_session_cb);
        }
    }

    ~RuntimeHolder() {
        SSL_CTX_free(client);
        SSL_CTX_free(server);
    }

    SSL_CTX *client = nullptr;
    SSL_CTX *server = nullptr;
};

RuntimeHolder &runtime() noexcept {
    static RuntimeHolder holder;
    return holder;
}

} // namespace

SSL_CTX *TlsRuntime::client_context() noexcept { return runtime().client; }

SSL_CTX *TlsRuntime::server_context() noexcept { return runtime().server; }

int TlsRuntime::server_handshake_state_index() noexcept {
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

int TlsRuntime::new_session_ops_index() noexcept {
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

} // namespace fiber::net::detail
