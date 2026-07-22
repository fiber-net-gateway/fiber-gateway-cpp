#ifndef FIBER_NACOS_DETAIL_NACOS_SERVICE_DEPENDENCIES_H
#define FIBER_NACOS_DETAIL_NACOS_SERVICE_DEPENDENCIES_H

#include <cstdint>
#include <memory>

#include <event/EventLoop.h>
#include <fiber/nacos/NacosAuthAccess.h>
#include <fiber/nacos/NacosClientConfig.h>

namespace fiber::nacos::detail {

struct NacosServiceDependencies {
    event::EventLoop *loop = nullptr;
    std::shared_ptr<const NacosClientConfig> config;
    NacosAuthSubscriber auth;
};

enum class NacosServiceState : std::uint8_t {
    Created,
    Running,
    Stopping,
    Stopped,
};

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_DETAIL_NACOS_SERVICE_DEPENDENCIES_H
