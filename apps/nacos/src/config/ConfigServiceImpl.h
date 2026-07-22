#ifndef FIBER_NACOS_CONFIG_CONFIG_SERVICE_IMPL_H
#define FIBER_NACOS_CONFIG_CONFIG_SERVICE_IMPL_H

#include <expected>
#include <memory>

#include <fiber/nacos/ConfigService.h>

#include "../detail/NacosServiceDependencies.h"

namespace fiber::nacos::detail {

[[nodiscard]] std::expected<std::unique_ptr<ConfigService>, NacosCreateError>
create_config_service(NacosServiceDependencies dependencies, ConfigServiceOptions options);

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_CONFIG_CONFIG_SERVICE_IMPL_H
