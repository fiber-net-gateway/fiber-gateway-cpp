#include "Http1ConnectionGroupKey.h"

#include <cstring>

namespace fiber::http {

namespace {

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

constexpr unsigned char ascii_to_lower(unsigned char ch) noexcept {
    return ch >= 'A' && ch <= 'Z' ? static_cast<unsigned char>(ch - 'A' + 'a') : ch;
}

inline void hash_byte(std::uint64_t &hash, std::uint8_t byte) noexcept {
    hash ^= static_cast<std::uint64_t>(byte);
    hash *= kFnvPrime;
}

inline void hash_be16(std::uint64_t &hash, std::uint16_t value) noexcept {
    hash_byte(hash, static_cast<std::uint8_t>(value >> 8U));
    hash_byte(hash, static_cast<std::uint8_t>(value & 0xffU));
}

inline void hash_be32(std::uint64_t &hash, std::uint32_t value) noexcept {
    hash_byte(hash, static_cast<std::uint8_t>((value >> 24U) & 0xffU));
    hash_byte(hash, static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    hash_byte(hash, static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    hash_byte(hash, static_cast<std::uint8_t>(value & 0xffU));
}

bool ip_equal(const net::IpAddress &left, const net::IpAddress &right) noexcept {
    if (left.family() != right.family()) {
        return false;
    }
    if (left.is_v4()) {
        return left.v4_bytes() == right.v4_bytes();
    }
    return left.scope_id() == right.scope_id() && left.v6_bytes() == right.v6_bytes();
}

std::uint64_t compute_name_hash(std::string_view host, std::uint16_t port,
                                Http1ConnectionGroupKey::Scheme scheme) noexcept {
    std::uint64_t hash = kFnvOffsetBasis;
    hash_byte(hash, static_cast<std::uint8_t>(Http1ConnectionGroupKey::HostKind::Name));
    hash_byte(hash, static_cast<std::uint8_t>(scheme));
    hash_be16(hash, port);
    for (char ch: host) {
        hash_byte(hash, ascii_to_lower(static_cast<unsigned char>(ch)));
    }
    return hash;
}

std::uint64_t compute_ip_hash(const net::IpAddress &ip, std::uint16_t port,
                              Http1ConnectionGroupKey::Scheme scheme) noexcept {
    std::uint64_t hash = kFnvOffsetBasis;
    hash_byte(hash, static_cast<std::uint8_t>(Http1ConnectionGroupKey::HostKind::Ip));
    hash_byte(hash, static_cast<std::uint8_t>(scheme));
    hash_be16(hash, port);
    hash_byte(hash, static_cast<std::uint8_t>(ip.family()));
    if (ip.is_v4()) {
        for (std::uint8_t byte: ip.v4_bytes()) {
            hash_byte(hash, byte);
        }
        return hash;
    }
    for (std::uint8_t byte: ip.v6_bytes()) {
        hash_byte(hash, byte);
    }
    hash_be32(hash, ip.scope_id());
    return hash;
}

} // namespace

std::optional<Http1ConnectionGroupKey> Http1ConnectionGroupKey::from_name(std::string_view host, std::uint16_t port,
                                                                          Scheme scheme) noexcept {
    if (host.empty() || host.size() > kMaxHostNameSize) {
        return std::nullopt;
    }

    Http1ConnectionGroupKey key;
    key.host_kind_ = HostKind::Name;
    key.scheme_ = scheme;
    key.port_ = port;
    key.host_name_size_ = static_cast<std::uint16_t>(host.size());
    for (std::size_t i = 0; i < host.size(); ++i) {
        key.host_name_[i] = static_cast<char>(ascii_to_lower(static_cast<unsigned char>(host[i])));
    }
    key.hash_ = compute_name_hash(host, port, scheme);
    return key;
}

Http1ConnectionGroupKey Http1ConnectionGroupKey::from_ip(net::IpAddress ip, std::uint16_t port,
                                                         Scheme scheme) noexcept {
    Http1ConnectionGroupKey key;
    key.host_kind_ = HostKind::Ip;
    key.scheme_ = scheme;
    key.port_ = port;
    key.ip_address_ = ip;
    key.hash_ = compute_ip_hash(ip, port, scheme);
    return key;
}

bool operator==(const Http1ConnectionGroupKey &left, const Http1ConnectionGroupKey &right) noexcept {
    if (left.hash_ != right.hash_ || left.host_kind_ != right.host_kind_ || left.scheme_ != right.scheme_ ||
        left.port_ != right.port_) {
        return false;
    }

    if (left.host_kind_ == Http1ConnectionGroupKey::HostKind::Name) {
        return left.host_name_size_ == right.host_name_size_ &&
               std::memcmp(left.host_name_.data(), right.host_name_.data(), left.host_name_size_) == 0;
    }

    return ip_equal(left.ip_address_, right.ip_address_);
}

} // namespace fiber::http
