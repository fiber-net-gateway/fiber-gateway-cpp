#ifndef FIBER_LITE_NGINX_RUNTIME_REQUEST_BODY_LIMITER_H
#define FIBER_LITE_NGINX_RUNTIME_REQUEST_BODY_LIMITER_H

#include <chrono>
#include <cstddef>

#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/common/mem/IoBufChain.h>

namespace fiber::http {
class HttpExchange;
}

namespace fiber::lite_nginx::runtime {

class RequestBodyLimiter {
public:
    RequestBodyLimiter(fiber::http::HttpExchange &exchange, std::size_t max_bytes) noexcept;

    [[nodiscard]] bool exceeded() const noexcept { return exceeded_; }

    fiber::async::Task<fiber::common::IoResult<fiber::mem::IoBufChain>>
    read_body(std::size_t max_bytes, std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    fiber::async::Task<fiber::common::IoResult<void>>
    discard_body(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    fiber::common::IoResult<void> abort(fiber::common::IoErr reason = fiber::common::IoErr::Canceled) noexcept;

private:
    fiber::async::Task<fiber::common::IoResult<void>> test_expect_continue(std::chrono::milliseconds timeout) noexcept;

    fiber::http::HttpExchange &exchange_;
    std::size_t max_bytes_ = 0;
    std::size_t bytes_read_ = 0;
    bool expect_tested_ = false;
    bool exceeded_ = false;
};

} // namespace fiber::lite_nginx::runtime

#endif // FIBER_LITE_NGINX_RUNTIME_REQUEST_BODY_LIMITER_H
