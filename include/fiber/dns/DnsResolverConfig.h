#ifndef FIBER_DNS_DNS_RESOLVER_CONFIG_H
#define FIBER_DNS_DNS_RESOLVER_CONFIG_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include "../net/SocketAddress.h"

namespace fiber::dns {

inline constexpr std::uint8_t kMaxDnsNameservers = 3;
inline constexpr std::uint8_t kMaxDnsSearchDomains = 6;
inline constexpr std::size_t kMaxDnsSearchBytes = 256;
inline constexpr std::size_t kMaxResolverConfigFileSize = 64U * 1024U;

// Fixed-capacity list preserving insertion order. The parser reports TooManyNameservers
// instead of truncating a configuration that exceeds kMaxDnsNameservers.
class DnsNameserverList {
public:
    [[nodiscard]] bool add(const net::SocketAddress &address) noexcept;
    void clear() noexcept { count_ = 0; }

    [[nodiscard]] std::uint8_t size() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] const net::SocketAddress &operator[](std::size_t index) const noexcept;
    [[nodiscard]] std::span<const net::SocketAddress> view() const noexcept {
        return std::span<const net::SocketAddress>(entries_.data(), count_);
    }

private:
    std::array<net::SocketAddress, kMaxDnsNameservers> entries_{};
    std::uint8_t count_ = 0;
};

class DnsSearchList {
public:
    void clear() noexcept;
    [[nodiscard]] std::uint8_t size() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] std::string_view operator[](std::size_t index) const noexcept;

private:
    friend class ResolverConfigParser;

    [[nodiscard]] bool add(std::string_view domain) noexcept;

    std::array<char, kMaxDnsSearchBytes> storage_{};
    std::array<std::uint16_t, kMaxDnsSearchDomains> offsets_{};
    std::array<std::uint16_t, kMaxDnsSearchDomains> lengths_{};
    std::uint16_t storage_size_ = 0;
    std::uint8_t count_ = 0;
};

enum class ResolverUnsupportedFeature : std::uint32_t {
    None = 0,
    Search = 1U << 0U,
    Ndots = 1U << 1U,
    SortList = 1U << 2U,
    Option = 1U << 3U,
    Directive = 1U << 4U,
};

[[nodiscard]] constexpr ResolverUnsupportedFeature operator|(ResolverUnsupportedFeature left,
                                                             ResolverUnsupportedFeature right) noexcept {
    return static_cast<ResolverUnsupportedFeature>(static_cast<std::uint32_t>(left) |
                                                   static_cast<std::uint32_t>(right));
}

constexpr ResolverUnsupportedFeature &operator|=(ResolverUnsupportedFeature &left,
                                                 ResolverUnsupportedFeature right) noexcept {
    left = left | right;
    return left;
}

[[nodiscard]] constexpr bool has_unsupported_feature(ResolverUnsupportedFeature features,
                                                     ResolverUnsupportedFeature feature) noexcept {
    return (static_cast<std::uint32_t>(features) & static_cast<std::uint32_t>(feature)) != 0;
}

struct SystemResolverConfig {
    DnsNameserverList nameservers{};
    DnsSearchList search{};
    std::chrono::milliseconds timeout{5000};
    std::uint8_t attempts = 2;
    std::uint8_t ndots = 1;
    bool rotate = false;
    ResolverUnsupportedFeature unsupported = ResolverUnsupportedFeature::None;
    std::size_t first_unsupported_line = 0;
};

enum class ResolverConfigErrorCode : std::uint8_t {
    InvalidArgument,
    CalledFromEventLoop,
    OpenFailed,
    ReadFailed,
    FileTooLarge,
    NoNameserver,
    TooManyNameservers,
    InvalidNameserver,
    InvalidDirective,
    InvalidOption,
    SearchListTooLarge,
};

struct ResolverConfigError {
    ResolverConfigErrorCode code = ResolverConfigErrorCode::InvalidArgument;
    std::size_t line = 0;
    std::size_t column = 0;
    int system_error = 0;
};

[[nodiscard]] std::string_view resolver_config_error_name(ResolverConfigErrorCode code) noexcept;

// Parses nameserver, search/domain, and options from supplied text without filesystem I/O.
// timeout, attempts, and rotate are actionable by DnsClient. search/domain and ndots are
// retained for callers but marked unsupported because DnsClient does not expand query names.
// sortlist, unknown options, and unknown directives are ignored and explicitly flagged.
[[nodiscard]] std::expected<SystemResolverConfig, ResolverConfigError>
parse_resolver_config(std::string_view text) noexcept;

// Synchronously reads and parses a resolver configuration file. This function rejects calls
// made from an EventLoop thread so filesystem I/O cannot accidentally enter a request path.
// It has no implicit fallback nameserver and never reloads the file after returning.
[[nodiscard]] std::expected<SystemResolverConfig, ResolverConfigError>
load_system_resolver_config(const char *path = "/etc/resolv.conf") noexcept;

} // namespace fiber::dns

#endif // FIBER_DNS_DNS_RESOLVER_CONFIG_H
