#ifndef FIBER_NET_TLS_CONNECTION_OPTIONS_H
#define FIBER_NET_TLS_CONNECTION_OPTIONS_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct ssl_session_st;
typedef struct ssl_session_st SSL_SESSION;

namespace fiber::net {

class SocketAddress;
class TlsContext;

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

using SelectTlsContextCallback = const TlsContext *(*) (void *ctx, const TlsClientHelloView &client_hello) noexcept;

enum class TlsClientCertificateMode : std::uint8_t {
    None,
    Optional,
    Required,
};

struct TlsNewSessionOps {
    void *ctx = nullptr;
    bool (*store)(void *ctx, SSL_SESSION *session) noexcept = nullptr;
};

struct TlsClientConnectionOptions {
    // Borrowed. The context must outlive the SSL connection.
    const TlsContext *context = nullptr;
    bool verify_peer = false;
    std::chrono::milliseconds handshake_timeout{10000};
    int min_version = 0x0303; // TLS 1.2
    int max_version = 0x0304; // TLS 1.3
    std::vector<std::string> alpn{"http/1.1"};
    // DNS name sent in ClientHello. IP literals are not emitted as SNI.
    std::string sni_name{};
    // DNS name or IP address authenticated against the peer certificate. When
    // empty, a DNS-valued sni_name is used.
    std::string verify_name{};
    bool enable_early_data = false;
    const TlsNewSessionOps *new_session_ops = nullptr;

    [[nodiscard]] bool enabled() const noexcept { return context != nullptr; }
};

struct TlsServerConnectionOptions {
    // Borrowed. The default and callback-selected contexts, this options
    // object, and callback state must outlive every derived SSL connection.
    const TlsContext *default_context = nullptr;
    // Returning nullptr selects default_context.
    SelectTlsContextCallback select_tls_ctx_callback = nullptr;
    void *select_tls_ctx_ctx = nullptr;
    TlsClientCertificateMode client_certificate_mode = TlsClientCertificateMode::None;
    std::chrono::milliseconds handshake_timeout{10000};
    int min_version = 0x0303; // TLS 1.2
    int max_version = 0x0304; // TLS 1.3
    std::vector<std::string> alpn{"http/1.1"};
    bool enable_early_data = false;

    [[nodiscard]] bool enabled() const noexcept { return default_context != nullptr; }
};

} // namespace fiber::net

#endif // FIBER_NET_TLS_CONNECTION_OPTIONS_H
