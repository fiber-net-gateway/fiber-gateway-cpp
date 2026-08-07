#ifndef FIBER_NACOS_NACOS_AUTH_ACCESS_H
#define FIBER_NACOS_NACOS_AUTH_ACCESS_H

#include <cstdint>
#include <string>

#include <fiber/async/Watch.h>

namespace fiber::nacos {

enum class NacosAuthAccessKind : std::uint8_t {
    NotConfigured,
    InitialFailed,
    Present,
    Stopped,
};

struct NacosAuthAccess {
    NacosAuthAccessKind kind = NacosAuthAccessKind::NotConfigured;
    std::string access_token;
};

using NacosAuthSubscriber = async::Watch<NacosAuthAccess>::Subscriber;

} // namespace fiber::nacos

#endif // FIBER_NACOS_NACOS_AUTH_ACCESS_H
