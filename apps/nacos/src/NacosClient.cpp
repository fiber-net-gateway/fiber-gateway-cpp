#include <fiber/nacos/NacosClient.h>

#include <new>
#include <utility>

#include <fiber/dns/DnsResolver.h>

#include "detail/NacosClientImpl.h"

namespace fiber::nacos {

std::expected<std::unique_ptr<NacosClient>, NacosCreateError> NacosClient::create_impl(event::EventLoop &loop,
                                                                                       dns::AddressResolver *resolver,
                                                                                       NacosClientConfig config,
                                                                                       NacosClientOptions options) {
    if (!detail::NacosClientImpl::valid_options(options)) {
        return std::unexpected(NacosCreateError{.code = NacosCreateErrorCode::InvalidOptions});
    }
    if (config.has_hostname_server() && resolver == nullptr) {
        return std::unexpected(NacosCreateError{.code = NacosCreateErrorCode::DnsResolverRequired});
    }
    if (resolver != nullptr && (!resolver->valid() || &resolver->loop() != &loop)) {
        return std::unexpected(NacosCreateError{.code = NacosCreateErrorCode::InvalidDnsResolver});
    }

    auto impl = std::unique_ptr<detail::NacosClientImpl>(
            new (std::nothrow) detail::NacosClientImpl(loop, resolver, std::move(config), options));
    if (!impl || !impl->init()) {
        return std::unexpected(NacosCreateError{.code = NacosCreateErrorCode::NoMem});
    }
    auto client = std::unique_ptr<NacosClient>(new (std::nothrow) NacosClient(std::move(impl)));
    if (!client) {
        return std::unexpected(NacosCreateError{.code = NacosCreateErrorCode::NoMem});
    }
    return client;
}

std::expected<std::unique_ptr<NacosClient>, NacosCreateError>
NacosClient::create(event::EventLoop &loop, NacosClientConfig config, NacosClientOptions options) {
    return create_impl(loop, nullptr, std::move(config), options);
}

std::expected<std::unique_ptr<NacosClient>, NacosCreateError> NacosClient::create(event::EventLoop &loop,
                                                                                  dns::AddressResolver &resolver,
                                                                                  NacosClientConfig config,
                                                                                  NacosClientOptions options) {
    return create_impl(loop, &resolver, std::move(config), options);
}

NacosClient::NacosClient(std::unique_ptr<detail::NacosClientImpl> impl) noexcept : impl_(std::move(impl)) {}

NacosClient::~NacosClient() = default;

common::IoResult<void> NacosClient::start() noexcept { return impl_->start(); }

async::Task<void> NacosClient::shutdown() noexcept { co_await impl_->shutdown(); }

NacosClient::AuthSubscriber NacosClient::subscribe_auth() { return impl_->subscribe_auth(); }

event::EventLoop &NacosClient::loop() const noexcept { return impl_->loop(); }

const NacosClientConfig &NacosClient::config() const noexcept { return impl_->config(); }

std::expected<detail::NacosServiceDependencies, NacosCreateError> NacosClient::service_dependencies() {
    return impl_->service_dependencies();
}

} // namespace fiber::nacos
