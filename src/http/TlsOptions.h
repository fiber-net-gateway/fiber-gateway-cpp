#ifndef FIBER_HTTP_TLS_OPTIONS_H
#define FIBER_HTTP_TLS_OPTIONS_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fiber::net {
class SocketAddress;
}

namespace fiber::http {

class TlsContext;
class TlsServerContext;

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
    std::size_t encoded = (static_cast<std::size_t>(data_[0]) << 8U) | static_cast<std::size_t>(data_[1]);
    if (encoded + 2 > size_) {
        return 0;
    }
    return encoded;
}

inline bool TlsAlpnProtocolsView::contains(std::string_view protocol) const noexcept {
    std::size_t remaining = protocol_list_size();
    if (remaining == 0) {
        return false;
    }
    const std::uint8_t *cur = data_ + 2;
    while (remaining > 0) {
        std::size_t len = cur[0];
        if (len + 1 > remaining) {
            return false;
        }
        if (len == protocol.size() &&
            std::char_traits<char>::compare(reinterpret_cast<const char *>(cur + 1), protocol.data(),
                                            static_cast<std::size_t>(len)) == 0) {
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

struct TlsIdentitySelectInput {
    std::string_view server_name{};
    TlsAlpnProtocolsView alpn{};
    std::string_view selected_alpn{};
    const fiber::net::SocketAddress *remote_addr = nullptr;
    const fiber::net::SocketAddress *local_addr = nullptr;
    const TlsServerContext *server_context = nullptr;
    TlsTransportKind transport = TlsTransportKind::Tcp;
};

using TlsClientHelloView = TlsIdentitySelectInput;

struct TlsIdentitySelectorOps {
    TlsContext *(*select)(void *ctx, const TlsIdentitySelectInput &input) noexcept = nullptr;
    void *ctx = nullptr;
};

struct TlsIdentityOptions {
    std::string name;
    std::string cert_file;
    std::string key_file;
};

struct TlsOptions {
    bool enabled = false;
    std::string cert_file;
    std::string key_file;
    std::string ca_file;
    bool verify_client = false;
    std::chrono::seconds handshake_timeout{10};
    int min_version = 0x0303; // TLS 1.2
    int max_version = 0x0304; // TLS 1.3
    std::vector<std::string> alpn{"http/1.1"};
    std::string server_name;
    std::vector<TlsIdentityOptions> identities;
    TlsIdentitySelectorOps identity_selector_ops{};
};

} // namespace fiber::http

#endif // FIBER_HTTP_TLS_OPTIONS_H
