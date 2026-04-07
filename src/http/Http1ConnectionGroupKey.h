#ifndef FIBER_HTTP_HTTP1_CONNECTION_GROUP_KEY_H
#define FIBER_HTTP_HTTP1_CONNECTION_GROUP_KEY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

#include "../common/Assert.h"
#include "../net/IpAddress.h"

namespace fiber::http {

class Http1ConnectionGroupKey {
public:
    enum class HostKind : std::uint8_t {
        Name,
        Ip,
    };

    enum class Scheme : std::uint8_t {
        Http,
        Https,
    };

    static constexpr std::size_t kMaxHostNameSize = 255;

    [[nodiscard]] static std::optional<Http1ConnectionGroupKey> from_name(std::string_view host,
                                                                          std::uint16_t port,
                                                                          Scheme scheme) noexcept;
    [[nodiscard]] static Http1ConnectionGroupKey from_ip(net::IpAddress ip, std::uint16_t port, Scheme scheme) noexcept;

    [[nodiscard]] HostKind host_kind() const noexcept { return host_kind_; }
    [[nodiscard]] Scheme scheme() const noexcept { return scheme_; }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] std::uint64_t hash() const noexcept { return hash_; }

    [[nodiscard]] std::string_view host_name() const noexcept {
        FIBER_ASSERT(host_kind_ == HostKind::Name);
        return std::string_view(host_name_.data(), host_name_size_);
    }

    [[nodiscard]] const net::IpAddress &ip_address() const noexcept {
        FIBER_ASSERT(host_kind_ == HostKind::Ip);
        return ip_address_;
    }

    [[nodiscard]] bool is_name() const noexcept { return host_kind_ == HostKind::Name; }
    [[nodiscard]] bool is_ip() const noexcept { return host_kind_ == HostKind::Ip; }

    friend bool operator==(const Http1ConnectionGroupKey &left, const Http1ConnectionGroupKey &right) noexcept;
    friend bool operator!=(const Http1ConnectionGroupKey &left, const Http1ConnectionGroupKey &right) noexcept {
        return !(left == right);
    }

private:
    Http1ConnectionGroupKey() = default;

    HostKind host_kind_ = HostKind::Name;
    Scheme scheme_ = Scheme::Http;
    std::uint16_t port_ = 0;
    std::uint16_t host_name_size_ = 0;
    std::uint64_t hash_ = 0;
    std::array<char, kMaxHostNameSize> host_name_{};
    net::IpAddress ip_address_{};
};

bool operator==(const Http1ConnectionGroupKey &left, const Http1ConnectionGroupKey &right) noexcept;

} // namespace fiber::http

namespace std {

template<>
struct hash<fiber::http::Http1ConnectionGroupKey> {
    std::size_t operator()(const fiber::http::Http1ConnectionGroupKey &key) const noexcept {
        return static_cast<std::size_t>(key.hash());
    }
};

} // namespace std

#endif // FIBER_HTTP_HTTP1_CONNECTION_GROUP_KEY_H
