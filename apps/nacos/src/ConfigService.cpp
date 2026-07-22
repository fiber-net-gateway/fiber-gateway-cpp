#include <fiber/nacos/ConfigService.h>

#include <new>
#include <utility>

#include <fiber/nacos/NacosClient.h>

#include "config/ConfigServiceImpl.h"

namespace fiber::nacos {

std::expected<std::unique_ptr<ConfigService>, NacosCreateError> ConfigService::create(NacosClient &client,
                                                                                      ConfigServiceOptions options) {
    if (!detail::ConfigServiceImpl::valid_options(options)) {
        return std::unexpected(NacosCreateError{.code = NacosCreateErrorCode::InvalidOptions});
    }
    auto dependencies = client.service_dependencies();
    if (!dependencies) {
        return std::unexpected(dependencies.error());
    }
    auto service = std::unique_ptr<ConfigService>(
            new (std::nothrow) detail::ConfigServiceImpl(std::move(*dependencies), std::move(options)));
    if (!service) {
        return std::unexpected(NacosCreateError{.code = NacosCreateErrorCode::NoMem});
    }
    return service;
}

} // namespace fiber::nacos
