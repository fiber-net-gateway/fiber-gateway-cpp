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

namespace fiber::ai_server {

enum class ProviderHttpErrorCode : std::uint8_t {
    Connect,
    SendHeader,
    SendBody,
    ReadHeader,
    ReadBody,
    ResponseTooLarge,
    InvalidResponse,
};

struct ProviderHttpError {
    ProviderHttpErrorCode code = ProviderHttpErrorCode::Connect;
    common::IoErr io_error = common::IoErr::None;
    const char *message = nullptr;
};

struct BufferedProviderResponse {
    int status_code = 0;
    std::string content_type;
    std::string retry_after;
    std::string request_id;
    mem::IoBuf body;
    ProviderLoadBalanceLease load_balance;
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

    [[nodiscard]] async::Task<common::IoResult<mem::IoBufChain>>
    read_body(std::size_t max_bytes, std::chrono::milliseconds timeout = std::chrono::seconds(300)) noexcept;
    common::IoResult<void> abort(common::IoErr reason = common::IoErr::Canceled) noexcept;
    void report_instance(InstanceReportOutcome outcome) noexcept { connection_.load_balance.report(outcome); }
    [[nodiscard]] ProviderLoadBalanceLease take_load_balance() noexcept { return std::move(connection_.load_balance); }

private:
    ProviderHttpResponseStream(ProviderConnectionLease connection, std::unique_ptr<http::ClientHttp1Exchange> upstream,
                               int status_code, std::string content_type, std::string retry_after,
                               std::string request_id) noexcept :
        connection_(std::move(connection)), upstream_(std::move(upstream)), status_code_(status_code),
        content_type_(std::move(content_type)), retry_after_(std::move(retry_after)),
        request_id_(std::move(request_id)) {}

    friend class ProviderHttpClient;

    // The exchange must be destroyed before its connection lease is returned.
    ProviderConnectionLease connection_;
    std::unique_ptr<http::ClientHttp1Exchange> upstream_;
    int status_code_ = 0;
    std::string content_type_;
    std::string retry_after_;
    std::string request_id_;
};

class ProviderHttpClient {
public:
    explicit ProviderHttpClient(ProviderConnectionManager &connections) noexcept : connections_(&connections) {}

    [[nodiscard]] async::Task<std::expected<ProviderHttpResponseStream, ProviderHttpError>>
    start(const ResolvedProviderAttempt &attempt, bool stream, mem::IoBufChain request_body,
          mem::BufPool &request_pool) noexcept;

    [[nodiscard]] async::Task<std::expected<BufferedProviderResponse, ProviderHttpError>>
    execute_buffered(const ResolvedProviderAttempt &attempt, bool stream, mem::IoBufChain request_body,
                     mem::BufPool &request_pool, std::size_t max_response_bytes = 32 * 1024 * 1024) noexcept;

private:
    ProviderConnectionManager *connections_ = nullptr;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_PROVIDER_HTTP_CLIENT_H
