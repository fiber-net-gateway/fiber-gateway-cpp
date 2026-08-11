#ifndef FIBER_NACOS_DETAIL_NACOS_SERVER_ADDRESS_RESOLVER_H
#define FIBER_NACOS_DETAIL_NACOS_SERVER_ADDRESS_RESOLVER_H

#include <cstdint>
#include <expected>
#include <span>
#include <string>

#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/dns/DnsResolver.h>
#include <fiber/event/EventLoop.h>
#include <fiber/nacos/NacosClientConfig.h>
#include <fiber/net/SocketAddress.h>

namespace fiber::nacos::detail {

struct NacosServerResolveError {
    common::IoErr io_error = common::IoErr::Unknown;
    dns::ResolveStatus status = dns::ResolveStatus::ServerFailure;
};

class NacosServerAddressResolver : public common::NonCopyable, public common::NonMovable {
public:
    NacosServerAddressResolver() noexcept = default;

    [[nodiscard]] bool init(event::EventLoop &loop, dns::AddressResolver *resolver) noexcept;

    [[nodiscard]] async::Task<std::expected<std::span<const net::SocketAddress>, NacosServerResolveError>>
    resolve(const NacosServerHost &host, std::uint16_t port) noexcept;

private:
    event::EventLoop *loop_ = nullptr;
    dns::AddressResolver *resolver_ = nullptr;
    dns::EndpointResolveResult result_{};
    net::SocketAddress literal_result_{};
};

[[nodiscard]] std::string make_nacos_server_authority(const NacosServerHost &host, std::uint16_t port);

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_DETAIL_NACOS_SERVER_ADDRESS_RESOLVER_H
