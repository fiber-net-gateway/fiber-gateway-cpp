#ifndef FIBER_NACOS_DISCOVERY_SERVICE_DISCOVERY_TYPES_H
#define FIBER_NACOS_DISCOVERY_SERVICE_DISCOVERY_TYPES_H

#include <cstdint>
#include <string_view>

namespace fiber::nacos {

struct ServiceKeyView {
    std::string_view service_name;
    std::string_view group;
};

enum class ServiceReadyError : std::uint8_t {
    Closed,
    Retired,
    Shutdown,
};

template<typename StatePtr, typename UpdateResult>
struct ServiceStateCreateResult {
    StatePtr state;
    UpdateResult result;
};

} // namespace fiber::nacos

#endif // FIBER_NACOS_DISCOVERY_SERVICE_DISCOVERY_TYPES_H
