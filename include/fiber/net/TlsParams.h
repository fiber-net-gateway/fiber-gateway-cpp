#ifndef FIBER_NET_TLS_PARAMS_H
#define FIBER_NET_TLS_PARAMS_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../common/IoError.h"

namespace fiber::net {

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

struct TlsClientHelloView {
    std::string_view server_name{};
    TlsAlpnProtocolsView offered_alpn{};
};

enum class TlsClientCertificateMode : std::uint8_t {
    None,
    Optional,
    Required,
};

using ConfigureTlsCallback = common::IoErr (*)(void *ctx, TlsServerHandshakeConfig &config,
                                               const TlsClientHelloView &client_hello) noexcept;

struct TlsClientSecurity {
    // The SSL retains its own references after successful installation.
    // TLS clients without a client certificate leave credential null.
    const TlsCredential *credential = nullptr;
    const TrustStore *trust_store = nullptr;
    bool verify_peer = false;
};

struct TlsClientParam {
    // Every member is applied synchronously when the client SSL is created
    // (TlsSslFactory::create_client / QuicTlsSession::init_client), before the
    // handshake coroutine ever suspends — BoringSSL copies what it needs out of
    // alpn/server_name/verify_name during that call (create_client() copies the
    // hostname into a bounded stack buffer itself, since it does not rely on
    // the view being NUL-terminated), so nothing here needs to stay valid past
    // create_client() returning, let alone for the whole handshake. Operation
    // policy such as handshake timeout, session caching, and early data belongs
    // to the transport driving the handshake rather than this TLS parameter
    // object.
    TlsClientSecurity security{};
    int min_version = 0x0303; // TLS 1.2
    int max_version = 0x0304; // TLS 1.3
    std::span<const std::string_view> alpn{};
    std::string_view server_name{};
    std::string_view verify_name{};
};

inline constexpr std::chrono::milliseconds kDefaultTlsHandshakeTimeout{10000};

// Owning ALPN protocol list for callers that need to keep a caller-configured
// set of protocols alive across many handshakes (e.g. QuicClient::Options::alpn,
// set once at init() and reused per connect()) and hand out a borrowed span for
// TlsClientParam::alpn/TlsServerParam::alpn. assign() copies the protocol
// characters; views returned by view() stay valid until the next mutation.
// Copy and move rebind the internal views to the new storage.
class TlsAlpnList {
public:
    TlsAlpnList() = default;
    TlsAlpnList(std::initializer_list<std::string_view> protocols) { assign(protocols); }
    TlsAlpnList(std::span<const std::string_view> protocols) { assign(protocols); }
    TlsAlpnList(const TlsAlpnList &other) { assign(other.view()); }
    TlsAlpnList(TlsAlpnList &&other) noexcept : owned_(std::move(other.owned_)) {
        other.owned_.clear();
        rebind_views();
    }
    TlsAlpnList &operator=(const TlsAlpnList &other) {
        if (this != &other) {
            assign(other.view());
        }
        return *this;
    }
    TlsAlpnList &operator=(TlsAlpnList &&other) noexcept {
        if (this != &other) {
            owned_ = std::move(other.owned_);
            other.owned_.clear();
            rebind_views();
        }
        return *this;
    }

    void assign(std::span<const std::string_view> protocols) {
        owned_.clear();
        owned_.reserve(protocols.size());
        for (const auto &protocol: protocols) {
            owned_.emplace_back(protocol);
        }
        rebind_views();
    }
    void assign(std::vector<std::string> &&protocols) {
        owned_ = std::move(protocols);
        rebind_views();
    }
    void clear() noexcept {
        owned_.clear();
        views_.clear();
    }
    [[nodiscard]] std::span<const std::string_view> view() const noexcept { return views_; }
    [[nodiscard]] bool empty() const noexcept { return views_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return views_.size(); }

private:
    void rebind_views() {
        views_.clear();
        views_.reserve(owned_.size());
        for (const auto &protocol: owned_) {
            views_.emplace_back(protocol.data(), protocol.size());
        }
    }

    std::vector<std::string> owned_;
    std::vector<std::string_view> views_;
};

struct TlsServerParam {
    // Trivially copyable, non-owning view assembled per handshake. The
    // handshake borrows it until it co_returns: the param, the storage its
    // alpn span points into, and the pointees of configure_ctx and trust_store
    // must stay valid until the handshake completes (the configure callback
    // runs at ClientHello; afterwards the SSL retains its own material
    // references). Copying the param is free and extends the borrow — but the
    // alpn span keeps pointing at the original backing. Required for TLS: the
    // callback configures the current SSL after ClientHello and must add at
    // least one credential.
    ConfigureTlsCallback configure_callback = nullptr;
    void *configure_ctx = nullptr;
    const TrustStore *trust_store = nullptr;
    TlsClientCertificateMode client_certificate_mode = TlsClientCertificateMode::None;
    std::span<const std::string_view> alpn{};
    int min_version = 0x0303; // TLS 1.2
    int max_version = 0x0304; // TLS 1.3
    bool enable_early_data = false;

    [[nodiscard]] bool enabled() const noexcept { return configure_callback != nullptr; }
};

} // namespace fiber::net

#endif // FIBER_NET_TLS_PARAMS_H
