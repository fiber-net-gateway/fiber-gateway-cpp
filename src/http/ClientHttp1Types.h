#ifndef FIBER_HTTP_CLIENT_HTTP1_TYPES_H
#define FIBER_HTTP_CLIENT_HTTP1_TYPES_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "HttpBodySpec.h"
#include "HttpCommon.h"
#include "HttpHeaders.h"

namespace fiber::http {

struct Http1ClientExchangeOptions {
    std::chrono::milliseconds write_timeout{30000};
    std::chrono::milliseconds response_header_timeout{10000};
    std::chrono::milliseconds response_body_timeout{30000};
    std::size_t response_header_init_size = 8 * 1024;
    std::size_t response_header_large_size = 32 * 1024;
    std::size_t response_header_large_num = 4;
};

struct Http1RequestHead {
    HttpMethod method = HttpMethod::Unknown;
    std::string_view target{};
    const HttpHeaders *headers = nullptr;
    HttpBodySpec body = HttpBodySpec::None();
};

struct Http1ResponseHead {
    HttpVersion version = HttpVersion::HTTP_1_1;
    int status_code = 0;
    std::string_view reason{};
    HttpHeaders headers;

    explicit Http1ResponseHead(mem::BufPool &pool) : headers(pool) {}

    [[nodiscard]] bool is_informational() const noexcept { return status_code >= 100 && status_code < 200; }
};

} // namespace fiber::http

#endif // FIBER_HTTP_CLIENT_HTTP1_TYPES_H
