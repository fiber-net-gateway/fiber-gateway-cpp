#ifndef FIBER_HTTP_DETAIL_HTTP2_HEADER_BLOCK_QUEUE_H
#define FIBER_HTTP_DETAIL_HTTP2_HEADER_BLOCK_QUEUE_H

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "../../async/Task.h"
#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../common/mem/BufPool.h"
#include "../ClientHttp2Types.h"

namespace fiber::http::detail {

class Http2HeaderBlockQueue : public common::NonCopyable, public common::NonMovable {
public:
    struct HeaderNode {
        explicit HeaderNode(mem::BufPool &pool) noexcept : head(pool) {}

        Http2ResponseHead head;
        HeaderNode *next = nullptr;
    };

    Http2HeaderBlockQueue(mem::BufPool &pool) noexcept;
    ~Http2HeaderBlockQueue() = default;

    [[nodiscard]] HeaderNode *allocate_node() noexcept;
    [[nodiscard]] common::IoErr push_header_block(HeaderNode *node) noexcept;
    void close_input() noexcept;
    void abort(common::IoErr reason) noexcept;

    fiber::async::Task<common::IoResult<const Http2ResponseHead *>>
    read_header(std::chrono::milliseconds timeout) noexcept;

private:
    struct PollResult;
    class HeaderReadAwaiter;

    [[nodiscard]] PollResult poll() const noexcept;
    [[nodiscard]] bool arm_waiter(HeaderReadAwaiter *awaiter) noexcept;
    void cancel_waiter(HeaderReadAwaiter *awaiter) noexcept;
    void notify_waiter() noexcept;

    mem::BufPool *pool_ = nullptr;
    HeaderNode *head_ = nullptr;
    HeaderNode *tail_ = nullptr;
    HeaderReadAwaiter *waiter_ = nullptr;
    common::IoErr abort_reason_ = common::IoErr::None;
    bool input_closed_ = false;
};

} // namespace fiber::http::detail

#endif // FIBER_HTTP_DETAIL_HTTP2_HEADER_BLOCK_QUEUE_H
