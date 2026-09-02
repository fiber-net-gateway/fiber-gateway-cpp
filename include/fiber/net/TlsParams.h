#ifndef FIBER_NET_TLS_PARAMS_H
#define FIBER_NET_TLS_PARAMS_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../common/IoError.h"

struct ssl_session_st;
typedef struct ssl_session_st SSL_SESSION;

namespace fiber::net {

class SocketAddress;
class TlsCredential;
class TrustStore;
class TlsServerHandshakeConfig;

class TlsAlpnProtocolsView {
public:
    constexpr TlsAlpnProtocolsView() noexcept = default;
    constexpr TlsAlpnProtocolsView(const std::uint8_t *data, std::size_t size) noexcept : data_(data), size_(size) {}

    [[nodiscard]] constexpr const std::uint8_t *data() const noexcept { return data_; }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return protocol_list_size() == 0; }
    [[nodiscard]] bool contains(std::string_view protocol) const noexcept;

private:
    [[nodiscard]] std::size_t protocol_list_size() const noexcept;

    const std::uint8_t *data_ = nullptr;
    std::size_t size_ = 0;
};

inline std::size_t TlsAlpnProtocolsView::protocol_list_size() const noexcept {
    if (!data_ || size_ < 2) {
        return 0;
    }
    const std::size_t encoded = (static_cast<std::size_t>(data_[0]) << 8U) | static_cast<std::size_t>(data_[1]);
    return encoded + 2 <= size_ ? encoded : 0;
}

inline bool TlsAlpnProtocolsView::contains(std::string_view protocol) const noexcept {
    std::size_t remaining = protocol_list_size();
    if (remaining == 0) {
        return false;
    }
    const std::uint8_t *cur = data_ + 2;
    while (remaining > 0) {
        const std::size_t len = cur[0];
        if (len + 1 > remaining) {
            return false;
        }
        if (len == protocol.size() &&
            std::char_traits<char>::compare(reinterpret_cast<const char *>(cur + 1), protocol.data(), len) == 0) {
            return true;
        }
        cur += len + 1;
        remaining -= len + 1;
    }
    return false;
}

enum class TlsTransportKind : std::uint8_t {
    Tcp,
    Quic,
};

struct TlsClientHelloView {
    std::string_view server_name{};
    TlsAlpnProtocolsView offered_alpn{};
    const SocketAddress *remote_addr = nullptr;
    const SocketAddress *local_addr = nullptr;
    TlsTransportKind transport = TlsTransportKind::Tcp;
};

enum class TlsClientCertificateMode : std::uint8_t {
    None,
    Optional,
    Required,
};

struct TlsNewSessionOps {
    void *ctx = nullptr;
    bool (*store)(void *ctx, SSL_SESSION *session) noexcept = nullptr;
};

using ConfigureTlsCallback = common::IoErr (*)(void *ctx, TlsServerHandshakeConfig &config,
                                               const TlsClientHelloView &client_hello) noexcept;

struct TlsClientParam {
    // handshake() synchronously creates and configures the SSL before returning
    // its task. These borrowed objects only need to remain valid through that
    // setup; the SSL retains its own references after successful installation.
    // TLS clients without a client certificate leave credential null.
    const TlsCredential *credential = nullptr;
    const TrustStore *trust_store = nullptr;
    bool verify_peer = false;
    std::chrono::milliseconds handshake_timeout{10000};
    int min_version = 0x0303; // TLS 1.2
    int max_version = 0x0304; // TLS 1.3
    std::vector<std::string> alpn{"http/1.1"};
    std::string sni_name{};
    std::string verify_name{};
    bool enable_early_data = false;
    const TlsNewSessionOps *new_session_ops = nullptr;

    // Higher-level transports use a value member for TLS parameters. This bit
    // distinguishes a plain connection from certificate-less client TLS.
    bool enable_tls = false;

    [[nodiscard]] bool enabled() const noexcept { return enable_tls; }
};

struct TlsServerParam {
    // This object remains borrowed through the handshake. Callback state and
    // material selected by it must remain valid until the synchronous callback
    // finishes. The SSL retains its own material references after installation.
    // Required for TLS. The synchronous callback configures the current SSL
    // after ClientHello and must add at least one credential.
    ConfigureTlsCallback configure_callback = nullptr;
    void *configure_ctx = nullptr;
    const TrustStore *trust_store = nullptr;
    TlsClientCertificateMode client_certificate_mode = TlsClientCertificateMode::None;
    std::chrono::milliseconds handshake_timeout{10000};
    int min_version = 0x0303; // TLS 1.2
    int max_version = 0x0304; // TLS 1.3
    std::vector<std::string> alpn{"http/1.1"};
    bool enable_early_data = false;

    [[nodiscard]] bool enabled() const noexcept { return configure_callback != nullptr; }
};

} // namespace fiber::net

#endif // FIBER_NET_TLS_PARAMS_H
