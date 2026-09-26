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
#include "../tls/handshake/TlsHandshakeMessage.h"

namespace fiber::tls {
class TlsTicketService;
}

namespace fiber::net {

class TlsCredential;
class TrustStore;
class TlsServerHandshakeConfig;

enum class TlsClientCertificateMode : std::uint8_t {
    None,
    Optional,
    Required,
};

using ConfigureTlsCallback = common::IoErr (*)(void *ctx, TlsServerHandshakeConfig &config,
                                               const tls::TlsClientHelloView &client_hello) noexcept;

struct TlsClientSecurity {
    // The SSL retains its own references after successful installation.
    // TLS clients without a client certificate leave credential null.
    const TlsCredential *credential = nullptr;
    // Trust anchors for peer verification. Null with verify_peer set means
    // the process-wide system roots (TrustStore::system_default): pass an
    // explicit store when the peer must chain to a private CA only — a null
    // store would happily accept publicly-trusted certificates too.
    const TrustStore *trust_store = nullptr;
    bool verify_peer = false;
};

struct TlsClientParam {
    // Members are applied when the handshake starts (the QUIC path copies
    // what it needs inside create_client; the TCP engine path stages borrowed
    // views into the per-connection handshake state). Either way the param,
    // the storage its alpn span points into, and the pointees of security
    // (credential, trust store) must stay valid until the handshake
    // co_returns — like TlsServerParam below. Operation policy such as
    // handshake timeout, session caching, and early data belongs to the
    // transport driving the handshake rather than this TLS parameter object.
    TlsClientSecurity security{};
    int min_version = 0x0303; // TLS 1.2
    int max_version = 0x0304; // TLS 1.3
    std::span<const std::string_view> alpn{};
    std::string_view server_name{};
    std::string_view verify_name{};
};

inline constexpr std::chrono::milliseconds kDefaultTlsHandshakeTimeout{10000};

// Owning ALPN protocol list for callers that need to keep a caller-configured
// set of protocols alive across many handshakes (e.g. an HTTP/3 client's "h3",
// set once and reused per connection) and hand out a borrowed span for
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
    // Stateless session tickets (09 §6): non-null wires the service's minter
    // and lookup into the handshake — the server then sends NewSessionTickets
    // (1.3) / session tickets (1.2) and resumes from presented ones. Null (the
    // default) = no tickets minted and no resumption: every connection is a
    // full handshake. Borrowed like the other members: the pointee must stay
    // valid until the handshake co_returns. Immutable after construction, and
    // mint/open are lock-free, so one service may be shared by every worker.
    const tls::TlsTicketService *ticket_service = nullptr;

    [[nodiscard]] bool enabled() const noexcept { return configure_callback != nullptr; }
};

} // namespace fiber::net

#endif // FIBER_NET_TLS_PARAMS_H
