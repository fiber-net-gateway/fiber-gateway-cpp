#ifndef FIBER_HTTP_HTTP2_LOCAL_STREAM_GATE_H
#define FIBER_HTTP_HTTP2_LOCAL_STREAM_GATE_H

#include <chrono>
#include <cstddef>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "Http2Connection.h"
#include "Http2Stream.h"

namespace fiber::http {

// FIFO admission in front of one connection's locally initiated streams.
//
// Http2Connection only offers the immediate try_attach_local_stream primitive.
// A client that wants to wait for room parks here instead, so the connection
// itself carries no wait queue. Because this gate owns both the queue and the
// fast path, fairness needs no capacity reservation: try_attach simply yields
// while anyone is queued.
//
// Lives on the connection's EventLoop and is not thread safe. It owns the
// connection's single capacity callback; chain set_capacity_callback to observe
// capacity changes alongside it.
class Http2LocalStreamGate : public common::NonCopyable, public common::NonMovable {
public:
    explicit Http2LocalStreamGate(Http2Connection &connection) noexcept;
    // Cancels every waiter. Their coroutines resume with Canceled on the loop,
    // so tearing the gate down while requests wait is a teardown-only path.
    ~Http2LocalStreamGate();

    // Immediate attempt. Returns Busy while other requests are queued: a
    // newcomer must not barge ahead of them.
    [[nodiscard]] common::IoResult<Http2Stream::Lease> try_attach(Http2Stream &stream) noexcept;

    // Suspends while the connection has no room, resuming in arrival order.
    // Timeout zero is a poll, timeout max waits indefinitely. Draining, closure,
    // a peer GOAWAY, or local stream id exhaustion cancels every waiter.
    [[nodiscard]] fiber::async::Task<common::IoResult<Http2Stream::Lease>>
    attach(Http2Stream &stream, std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;

    void cancel_all(common::IoErr reason) noexcept;

    [[nodiscard]] Http2Connection &connection() const noexcept { return *connection_; }
    [[nodiscard]] std::size_t waiter_count() const noexcept { return waiter_count_; }
    [[nodiscard]] bool has_waiters() const noexcept { return waiter_head_ != nullptr; }

    // Invoked after the gate has reacted to a connection capacity change.
    void set_capacity_callback(Http2Connection::CapacityCallback cb, void *ctx) noexcept;
    void clear_capacity_callback() noexcept;

private:
    class Waiter;

    static void on_connection_capacity(void *ctx, Http2Connection &connection) noexcept;
    void handle_capacity_change() noexcept;
    void wake_waiters() noexcept;
    void link_waiter(Waiter &waiter) noexcept;
    void unlink_waiter(Waiter &waiter) noexcept;

    Http2Connection *connection_ = nullptr;
    Waiter *waiter_head_ = nullptr;
    Waiter *waiter_tail_ = nullptr;
    std::size_t waiter_count_ = 0;
    Http2Connection::CapacityCallback capacity_cb_ = nullptr;
    void *capacity_ctx_ = nullptr;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_LOCAL_STREAM_GATE_H
