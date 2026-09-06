#ifndef FIBER_HTTP_CLIENT_HTTP3_TYPES_H
#define FIBER_HTTP_CLIENT_HTTP3_TYPES_H

#include <cstdint>
#include <string_view>

#include "ClientHttpTypes.h"

namespace fiber::http {

enum class Http3ExtendedConnectSupport : std::uint8_t {
    Unknown,
    Disabled,
    Enabled,
};

enum class Http3RequestOutcome : std::uint8_t {
    NotSent,
    Rejected,
    PossiblyProcessed,
    Complete,
};

} // namespace fiber::http

#endif // FIBER_HTTP_CLIENT_HTTP3_TYPES_H
