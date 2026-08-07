#include "ResponseExecutor.h"
#include "../observability/AccessRequestTelemetry.h"

#include <fiber/http/HttpBodySpec.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpExchangeIo.h>
#include <fiber/http/HttpHeaders.h>

namespace fiber::access_server {
namespace {

enum class BodyDiscardStatus : std::uint8_t {
    Complete,
    TooLarge,
};

bool apply_headers(http::HttpHeaders &headers, std::span<const EvaluatedHeader> values) noexcept {
    for (const EvaluatedHeader &header: values) {
        if (!headers.set(header.name, header.value)) {
            return false;
        }
    }
    return true;
}

async::Task<Result<void>> send_response(http::HttpExchange &exchange, const PreparedResponse &response,
                                        std::chrono::milliseconds timeout, AccessRequestTelemetry &telemetry) noexcept {
    http::HttpHeaders &headers = telemetry.response_headers();
    if (!apply_headers(headers, response.headers) || !telemetry.finalize_response_headers()) {
        co_return std::unexpected(Err::from_error(common::IoErr::NoMem));
    }

    const bool empty = response.body.empty();
    auto header_result = co_await exchange.send_header(
            {
                    .kind = http::OutgoingHeaderKind::Final,
                    .status_code = response.status,
                    .headers = &headers,
                    .body = http::HttpBodySpec::ContentLength(response.body.size()),
                    .connection_mode = http::ResponseConnectionMode::Auto,
                    .end_stream = empty,
            },
            timeout);
    if (!header_result) {
        co_return std::unexpected(Err::from_error(header_result.error()));
    }
    if (empty) {
        co_return Result<void>{};
    }

    auto body_result = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(response.body.data()),
                                                   response.body.size(), true, timeout);
    if (!body_result) {
        co_return std::unexpected(Err::from_error(body_result.error()));
    }
    co_return Result<void>{};
}

async::Task<common::IoResult<BodyDiscardStatus>> discard_request_body(http::HttpExchange &exchange,
                                                                      std::size_t max_request_body_size,
                                                                      std::chrono::milliseconds timeout) noexcept {
    const http::HttpBodySpec body_spec = exchange.request_body_spec();
    if (max_request_body_size != 0 && body_spec.is_content_length() &&
        body_spec.content_length() > max_request_body_size) {
        co_return BodyDiscardStatus::TooLarge;
    }

    std::size_t consumed = 0;
    for (;;) {
        auto chunk = co_await exchange.read_body(4096, timeout);
        if (!chunk) {
            co_return std::unexpected(chunk.error());
        }
        const std::size_t bytes = chunk->readable_bytes();
        if (max_request_body_size != 0 &&
            (consumed > max_request_body_size || bytes > max_request_body_size - consumed)) {
            co_return BodyDiscardStatus::TooLarge;
        }
        consumed += bytes;
        if (chunk->complete()) {
            co_return BodyDiscardStatus::Complete;
        }
    }
}

} // namespace

async::Task<Result<void>> ResponseExecutor::execute(http::HttpExchange &exchange, const CompiledRoute &route,
                                                    AccessRequestTelemetry &telemetry, TemplateEvaluator evaluator,
                                                    std::size_t max_request_body_size) const noexcept {
    auto discard_result = co_await discard_request_body(exchange, max_request_body_size, options_.body_timeout);
    if (!discard_result) {
        co_return std::unexpected(Err::from_error(discard_result.error()));
    }
    if (*discard_result == BodyDiscardStatus::TooLarge) {
        co_return std::unexpected(Err::from_exception(Exception::request_body_too_large()));
    }
    if (!route.response) {
        co_return std::unexpected(Err::from_exception(Exception::unknown("route is not a response route")));
    }

    auto prepared = prepare_response(*route.response, evaluator);
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    co_return co_await send_response(exchange, *prepared, options_.write_timeout, telemetry);
}

} // namespace fiber::access_server
