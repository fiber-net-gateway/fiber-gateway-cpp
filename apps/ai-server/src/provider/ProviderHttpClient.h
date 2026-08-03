#ifndef FIBER_AI_SERVER_PROVIDER_HTTP_CLIENT_H
#define FIBER_AI_SERVER_PROVIDER_HTTP_CLIENT_H

#include "ProviderConnectionManager.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include <async/Task.h>
#include <common/IoError.h>
#include <common/mem/BufPool.h>
#include <common/mem/IoBuf.h>
#include <common/mem/IoBufChain.h>
#include <http/ClientHttp1Exchange.h>

namespace fiber::cat {
class Transaction;
} // namespace fiber::cat

namespace fiber::ai_server {

enum class ProviderHttpErrorCode : std::uint8_t {
    InvalidEndpoint,
    NoServiceEndpoint,
    Dns,
    PoolShutdown,
    Connect,
    SendHeader,
    SendBody,
    ReadHeader,
    ReadBody,
    ResponseTooLarge,
    InvalidResponse,
    Count,
};

[[nodiscard]] std::string_view provider_http_error_code_name(ProviderHttpErrorCode code) noexcept;

struct ProviderHttpTiming {
    std::chrono::microseconds time_to_response_header{};
    std::chrono::microseconds time_to_first_token{};
    std::chrono::microseconds body_transfer{};
    bool response_header_observed = false;
    bool first_token_observed = false;
    bool body_transfer_observed = false;
};

struct ProviderHttpError {
    ProviderHttpErrorCode code = ProviderHttpErrorCode::Connect;
    common::IoErr io_error = common::IoErr::None;
    const char *message = nullptr;
    std::uint64_t failed_service_peer_id = 0;
    bool dns_backoff_hit = false;
    ProviderHttpTiming timing;
};

struct BufferedProviderResponse {
    int status_code = 0;
    std::string content_type;
    std::string retry_after;
    std::string request_id;
    mem::IoBuf body;
    ProviderLoadBalanceLease load_balance;
    ProviderHttpTiming timing;
};

class ProviderHttpResponseStream {
public:
    ProviderHttpResponseStream() noexcept = default;
    ~ProviderHttpResponseStream() = default;

    ProviderHttpResponseStream(const ProviderHttpResponseStream &) = delete;
    ProviderHttpResponseStream &operator=(const ProviderHttpResponseStream &) = delete;
    ProviderHttpResponseStream(ProviderHttpResponseStream &&) noexcept = default;
    ProviderHttpResponseStream &operator=(ProviderHttpResponseStream &&) noexcept = default;

    [[nodiscard]] int status_code() const noexcept { return status_code_; }
    [[nodiscard]] std::string_view content_type() const noexcept { return content_type_; }
    [[nodiscard]] std::string_view retry_after() const noexcept { return retry_after_; }
    [[nodiscard]] std::string_view request_id() const noexcept { return request_id_; }
    [[nodiscard]] bool valid() const noexcept { return upstream_ != nullptr; }
    [[nodiscard]] std::uint64_t service_peer_id() const noexcept { return connection_.load_balance.peer_id(); }
    [[nodiscard]] const ProviderHttpTiming &timing() const noexcept { return timing_; }

    [[nodiscard]] async::Task<common::IoResult<mem::IoBufChain>>
    read_body(std::size_t max_bytes, std::chrono::milliseconds timeout = std::chrono::seconds(300)) noexcept;
    void observe_first_token() noexcept;
    common::IoResult<void> abort(common::IoErr reason = common::IoErr::Canceled) noexcept;
    void report_instance(InstanceReportOutcome outcome) noexcept { connection_.load_balance.report(outcome); }
    [[nodiscard]] ProviderLoadBalanceLease take_load_balance() noexcept { return std::move(connection_.load_balance); }

private:
    ProviderHttpResponseStream(ProviderConnectionLease connection, std::unique_ptr<http::ClientHttp1Exchange> upstream,
                               int status_code, std::string content_type, std::string retry_after,
                               std::string request_id, std::chrono::steady_clock::time_point request_send_started,
                               ProviderHttpTiming timing) noexcept :
        connection_(std::move(connection)), upstream_(std::move(upstream)), status_code_(status_code),
        content_type_(std::move(content_type)), retry_after_(std::move(retry_after)),
        request_id_(std::move(request_id)), request_send_started_(request_send_started), timing_(timing) {}

    friend class ProviderHttpClient;

    // The exchange must be destroyed before its connection lease is returned.
    ProviderConnectionLease connection_;
    std::unique_ptr<http::ClientHttp1Exchange> upstream_;
    int status_code_ = 0;
    std::string content_type_;
    std::string retry_after_;
    std::string request_id_;
    std::chrono::steady_clock::time_point request_send_started_{};
    std::chrono::steady_clock::time_point first_body_observed_at_{};
    ProviderHttpTiming timing_;
    bool first_body_observed_ = false;
};

class ProviderHttpClient {
public:
    explicit ProviderHttpClient(ProviderConnectionManager &connections) noexcept : connections_(&connections) {}

    [[nodiscard]] async::Task<std::expected<ProviderHttpResponseStream, ProviderHttpError>>
    start(const ResolvedProviderAttempt &attempt, bool stream, mem::IoBufChain request_body, mem::BufPool &request_pool,
          ProviderServiceSelection service_selection, cat::Transaction &cat_transaction,
          std::string_view trace_state = {}) noexcept;

    [[nodiscard]] async::Task<std::expected<BufferedProviderResponse, ProviderHttpError>>
    execute_buffered(const ResolvedProviderAttempt &attempt, bool stream, mem::IoBufChain request_body,
                     mem::BufPool &request_pool, std::size_t max_response_bytes,
                     ProviderServiceSelection service_selection, cat::Transaction &cat_transaction,
                     std::string_view trace_state = {}) noexcept;

private:
    ProviderConnectionManager *connections_ = nullptr;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_PROVIDER_HTTP_CLIENT_H
