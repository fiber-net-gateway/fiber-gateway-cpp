#include "ResponseExecutor.h"
#include "../observability/AccessRequestTelemetry.h"

#include "../../../../src/http/HttpBodySpec.h"
#include "../../../../src/http/HttpExchange.h"
#include "../../../../src/http/HttpExchangeIo.h"
#include "../../../../src/http/HttpHeaders.h"

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

async::Task<common::IoResult<void>> send_response(http::HttpExchange &exchange, const PreparedResponse &response,
                                                  std::span<const EvaluatedHeader> base_headers,
                                                  std::chrono::milliseconds timeout,
                                                  AccessRequestTelemetry *telemetry) noexcept {
    http::HttpHeaders headers(exchange.pool());
    if (!apply_headers(headers, base_headers) || !apply_headers(headers, response.headers) ||
        (telemetry && !telemetry->inject_response_headers(headers))) {
        co_return std::unexpected(common::IoErr::NoMem);
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
        co_return std::unexpected(header_result.error());
    }
    if (empty) {
        co_return common::IoResult<void>{};
    }

    auto body_result = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(response.body.data()),
                                                   response.body.size(), true, timeout);
    if (!body_result) {
        co_return std::unexpected(body_result.error());
    }
    co_return common::IoResult<void>{};
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

async::Task<common::IoResult<void>> ResponseExecutor::execute(http::HttpExchange &exchange, const CompiledRoute &route,
                                                              std::span<const EvaluatedHeader> base_headers,
                                                              TemplateEvaluator evaluator,
                                                              std::size_t max_request_body_size,
                                                              std::string_view trace_id,
                                                              AccessRequestTelemetry *telemetry) const noexcept {
    auto discard_result = co_await discard_request_body(exchange, max_request_body_size, options_.body_timeout);
    if (!discard_result) {
        co_return std::unexpected(discard_result.error());
    }
    if (*discard_result == BodyDiscardStatus::TooLarge) {
        co_return co_await error_responder_.send(exchange, AccessError::request_body_too_large(), base_headers, {},
                                                 trace_id, true, telemetry);
    }
    if (!route.response) {
        co_return co_await error_responder_.send(exchange, AccessError::unknown("route is not a response route"),
                                                 base_headers, {}, trace_id, true, telemetry);
    }

    auto prepared = prepare_response(*route.response, evaluator);
    if (!prepared) {
        co_return co_await error_responder_.send(exchange, prepared.error().error, base_headers,
                                                 prepared.error().inherited_headers, trace_id, true, telemetry);
    }
    co_return co_await send_response(exchange, *prepared, base_headers, options_.write_timeout, telemetry);
}

} // namespace fiber::access_server
