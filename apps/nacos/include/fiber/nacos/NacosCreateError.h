#ifndef FIBER_NACOS_NACOS_CREATE_ERROR_H
#define FIBER_NACOS_NACOS_CREATE_ERROR_H

#include <cstdint>

namespace fiber::nacos {

enum class NacosCreateErrorCode : std::uint8_t {
    InvalidOptions,
    InvalidState,
    NoMem,
    DnsResolverRequired,
    InvalidDnsResolver,
};

struct NacosCreateError {
    NacosCreateErrorCode code = NacosCreateErrorCode::InvalidOptions;
};

} // namespace fiber::nacos

#endif // FIBER_NACOS_NACOS_CREATE_ERROR_H
