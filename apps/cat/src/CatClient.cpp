#include <fiber/cat/CatClient.h>

#include <limits>
#include <new>
#include <utility>

#include <dns/DnsResolver.h>
#include <net/IpAddress.h>

#include "CatClientImpl.h"

namespace fiber::cat {

namespace {

bool valid_options(const CatClientOptions &options) noexcept {
    return options.max_queued_messages > 0 &&
           options.max_queued_messages <= std::numeric_limits<std::uint32_t>::max() && options.max_queued_bytes > 0 &&
           options.max_queued_bytes <= std::numeric_limits<std::uint32_t>::max() &&
           options.max_router_response_bytes > 0 && options.max_collectors > 0 && options.max_batch_messages > 0 &&
           options.max_batch_messages <= 16 && options.max_batch_bytes > 0 && options.max_send_bytes_per_pump > 0 &&
           options.max_send_calls_per_pump > 0 && options.router_connect_timeout > std::chrono::milliseconds::zero() &&
           options.router_request_timeout > std::chrono::milliseconds::zero() &&
           options.router_refresh_interval > std::chrono::milliseconds::zero() &&
           options.collector_connect_timeout > std::chrono::milliseconds::zero() &&
           options.collector_write_timeout > std::chrono::milliseconds::zero() &&
           options.reconnect_initial_delay > std::chrono::milliseconds::zero() &&
           options.reconnect_max_delay >= options.reconnect_initial_delay &&
           options.shutdown_drain_timeout >= std::chrono::milliseconds::zero();
}

bool needs_resolver(const CatClientConfig &config) noexcept {
    for (const CatRouterEndpoint &router: config.routers()) {
        net::IpAddress literal;
        if (!net::IpAddress::parse(router.host, literal)) {
            return true;
        }
    }
    return false;
}

} // namespace

std::expected<std::unique_ptr<CatClient>, CatClientCreateError>
CatClient::create(event::EventLoop &sender_loop, CatClientConfig config, CatClientOptions options,
                  dns::AddressResolver *resolver) noexcept {
    if (!valid_options(options)) {
        return std::unexpected(CatClientCreateError::InvalidOptions);
    }
    if (needs_resolver(config) && (!resolver || !resolver->valid() || &resolver->loop() != &sender_loop)) {
        return std::unexpected(CatClientCreateError::InvalidResolver);
    }

    auto *raw_impl =
            new (std::nothrow) detail::CatClientImpl(sender_loop, std::move(config), std::move(options), resolver);
    if (!raw_impl) {
        return std::unexpected(CatClientCreateError::NoMemory);
    }
    std::shared_ptr<detail::CatClientImpl> impl(raw_impl);
    auto *raw_client = new (std::nothrow) CatClient(std::move(impl));
    if (!raw_client) {
        return std::unexpected(CatClientCreateError::NoMemory);
    }
    return std::unique_ptr<CatClient>(raw_client);
}

CatClient::~CatClient() {
    if (impl_) {
        impl_->begin_stop();
    }
}

common::IoResult<void> CatClient::start() noexcept { return impl_->start(); }

async::Task<void> CatClient::shutdown() noexcept { co_await impl_->shutdown(); }

CatClientState CatClient::state() const noexcept { return impl_->state(); }

CatClientStats CatClient::stats() const noexcept { return impl_->stats(); }

event::EventLoop &CatClient::sender_loop() const noexcept { return impl_->sender_loop(); }

std::shared_ptr<detail::CatClientCore> CatClient::core() const noexcept { return impl_; }

} // namespace fiber::cat
