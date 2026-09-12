#ifndef FIBER_HTTP_HTTP_CONNECTION_GROUP_KEY_H
#define FIBER_HTTP_HTTP_CONNECTION_GROUP_KEY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

#include "../common/Assert.h"
#include "../net/IpAddress.h"

namespace fiber::http {

// Identity of one upstream service as a client sees it: the request authority host, the port,
// the scheme, and optionally the address to dial.
//
//   host      ip        dial            TLS server name
//   name      none      DNS at dial     host
//   name      pinned    ip              host
//   literal   parsed    ip              n/a (HTTPS is rejected: RFC 6066 §3 forbids literal SNI)
//
// Every field takes part in hashing and equality, so a name with a pinned address and the same
// name resolved through DNS are distinct groups, as are two names pinned to the same address.
class HttpConnectionGroupKey {
public:
    enum class Scheme : std::uint8_t {
        Http,
        Https,
    };

    static constexpr std::size_t kMaxHostSize = 255;

    // `host` is a DNS name or a bare IP literal (no brackets, no port); it is stored lowercased.
    // A literal host is parsed into ip() and must not be combined with an explicit `ip`.
    // `ip` pins the dial target for a DNS-name host; without it the host is resolved at dial time.
    // Returns nullopt for an empty, oversized, or malformed host, for Https with a literal host,
    // and for a literal host combined with `ip`.
    [[nodiscard]] static std::optional<HttpConnectionGroupKey>
    make(std::string_view host, std::uint16_t port, Scheme scheme,
         std::optional<net::IpAddress> ip = std::nullopt) noexcept;

    [[nodiscard]] std::string_view host() const noexcept { return std::string_view(host_.data(), host_size_); }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] Scheme scheme() const noexcept { return scheme_; }
    [[nodiscard]] bool has_ip() const noexcept { return has_ip_; }
    [[nodiscard]] const net::IpAddress &ip() const noexcept {
        FIBER_ASSERT(has_ip_);
        return ip_;
    }
    [[nodiscard]] std::uint64_t hash() const noexcept { return hash_; }

    friend bool operator==(const HttpConnectionGroupKey &left, const HttpConnectionGroupKey &right) noexcept;
    friend bool operator!=(const HttpConnectionGroupKey &left, const HttpConnectionGroupKey &right) noexcept {
        return !(left == right);
    }

private:
    HttpConnectionGroupKey() = default;

    Scheme scheme_ = Scheme::Http;
    bool has_ip_ = false;
    std::uint16_t port_ = 0;
    std::uint16_t host_size_ = 0;
    std::uint64_t hash_ = 0;
    net::IpAddress ip_{};
    std::array<char, kMaxHostSize> host_{};
};

bool operator==(const HttpConnectionGroupKey &left, const HttpConnectionGroupKey &right) noexcept;

} // namespace fiber::http

namespace std {

template<>
struct hash<fiber::http::HttpConnectionGroupKey> {
    std::size_t operator()(const fiber::http::HttpConnectionGroupKey &key) const noexcept {
        return static_cast<std::size_t>(key.hash());
    }
};

} // namespace std

#endif // FIBER_HTTP_HTTP_CONNECTION_GROUP_KEY_H
