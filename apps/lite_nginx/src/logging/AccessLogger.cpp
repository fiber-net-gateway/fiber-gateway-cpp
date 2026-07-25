#include "AccessLogger.h"

#include <atomic>
#include <chrono>
#include <string_view>

#include "http/HttpExchange.h"
#include "log/Log.h"
#include "script/JsGc.h"
#include "script/Script.h"
#include "script/std/NodeText.h"

#include "AccessLogScriptExtension.h"

namespace fiber::lite_nginx::logging {
namespace {

DEFINE_LOGGER(LOG_ACCESS_ERROR, "lite_nginx.access_error");

std::atomic<std::uint64_t> g_next_request_id{1};

std::string_view access_outcome(const fiber::http::HttpResponseStats &stats,
                                const RequestLogContext &context) noexcept {
    if (context.client_aborted) {
        return "client_aborted";
    }
    if (context.upstream_error != fiber::common::IoErr::None) {
        return "upstream_error";
    }
    if (stats.terminal_error != fiber::common::IoErr::None) {
        return "io_error";
    }
    if (!stats.header_sent) {
        return "no_response";
    }
    return stats.completed ? "ok" : "incomplete";
}

std::uint64_t elapsed_us(std::chrono::steady_clock::time_point begin,
                         std::chrono::steady_clock::time_point end) noexcept {
    if (end <= begin) {
        return 0;
    }
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
}

} // namespace

std::uint64_t next_request_id() noexcept { return g_next_request_id.fetch_add(1, std::memory_order_relaxed); }

std::expected<void, runtime::RuntimeError> AccessLogger::bind(const runtime::RuntimeConfig &runtime) {
    logs_.clear();
    logs_.reserve(runtime.access_logs.size());
    auto &manager = fiber::log::LoggerManager::global();
    for (const auto &access_log: runtime.access_logs) {
        const fiber::log::Logger *logger = manager.find_logger(access_log.logger_name);
        if (logger == nullptr) {
            return std::unexpected(runtime::RuntimeError{
                    .message = "access_log logger was not initialized: " + access_log.logger_name,
                    .location = access_log.location,
            });
        }
        logs_.push_back({.runtime = &access_log, .logger = logger});
    }
    return {};
}

void AccessLogger::write(fiber::http::HttpExchange &exchange, const RequestLogContext &context,
                         std::chrono::steady_clock::time_point finished_at) const noexcept {
    if (context.access_log == runtime::kDisabledAccessLog || context.access_log >= logs_.size()) {
        return;
    }
    const BoundAccessLog &bound = logs_[context.access_log];
    if (bound.runtime == nullptr || bound.logger == nullptr || !bound.logger->enabled(fiber::log::LogLevel::Info)) {
        return;
    }

    auto emit = [&](std::string_view message) noexcept {
        fiber::log::LogLine(*bound.logger, fiber::log::LogLevel::Info, __FILE__, __LINE__, __func__) << message;
    };
    if (!bound.runtime->template_script) {
        emit(bound.runtime->literal_message);
        return;
    }

    const auto &stats = exchange.response_stats();
    const std::uint64_t upstream_time =
            context.upstream_started ? elapsed_us(context.upstream_started_at, finished_at) : 0;
    const AccessLogScriptData data{
            .request_id = context.request_id,
            .body_bytes_sent = stats.body_bytes_sent,
            .request_time_us = elapsed_us(context.started_at, finished_at),
            .upstream_time_us = upstream_time,
            .server_name = context.server_name,
            .location_pattern = context.location_pattern,
            .outcome = access_outcome(stats, context),
            .upstream_host = context.upstream_host,
            .upstream_port = context.upstream_port,
            .status = stats.status_code,
            .upstream_status = context.upstream_status,
            .upstream_error = context.upstream_error,
            .upstream_started = context.upstream_started,
    };
    fiber::script::GcHeap heap(exchange.pool());
    AccessLogEvalContext script_context(exchange, heap, context.connection, data);
    script_context.set_path_vars(context.path_vars);
    auto result =
            bound.runtime->template_script->exec_sync(fiber::script::JsValue::make_undefined(), &script_context, heap);
    if (!result.is_value()) {
        LOG(LOG_ACCESS_ERROR, ERROR) << "request_id=" << context.request_id
                                     << " access_log template evaluation failed logger="
                                     << fiber::log::quoted(bound.runtime->logger_name);
        return;
    }
    std::string_view message;
    if (!fiber::script::std_lib::string_utf8_view(result.value(), message)) {
        LOG(LOG_ACCESS_ERROR, ERROR) << "request_id=" << context.request_id
                                     << " access_log template returned non-string logger="
                                     << fiber::log::quoted(bound.runtime->logger_name);
        return;
    }
    emit(message);
}

} // namespace fiber::lite_nginx::logging
