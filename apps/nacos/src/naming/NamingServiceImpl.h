#ifndef FIBER_NACOS_NAMING_NAMING_SERVICE_IMPL_H
#define FIBER_NACOS_NAMING_NAMING_SERVICE_IMPL_H

#include <expected>
#include <memory>

#include <fiber/nacos/NamingService.h>

#include "../detail/NacosServiceDependencies.h"

namespace fiber::nacos::detail {

[[nodiscard]] std::expected<std::unique_ptr<NamingService>, NacosCreateError>
create_naming_service(NacosServiceDependencies dependencies, NamingServiceOptions options);

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_NAMING_NAMING_SERVICE_IMPL_H
