#ifndef FIBER_LITE_NGINX_LOGGING_ACCESS_LOGGER_H
#define FIBER_LITE_NGINX_LOGGING_ACCESS_LOGGER_H

#include <chrono>
#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>
#include <vector>

#include "common/IoError.h"
#include "http_script/ScriptExchangeCtx.h"

#include "../runtime/RuntimeConfig.h"

namespace fiber::http {
class HttpExchange;
}

namespace fiber::log {
class Logger;
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
    runtime::AccessLogId access_log = runtime::kDisabledAccessLog;
    fiber::http_script::ScriptConnectionInfo connection;
    std::vector<std::pair<std::string_view, std::string_view>> path_vars;
    bool upstream_started = false;
    bool client_aborted = false;
};

[[nodiscard]] std::uint64_t next_request_id() noexcept;

class AccessLogger {
public:
    [[nodiscard]] std::expected<void, runtime::RuntimeError> bind(const runtime::RuntimeConfig &runtime);

    void write(fiber::http::HttpExchange &exchange, const RequestLogContext &context,
               std::chrono::steady_clock::time_point finished_at) const noexcept;

private:
    struct BoundAccessLog {
        const runtime::AccessLogRuntime *runtime = nullptr;
        const fiber::log::Logger *logger = nullptr;
    };

    std::vector<BoundAccessLog> logs_;
};

} // namespace fiber::lite_nginx::logging

#endif // FIBER_LITE_NGINX_LOGGING_ACCESS_LOGGER_H
