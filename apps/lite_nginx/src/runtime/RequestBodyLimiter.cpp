#include "RequestBodyLimiter.h"

#include <algorithm>
#include <limits>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpHeaderHash.h>

namespace fiber::lite_nginx::runtime {

RequestBodyLimiter::RequestBodyLimiter(fiber::http::HttpExchange &exchange, std::size_t max_bytes) noexcept :
    exchange_(exchange), max_bytes_(max_bytes) {
    const fiber::http::HttpBodySpec body = exchange_.request_body_spec();
    exceeded_ = max_bytes_ != 0 && body.is_content_length() && body.content_length() > max_bytes_;
}

fiber::async::Task<fiber::common::IoResult<void>>
RequestBodyLimiter::test_expect_continue(std::chrono::milliseconds timeout) noexcept {
    if (expect_tested_ || exchange_.response_stats().header_sent) {
        co_return fiber::common::IoResult<void>{};
    }
    expect_tested_ = true;

    const auto *expect = exchange_.expect_header();
    if (exchange_.version() != fiber::http::HttpVersion::HTTP_1_1 || expect == nullptr ||
        !fiber::http::http_header_name_equals_ci(expect->value_view(), "100-continue")) {
        co_return fiber::common::IoResult<void>{};
    }
    co_return co_await exchange_.send_continue_header(timeout);
}

fiber::async::Task<fiber::common::IoResult<fiber::mem::IoBufChain>>
RequestBodyLimiter::read_body(std::size_t max_bytes, std::chrono::milliseconds timeout) noexcept {
    if (exceeded_) {
        co_return std::unexpected(fiber::common::IoErr::MessageTooLarge);
    }
    if (max_bytes != 0) {
        auto expect_result = co_await test_expect_continue(timeout);
        if (!expect_result) {
            co_return std::unexpected(expect_result.error());
        }
    }

    std::size_t read_limit = max_bytes;
    std::size_t remaining = 0;
    if (max_bytes_ != 0) {
        FIBER_ASSERT(bytes_read_ <= max_bytes_);
        remaining = max_bytes_ - bytes_read_;
        if (max_bytes != 0) {
            const std::size_t probe = remaining == std::numeric_limits<std::size_t>::max() ? remaining : remaining + 1;
            read_limit = std::min(max_bytes, probe);
        }
    }

    auto result = co_await exchange_.read_body(read_limit, timeout);
    if (!result || max_bytes_ == 0) {
        co_return std::move(result);
    }

    const std::size_t bytes = result->readable_bytes();
    if (bytes > remaining) {
        exceeded_ = true;
        co_return std::unexpected(fiber::common::IoErr::MessageTooLarge);
    }
    bytes_read_ += bytes;
    co_return std::move(result);
}

fiber::async::Task<fiber::common::IoResult<void>>
RequestBodyLimiter::discard_body(std::chrono::milliseconds timeout) noexcept {
    for (;;) {
        auto result = co_await read_body(4096, timeout);
        if (!result) {
            co_return std::unexpected(result.error());
        }
        if (result->complete()) {
            co_return fiber::common::IoResult<void>{};
        }
    }
}

fiber::common::IoResult<void> RequestBodyLimiter::abort(fiber::common::IoErr reason) noexcept {
    return exchange_.abort(reason);
}

} // namespace fiber::lite_nginx::runtime
