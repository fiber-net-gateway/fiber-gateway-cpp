#ifndef FIBER_LITE_NGINX_LOGGING_ACCESS_LOGGER_H
#define FIBER_LITE_NGINX_LOGGING_ACCESS_LOGGER_H

#include <chrono>
#include <cstdint>
#include <string_view>

#include "common/IoError.h"

namespace fiber::http {
class HttpExchange;
}

namespace fiber::lite_nginx::logging {

struct RequestLogContext {
    std::chrono::steady_clock::time_point started_at{};
    std::chrono::steady_clock::time_point upstream_started_at{};
    std::uint64_t request_id = 0;
    std::string_view server_name;
    std::string_view location_pattern;
    std::string_view upstream_host;
    std::uint16_t upstream_port = 0;
    int upstream_status = 0;
    fiber::common::IoErr upstream_error = fiber::common::IoErr::None;
    bool access_log = false;
    bool upstream_started = false;
};

[[nodiscard]] std::uint64_t next_request_id() noexcept;

void write_access_log(const fiber::http::HttpExchange &exchange, const RequestLogContext &context,
                      std::chrono::steady_clock::time_point finished_at) noexcept;

} // namespace fiber::lite_nginx::logging

#endif // FIBER_LITE_NGINX_LOGGING_ACCESS_LOGGER_H
