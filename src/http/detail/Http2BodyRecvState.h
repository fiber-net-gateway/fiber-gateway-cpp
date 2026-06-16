#ifndef FIBER_HTTP_DETAIL_HTTP2_BODY_RECV_STATE_H
#define FIBER_HTTP_DETAIL_HTTP2_BODY_RECV_STATE_H

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "../../async/Task.h"
#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../common/mem/IoBufChain.h"
#include "../HttpExchange.h"

namespace fiber::http {

class Http2Stream;

namespace detail {

class Http2BodyRecvState : public common::NonCopyable, public common::NonMovable {
public:
    Http2BodyRecvState(mem::IoBufNodePool &node_pool,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds::zero()) noexcept;
    ~Http2BodyRecvState() = default;

    [[nodiscard]] common::IoErr push_body(mem::IoBuf &&buf, bool end_stream) noexcept;
    void close_input() noexcept;
    void abort(common::IoErr reason) noexcept;
    [[nodiscard]] std::size_t queued_bytes() const noexcept { return queue_.readable_bytes(); }

    fiber::async::Task<common::IoResult<mem::IoBufChain>> read_body(Http2Stream &stream, std::size_t max_bytes) noexcept;

private:
    struct PollResult;
    class BodyReadAwaiter;

    [[nodiscard]] PollResult poll() const noexcept;
    [[nodiscard]] bool arm_waiter(BodyReadAwaiter *awaiter) noexcept;
    void cancel_waiter(BodyReadAwaiter *awaiter) noexcept;
    void notify_waiter() noexcept;

    mem::IoBufChain queue_;
    std::chrono::milliseconds timeout_{};
    BodyReadAwaiter *waiter_ = nullptr;
    common::IoErr abort_reason_ = common::IoErr::None;
    bool input_closed_ = false;
};

} // namespace detail
} // namespace fiber::http

#endif // FIBER_HTTP_DETAIL_HTTP2_BODY_RECV_STATE_H
