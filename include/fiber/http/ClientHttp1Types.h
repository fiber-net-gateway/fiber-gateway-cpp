#ifndef FIBER_HTTP_CLIENT_HTTP1_TYPES_H
#define FIBER_HTTP_CLIENT_HTTP1_TYPES_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ClientHttpTypes.h"

namespace fiber::http {

struct Http1ClientExchangeOptions {
    std::size_t response_header_init_size = 8 * 1024;
    std::size_t response_header_large_size = 32 * 1024;
    std::size_t response_header_large_num = 4;
};

} // namespace fiber::http

#endif // FIBER_HTTP_CLIENT_HTTP1_TYPES_H
