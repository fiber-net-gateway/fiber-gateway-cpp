#ifndef FIBER_ACCESS_SERVER_ACCESS_REQUEST_TELEMETRY_H
#define FIBER_ACCESS_SERVER_ACCESS_REQUEST_TELEMETRY_H

#include "AccessServerMetrics.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include <common/IoError.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <fiber/cat/MessageTrace.h>
#include <fiber/cat/Transaction.h>

namespace fiber::cat {
class CatClient;
}

namespace fiber::http {
class HttpExchange;
class HttpHeaders;
} // namespace fiber::http

namespace fiber::access_server {

struct AccessError;
struct CompiledRoute;
struct ProxyUpstreamEndpoint;

class AccessProviderTransaction final : public common::NonCopyable {
public:
    AccessProviderTransaction() noexcept = default;
    AccessProviderTransaction(AccessProviderTransaction &&other) noexcept;
    AccessProviderTransaction &operator=(AccessProviderTransaction &&other) noexcept;
    ~AccessProviderTransaction();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] cat::Transaction *cat_parent() noexcept;

    void add_upstream(std::string_view upstream, std::size_t attempt) noexcept;
    void add_connection_reuse(std::uint64_t reuse_count) noexcept;
    void fail(std::string_view phase, common::IoErr error) noexcept;
    void complete(int status_code) noexcept;

private:
    friend class AccessRequestTelemetry;

    explicit AccessProviderTransaction(cat::Transaction transaction) noexcept : transaction_(std::move(transaction)) {}
    void cancel_pending() noexcept;

    std::optional<cat::Transaction> transaction_;
};

class AccessRequestTelemetry final : public common::NonCopyable, public common::NonMovable {
public:
    AccessRequestTelemetry(http::HttpExchange &exchange, AccessServerMetrics::Worker *metrics,
                           cat::CatClient *cat_client) noexcept;
    ~AccessRequestTelemetry();

    void set_project(std::string_view project, std::string_view effective_host,
                     std::string_view context_cluster) noexcept;
    void set_route(const CompiledRoute &route) noexcept;
    void set_error(const AccessError &error) noexcept;
    void set_upstream(const ProxyUpstreamEndpoint &endpoint) noexcept;
    [[nodiscard]] AccessProviderTransaction start_provider_transaction(std::string_view name) noexcept;

    [[nodiscard]] std::string_view trace_id() const noexcept;
    [[nodiscard]] bool inject_response_headers(http::HttpHeaders &headers) const noexcept;
    [[nodiscard]] bool inject_upstream_headers(http::HttpHeaders &headers,
                                               AccessProviderTransaction *provider = nullptr) noexcept;

private:
    [[nodiscard]] std::string_view copy_to_request_pool(std::string_view value) noexcept;
    void add_root_data(std::string_view key, std::string_view value) noexcept;
    void update_transaction_name() noexcept;

    http::HttpExchange *exchange_ = nullptr;
    AccessServerMetrics::Worker *metrics_ = nullptr;
    std::chrono::steady_clock::time_point started_{};
    std::optional<cat::Transaction> root_;
    std::optional<cat::MessageTraceContext> context_;
    std::string_view project_;
    std::string_view route_;
    std::string_view cluster_;
    std::string_view upstream_;
    std::string_view error_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_REQUEST_TELEMETRY_H
