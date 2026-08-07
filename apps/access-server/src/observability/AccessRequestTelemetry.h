#ifndef FIBER_ACCESS_SERVER_ACCESS_REQUEST_TELEMETRY_H
#define FIBER_ACCESS_SERVER_ACCESS_REQUEST_TELEMETRY_H

#include "AccessServerMetrics.h"
#include "AccessTraceState.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include <fiber/cat/MessageTrace.h>
#include <fiber/cat/Transaction.h>
#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/http/HttpHeaders.h>
#include <fiber/http_script/ScriptExchangeCtx.h>

namespace fiber::cat {
class CatClient;
}

namespace fiber::http {
class HttpExchange;
} // namespace fiber::http

namespace fiber::http_script {
class ConstPackage;
}

namespace fiber::access_server {

struct Exception;
struct CompiledRoute;
struct ProxyUpstreamEndpoint;

class AccessProviderTransaction final : public common::NonCopyable {
public:
    AccessProviderTransaction() noexcept = default;
    AccessProviderTransaction(AccessProviderTransaction &&other) noexcept;
    AccessProviderTransaction &operator=(AccessProviderTransaction &&other) noexcept;
    ~AccessProviderTransaction();

    [[nodiscard]] bool valid() const noexcept;

    void add_upstream(std::string_view upstream, std::size_t attempt) noexcept;
    void add_connection_reuse(std::uint64_t reuse_count) noexcept;
    void fail(std::string_view phase, common::IoErr error) noexcept;
    void call_error(const Exception &exception, std::string_view phase, common::IoErr error) noexcept;
    void complete(int status_code) noexcept;

private:
    friend class AccessRequestTelemetry;

    explicit AccessProviderTransaction(cat::Transaction transaction) noexcept : transaction_(std::move(transaction)) {}
    void cancel_pending() noexcept;

    cat::Transaction transaction_;
};

// Request-lifetime owner for access-server execution state and optional observability sinks.
class AccessRequestTelemetry final : public common::NonCopyable, public common::NonMovable {
public:
    AccessRequestTelemetry(http::HttpExchange &exchange, AccessServerMetrics::Worker *metrics,
                           cat::CatClient *cat_client) noexcept;
    ~AccessRequestTelemetry();

    void set_project(std::string_view project, std::string_view effective_host,
                     std::string_view context_cluster) noexcept;
    void set_route(const CompiledRoute &route) noexcept;
    void record_exception(const Exception &exception) noexcept;
    void record_upstream_exception(const Exception &exception) noexcept;
    void record_response_error(common::IoErr error) noexcept;
    void mark_io_error(common::IoErr error) noexcept;
    void set_upstream(const ProxyUpstreamEndpoint &endpoint) noexcept;
    [[nodiscard]] AccessProviderTransaction start_provider_transaction(std::string_view name) noexcept;

    [[nodiscard]] std::string_view trace_id() const noexcept;
    [[nodiscard]] std::string_view trace_parent() const noexcept { return trace_parent_; }
    [[nodiscard]] std::optional<std::string_view> trace_context(std::string_view key) const noexcept;
    [[nodiscard]] common::IoResult<void> bind_trace_context(const http_script::ConstPackage &constants) noexcept;
    [[nodiscard]] common::IoResult<void> put_trace_context(std::string_view key, std::string_view value) noexcept;
    void remove_trace_context(std::string_view key) noexcept;
    [[nodiscard]] http_script::ScriptExchangeCtx &script_context() noexcept { return script_context_; }
    [[nodiscard]] const http_script::ScriptExchangeCtx &script_context() const noexcept { return script_context_; }
    [[nodiscard]] http::HttpHeaders &response_headers() noexcept { return response_headers_; }
    [[nodiscard]] const http::HttpHeaders &response_headers() const noexcept { return response_headers_; }
    [[nodiscard]] bool finalize_response_headers() noexcept;
    [[nodiscard]] bool inject_upstream_headers(http::HttpHeaders &headers,
                                               AccessProviderTransaction &provider) noexcept;

private:
    [[nodiscard]] std::string_view copy_to_request_pool(std::string_view value) noexcept;
    void add_root_data(std::string_view key, std::string_view value) noexcept;
    void mark_failed(std::string_view error) noexcept;
    void update_transaction_name() noexcept;

    script::GcHeap script_heap_;
    http_script::ScriptExchangeCtx script_context_;
    http::HttpHeaders response_headers_;
    AccessTraceState trace_state_;
    AccessServerMetrics::Worker *metrics_ = nullptr;
    std::chrono::steady_clock::time_point started_{};
    cat::Transaction root_;
    std::optional<cat::MessageTraceContext> context_;
    const http_script::ConstPackage *const_package_ = nullptr;
    std::string_view trace_parent_;
    std::string_view project_;
    std::string_view route_;
    std::string_view cluster_;
    std::string_view upstream_;
    std::string_view error_;
    bool execution_failed_ = false;
    bool failure_recorded_ = false;
    bool exception_recorded_ = false;
    bool response_error_recorded_ = false;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_REQUEST_TELEMETRY_H
