#ifndef FIBER_LITE_NGINX_LOGGING_ACCESS_LOG_SCRIPT_LIBRARY_H
#define FIBER_LITE_NGINX_LOGGING_ACCESS_LOG_SCRIPT_LIBRARY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "common/IoError.h"
#include "http_script/RouteScriptLibrary.h"
#include "http_script/ScriptExchangeCtx.h"

namespace fiber::script::std_lib {
class StdLibrary;
}

namespace fiber::lite_nginx::logging {

struct AccessLogScriptData {
    std::uint64_t request_id = 0;
    std::uint64_t body_bytes_sent = 0;
    std::uint64_t request_time_us = 0;
    std::uint64_t upstream_time_us = 0;
    std::string_view server_name;
    std::string_view location_pattern;
    std::string_view outcome;
    std::string_view upstream_host;
    std::uint16_t upstream_port = 0;
    int status = 0;
    int upstream_status = 0;
    fiber::common::IoErr upstream_error = fiber::common::IoErr::None;
    bool upstream_started = false;
};

class AccessLogEvalContext final : public fiber::http_script::ScriptExchangeCtx {
public:
    AccessLogEvalContext(fiber::http::HttpExchange &exchange, fiber::script::GcHeap &heap,
                         fiber::http_script::ScriptConnectionInfo connection, const AccessLogScriptData &data) noexcept
        : ScriptExchangeCtx(exchange, heap, connection), data_(&data) {}

    [[nodiscard]] const AccessLogScriptData &data() const noexcept { return *data_; }

private:
    const AccessLogScriptData *data_;
};

class AccessLogScriptLibrary final : public fiber::http_script::RouteScriptLibrary {
public:
    AccessLogScriptLibrary(fiber::script::std_lib::StdLibrary &shared, const std::vector<std::string> &path_var_names);

    const HostCallable *resolve_constant(std::string_view namespace_name, std::string_view key) const override;

private:
    enum class Field : std::uint8_t {
        RequestId,
        Server,
        Location,
        Status,
        BodyBytesSent,
        RequestTimeUs,
        Outcome,
        UpstreamHost,
        UpstreamPort,
        UpstreamStatus,
        UpstreamTimeUs,
        UpstreamError,
        Count,
    };

    struct FieldRef {
        Field field = Field::RequestId;
    };

    static fiber::script::AbiResult field_fn(void *userdata, const HostCallFrame &frame) noexcept;

    std::array<FieldRef, static_cast<std::size_t>(Field::Count)> fields_{};
    std::array<HostCallable, static_cast<std::size_t>(Field::Count)> callables_{};
};

} // namespace fiber::lite_nginx::logging

#endif // FIBER_LITE_NGINX_LOGGING_ACCESS_LOG_SCRIPT_LIBRARY_H
