#ifndef FIBER_DNS_DNS_RESOLVER_H
#define FIBER_DNS_DNS_RESOLVER_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "../net/IpAddress.h"
#include "../net/SocketAddress.h"
#include "DnsResolverLocal.h"

namespace fiber::dns {

enum class AddressPolicy : std::uint8_t {
    V6First,
    V4First,
    V6Only,
    V4Only,
};

class AddressResolveResult : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        std::uint16_t max_records = 16;
        std::uint16_t max_name_storage = 512;
    };

    AddressResolveResult() noexcept = default;

    [[nodiscard]] bool init() noexcept { return init(Options{}); }
    [[nodiscard]] bool init(Options options) noexcept;
    void release() noexcept;
    void clear() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::string_view canonical_name() const noexcept { return canonical_name_; }
    [[nodiscard]] const net::IpAddress *records() const noexcept { return records_.get(); }
    [[nodiscard]] std::uint16_t record_count() const noexcept { return record_count_; }
    [[nodiscard]] std::uint16_t v4_count() const noexcept { return v4_count_; }
    [[nodiscard]] std::uint16_t v6_count() const noexcept { return v6_count_; }
    [[nodiscard]] std::chrono::steady_clock::time_point expire_at() const noexcept { return expire_at_; }

private:
    friend class DnsResolver;

    [[nodiscard]] common::IoErr assign_positive(std::string_view canonical_name,
                                                const net::IpAddress *records,
                                                std::uint16_t count,
                                                std::uint16_t v4_count,
                                                std::uint16_t v6_count,
                                                std::chrono::steady_clock::time_point expire_at) noexcept;
    [[nodiscard]] common::IoErr assign_canonical(std::string_view canonical_name) noexcept;

    Options options_{};
    std::unique_ptr<net::IpAddress[]> records_{};
    std::unique_ptr<char[]> name_storage_{};
    std::string_view canonical_name_{};
    std::uint16_t record_count_ = 0;
    std::uint16_t v4_count_ = 0;
    std::uint16_t v6_count_ = 0;
    std::chrono::steady_clock::time_point expire_at_{};
};

class EndpointResolveResult : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        std::uint16_t max_records = 16;
        std::uint16_t max_name_storage = 512;
    };

    EndpointResolveResult() noexcept = default;

    [[nodiscard]] bool init() noexcept { return init(Options{}); }
    [[nodiscard]] bool init(Options options) noexcept;
    void release() noexcept;
    void clear() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::string_view canonical_name() const noexcept { return canonical_name_; }
    [[nodiscard]] const net::SocketAddress *records() const noexcept { return records_.get(); }
    [[nodiscard]] std::uint16_t record_count() const noexcept { return record_count_; }
    [[nodiscard]] std::chrono::steady_clock::time_point expire_at() const noexcept { return expire_at_; }

private:
    friend class AddressResolver;

    [[nodiscard]] common::IoErr assign_positive(std::string_view canonical_name,
                                                const net::SocketAddress *records,
                                                std::uint16_t count,
                                                std::chrono::steady_clock::time_point expire_at) noexcept;
    [[nodiscard]] common::IoErr assign_canonical(std::string_view canonical_name) noexcept;

    Options options_{};
    std::unique_ptr<net::SocketAddress[]> records_{};
    std::unique_ptr<char[]> name_storage_{};
    std::string_view canonical_name_{};
    std::uint16_t record_count_ = 0;
    std::chrono::steady_clock::time_point expire_at_{};
};

class DnsResolver : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        AddressPolicy default_policy = AddressPolicy::V6First;
        ResolveResult::Options family_result_options{};
    };

    DnsResolver() noexcept = default;

    [[nodiscard]] bool init(DnsResolverLocal &local) noexcept { return init(local, Options{}); }
    [[nodiscard]] bool init(DnsResolverLocal &local, Options options) noexcept;
    void release() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] event::EventLoop &loop() const noexcept;

    [[nodiscard]] async::Task<common::IoResult<ResolveStatus>> resolve_host(std::string_view host,
                                                                            AddressResolveResult &out) noexcept;
    [[nodiscard]] async::Task<common::IoResult<ResolveStatus>> resolve_host(std::string_view host,
                                                                            AddressPolicy policy,
                                                                            AddressResolveResult &out) noexcept;

private:
    DnsResolverLocal *local_ = nullptr;
    Options options_{};
};

class AddressResolver : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        AddressPolicy default_policy = AddressPolicy::V6First;
        AddressResolveResult::Options address_result_options{};
    };

    AddressResolver() noexcept = default;

    [[nodiscard]] bool init(DnsResolver &resolver) noexcept { return init(resolver, Options{}); }
    [[nodiscard]] bool init(DnsResolver &resolver, Options options) noexcept;
    void release() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] event::EventLoop &loop() const noexcept;

    [[nodiscard]] async::Task<common::IoResult<ResolveStatus>> resolve(std::string_view host,
                                                                       std::uint16_t port,
                                                                       EndpointResolveResult &out) noexcept;
    [[nodiscard]] async::Task<common::IoResult<ResolveStatus>> resolve(std::string_view host,
                                                                       std::uint16_t port,
                                                                       AddressPolicy policy,
                                                                       EndpointResolveResult &out) noexcept;

private:
    DnsResolver *resolver_ = nullptr;
    Options options_{};
};

} // namespace fiber::dns

#endif // FIBER_DNS_DNS_RESOLVER_H
