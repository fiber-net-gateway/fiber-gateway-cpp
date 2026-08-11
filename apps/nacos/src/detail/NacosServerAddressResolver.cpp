#include "NacosServerAddressResolver.h"

#include <fiber/common/Assert.h>

namespace fiber::nacos::detail {

bool NacosServerAddressResolver::init(event::EventLoop &loop, dns::AddressResolver *resolver) noexcept {
    if (loop_ != nullptr || (resolver != nullptr && (!resolver->valid() || &resolver->loop() != &loop))) {
        return false;
    }
    if (resolver != nullptr && !result_.init()) {
        return false;
    }
    loop_ = &loop;
    resolver_ = resolver;
    return true;
}

async::Task<std::expected<std::span<const net::SocketAddress>, NacosServerResolveError>>
NacosServerAddressResolver::resolve(const NacosServerHost &host, std::uint16_t port) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(port != 0);

    if (host.is_ip_literal()) {
        literal_result_ = net::SocketAddress(host.literal_ip(), port);
        co_return std::span<const net::SocketAddress>(&literal_result_, 1);
    }
    if (resolver_ == nullptr) {
        co_return std::unexpected(NacosServerResolveError{.io_error = common::IoErr::NotSupported});
    }

    auto resolved = co_await resolver_->resolve(host.value(), port, result_);
    if (!resolved) {
        co_return std::unexpected(NacosServerResolveError{.io_error = resolved.error()});
    }
    if (*resolved != dns::ResolveStatus::Success || result_.record_count() == 0) {
        co_return std::unexpected(NacosServerResolveError{
                .io_error = common::IoErr::NotFound,
                .status = *resolved,
        });
    }
    co_return std::span<const net::SocketAddress>(result_.records(), result_.record_count());
}

std::string make_nacos_server_authority(const NacosServerHost &host, std::uint16_t port) {
    std::string result;
    if (host.is_ip_literal() && host.literal_ip().is_v6()) {
        result.push_back('[');
        result.append(host.value());
        result.push_back(']');
    } else {
        result.append(host.value());
    }
    result.push_back(':');
    result.append(std::to_string(port));
    return result;
}

} // namespace fiber::nacos::detail
