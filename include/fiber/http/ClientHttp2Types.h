#ifndef FIBER_HTTP_CLIENT_HTTP2_TYPES_H
#define FIBER_HTTP_CLIENT_HTTP2_TYPES_H

#include <cstdint>
#include <string_view>

#include "ClientHttpTypes.h"

namespace fiber::http {

enum class Http2ExtendedConnectSupport : std::uint8_t {
    Unknown,
    Disabled,
    Enabled,
};

} // namespace fiber::http

#endif // FIBER_HTTP_CLIENT_HTTP2_TYPES_H
