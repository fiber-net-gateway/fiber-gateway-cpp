#include "TlsSslFactory.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <fiber/net/IpAddress.h>
#include <fiber/net/TlsCredential.h>
#include <fiber/net/TrustStore.h>

#include "TlsRuntime.h"

namespace fiber::net::detail {

namespace {

constexpr std::size_t kMaxAlpnWireSize = 4096;

class OpenSslErrorQueueScope {
public:
    OpenSslErrorQueueScope() noexcept { ERR_clear_error(); }
    ~OpenSslErrorQueueScope() noexcept { ERR_clear_error(); }
};

common::IoErr configure_versions(SSL *ssl, int min_version, int max_version) noexcept {
    if ((min_version > 0 && SSL_set_min_proto_version(ssl, static_cast<std::uint16_t>(min_version)) != 1) ||
        (max_version > 0 && SSL_set_max_proto_version(ssl, static_cast<std::uint16_t>(max_version)) != 1)) {
        return common::IoErr::Invalid;
    }
    return common::IoErr::None;
}

common::IoErr configure_client_alpn(SSL *ssl, const std::vector<std::string> &protocols) noexcept {
    std::array<std::uint8_t, kMaxAlpnWireSize> wire{};
    std::size_t size = 0;
    for (const auto &protocol: protocols) {
        if (protocol.empty()) {
            continue;
        }
        if (protocol.size() > 255 || size + protocol.size() + 1 > wire.size()) {
            return common::IoErr::Invalid;
        }
        wire[size++] = static_cast<std::uint8_t>(protocol.size());
        std::memcpy(wire.data() + size, protocol.data(), protocol.size());
        size += protocol.size();
    }
    return SSL_set_alpn_protos(ssl, wire.data(), size) == 0 ? common::IoErr::None : common::IoErr::Invalid;
}

} // namespace

common::IoResult<SSL *> TlsSslFactory::create_client(const TlsClientParam &param,
                                                     const TlsNewSessionOps *new_session_ops) noexcept {
    OpenSslErrorQueueScope error_queue_scope;
    SSL_CTX *context = TlsRuntime::client_context();
    if (!context || !param.enabled() || (param.verify_peer && !param.trust_store)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    SSL *ssl = SSL_new(context);
    if (!ssl) {
        return std::unexpected(common::IoErr::NoMem);
    }
    auto fail = [ssl](common::IoErr error) -> common::IoResult<SSL *> {
        SSL_free(ssl);
        return std::unexpected(error);
    };

    common::IoErr error = configure_versions(ssl, param.min_version, param.max_version);
    if (error != common::IoErr::None) {
        return fail(error);
    }
    SSL_set_connect_state(ssl);

    IpAddress sni_ip{};
    const bool sni_is_ip = !param.sni_name.empty() && IpAddress::parse(param.sni_name, sni_ip);
    if (!param.sni_name.empty() && !sni_is_ip && SSL_set_tlsext_host_name(ssl, param.sni_name.c_str()) != 1) {
        return fail(common::IoErr::Invalid);
    }

    if (param.verify_peer) {
        if (SSL_set1_verify_cert_store(ssl, param.trust_store->store_) != 1) {
            return fail(common::IoErr::Invalid);
        }
        SSL_set_verify(ssl, SSL_VERIFY_PEER, nullptr);
        const std::string &verify_name = param.verify_name.empty() ? param.sni_name : param.verify_name;
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

    if (param.credential && SSL_add1_credential(ssl, param.credential->credential_) != 1) {
        return fail(common::IoErr::Invalid);
    }
    error = configure_client_alpn(ssl, param.alpn);
    if (error != common::IoErr::None) {
        return fail(error);
    }
    if (SSL_set_ex_data(ssl, TlsRuntime::new_session_ops_index(), const_cast<TlsNewSessionOps *>(new_session_ops)) !=
        1) {
        return fail(common::IoErr::Invalid);
    }
    return ssl;
}

common::IoResult<SSL *> TlsSslFactory::create_server(const TlsServerParam &param, TlsServerHandshakeState &state,
                                                     const SocketAddress *local_addr, const SocketAddress *remote_addr,
                                                     TlsTransportKind transport) noexcept {
    OpenSslErrorQueueScope error_queue_scope;
    SSL_CTX *context = TlsRuntime::server_context();
    if (!context || !param.enabled()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    SSL *ssl = SSL_new(context);
    if (!ssl) {
        return std::unexpected(common::IoErr::NoMem);
    }
    state = {
            .param = &param,
            .remote_addr = remote_addr,
            .local_addr = local_addr,
            .transport = transport,
    };
    SSL_set_accept_state(ssl);
    if (SSL_set_ex_data(ssl, TlsRuntime::server_handshake_state_index(), &state) != 1) {
        SSL_free(ssl);
        return std::unexpected(common::IoErr::Invalid);
    }
    return ssl;
}

} // namespace fiber::net::detail
