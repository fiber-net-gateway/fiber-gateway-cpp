#include <fiber/http/HttpConnectionGroupKey.h>

#include <cstring>

namespace fiber::http {

namespace {

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

constexpr char ascii_to_lower(char ch) noexcept {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
}

// Rejects bytes that can only come from an unsplit authority ("[v6]:port") or a URL path, plus
// control characters and whitespace. ':' is checked separately since IPv6 literals contain it.
bool valid_host_byte(char ch) noexcept {
    const auto byte = static_cast<unsigned char>(ch);
    if (byte <= 0x20 || byte == 0x7f) {
        return false;
    }
    return ch != '/' && ch != '[' && ch != ']';
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

void hash_ip(std::uint64_t &hash, const net::IpAddress &ip) noexcept {
    hash_byte(hash, static_cast<std::uint8_t>(ip.family()));
    if (ip.is_v4()) {
        for (std::uint8_t byte: ip.v4_bytes()) {
            hash_byte(hash, byte);
        }
        return;
    }
    for (std::uint8_t byte: ip.v6_bytes()) {
        hash_byte(hash, byte);
    }
    hash_be32(hash, ip.scope_id());
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

} // namespace

std::optional<HttpConnectionGroupKey> HttpConnectionGroupKey::make(std::string_view host, std::uint16_t port,
                                                                   Scheme scheme,
                                                                   std::optional<net::IpAddress> ip) noexcept {
    if (host.empty() || host.size() > kMaxHostSize) {
        return std::nullopt;
    }
    for (char ch: host) {
        if (!valid_host_byte(ch)) {
            return std::nullopt;
        }
    }

    net::IpAddress literal;
    if (net::IpAddress::parse(host, literal)) {
        // A literal host carries its own address and cannot name a TLS server.
        if (ip.has_value() || scheme == Scheme::Https) {
            return std::nullopt;
        }
        ip = literal;
    } else if (host.find(':') != std::string_view::npos) {
        return std::nullopt;
    }

    HttpConnectionGroupKey key;
    key.scheme_ = scheme;
    key.port_ = port;
    key.host_size_ = static_cast<std::uint16_t>(host.size());
    for (std::size_t i = 0; i < host.size(); ++i) {
        key.host_[i] = ascii_to_lower(host[i]);
    }

    std::uint64_t hash = kFnvOffsetBasis;
    hash_byte(hash, static_cast<std::uint8_t>(scheme));
    hash_be16(hash, port);
    hash_byte(hash, ip.has_value() ? 1U : 0U);
    if (ip.has_value()) {
        key.has_ip_ = true;
        key.ip_ = *ip;
        hash_ip(hash, *ip);
    }
    for (std::size_t i = 0; i < host.size(); ++i) {
        hash_byte(hash, static_cast<std::uint8_t>(key.host_[i]));
    }
    key.hash_ = hash;
    return key;
}

bool operator==(const HttpConnectionGroupKey &left, const HttpConnectionGroupKey &right) noexcept {
    if (left.hash_ != right.hash_ || left.scheme_ != right.scheme_ || left.port_ != right.port_ ||
        left.has_ip_ != right.has_ip_ || left.host_size_ != right.host_size_) {
        return false;
    }
    if (left.has_ip_ && !ip_equal(left.ip_, right.ip_)) {
        return false;
    }
    return std::memcmp(left.host_.data(), right.host_.data(), left.host_size_) == 0;
}

} // namespace fiber::http
