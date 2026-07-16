#include <fiber/nacos/NacosClient.h>

#include <new>
#include <utility>

#include "detail/NacosClientImpl.h"

namespace fiber::nacos {
namespace {

bool valid_options(const NacosClientOptions &options) noexcept {
    return options.connect_timeout > std::chrono::milliseconds::zero() &&
           options.request_timeout > std::chrono::milliseconds::zero() && options.max_auth_response_bytes > 0 &&
           options.retry_initial_delay > std::chrono::milliseconds::zero() &&
           options.retry_max_delay >= options.retry_initial_delay && options.refresh_percent > 0 &&
           options.refresh_percent < 100 && options.min_refresh_delay > std::chrono::milliseconds::zero();
}

} // namespace

std::expected<std::unique_ptr<NacosClient>, NacosCreateError>
NacosClient::create(event::EventLoop &loop, NacosClientConfig config, NacosClientOptions options) {
    if (!valid_options(options)) {
        return std::unexpected(NacosCreateError{.code = NacosCreateErrorCode::InvalidOptions});
    }

    auto impl = std::unique_ptr<detail::NacosClientImpl>(
            new (std::nothrow) detail::NacosClientImpl(loop, std::move(config), options));
    if (!impl) {
        return std::unexpected(NacosCreateError{.code = NacosCreateErrorCode::NoMem});
    }
    auto client = std::unique_ptr<NacosClient>(new (std::nothrow) NacosClient(std::move(impl)));
    if (!client) {
        return std::unexpected(NacosCreateError{.code = NacosCreateErrorCode::NoMem});
    }
    return client;
}

NacosClient::NacosClient(std::unique_ptr<detail::NacosClientImpl> impl) noexcept : impl_(std::move(impl)) {}

NacosClient::~NacosClient() = default;

common::IoResult<void> NacosClient::start() noexcept { return impl_->start(); }

async::Task<void> NacosClient::shutdown() noexcept { co_await impl_->shutdown(); }

NacosClient::AuthSubscriber NacosClient::subscribe_auth() { return impl_->subscribe_auth(); }

event::EventLoop &NacosClient::loop() const noexcept { return impl_->loop(); }

const NacosClientConfig &NacosClient::config() const noexcept { return impl_->config(); }

} // namespace fiber::nacos
