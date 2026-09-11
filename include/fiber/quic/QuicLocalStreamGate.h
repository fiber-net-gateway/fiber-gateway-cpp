#ifndef FIBER_QUIC_QUIC_LOCAL_STREAM_GATE_H
#define FIBER_QUIC_QUIC_LOCAL_STREAM_GATE_H

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "../async/Task.h"
#include "../async/WaitAwaiter.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "QuicConnection.h"
#include "QuicStream.h"

namespace fiber::quic {

// FIFO admission in front of one connection's locally initiated streams.
//
// QuicConnection only offers the immediate try_attach_local_stream primitive.
// A caller that wants to wait for peer stream credit parks here instead, so the
// connection itself carries no wait queue. Because this gate owns both the
// queue and the fast path, fairness needs no credit reservation: try_attach
// simply yields while anyone is queued.
//
// Bidirectional and unidirectional streams draw on separate peer budgets, so
// the gate keeps a queue per type and a change to one never disturbs the other.
//
// Lives on the connection's EventLoop and is not thread safe. The owner
// forwards QuicConnection::Ops notifications to on_state_change /
// on_capacity_change, and may cancel a queue itself when it knows something the
// transport does not -- an HTTP/3 GOAWAY refuses new requests while QUIC is
// still perfectly willing to open streams.
class QuicLocalStreamGate : public common::NonCopyable, public common::NonMovable {
public:
    explicit QuicLocalStreamGate(QuicConnection &connection) noexcept;
    // Cancels every waiter. Their coroutines resume with Canceled on the loop,
    // so tearing the gate down while requests wait is a teardown-only path.
    ~QuicLocalStreamGate();

    // Immediate attempt. Returns Busy while other requests of this type are
    // queued: a newcomer must not barge ahead of them. The lease is consumed
    // only on success, so a caller may retry with the same stream.
    [[nodiscard]] common::IoResult<QuicStream *>
    try_attach(QuicStream::Lease &&stream, QuicStreamType type,
               QuicStreamEarlyDataMode early_data_mode = QuicStreamEarlyDataMode::OneRttOnly) noexcept;

    // Suspends while the connection has no credit for this type, resuming in
    // arrival order. Timeout zero is a poll, timeout max waits indefinitely.
    // Closure, or a cancel_all from the owner, cancels every waiter. A
    // ReplaySafe caller never waits: it wants 0-RTT or an answer, so it gets
    // Busy rather than a wait for the handshake to finish.
    [[nodiscard]] async::Task<common::IoResult<QuicStream *>>
    attach(QuicStream::Lease stream, QuicStreamType type,
           std::chrono::milliseconds timeout = std::chrono::milliseconds::max(),
           QuicStreamEarlyDataMode early_data_mode = QuicStreamEarlyDataMode::OneRttOnly) noexcept;

    void cancel_all(common::IoErr reason) noexcept;
    void cancel_all(QuicStreamType type, common::IoErr reason) noexcept;
    void on_capacity_change() noexcept;
    void on_state_change() noexcept;

    [[nodiscard]] QuicConnection &connection() const noexcept { return *connection_; }
    [[nodiscard]] std::size_t waiter_count() const noexcept { return bidi_.count + uni_.count; }
    [[nodiscard]] std::size_t waiter_count(QuicStreamType type) const noexcept;
    [[nodiscard]] bool has_waiters() const noexcept { return waiter_count() != 0; }

private:
    class Waiter;

    struct Queue {
        Waiter *head = nullptr;
        Waiter *tail = nullptr;
        std::size_t count = 0;
    };

    [[nodiscard]] Queue &queue_for(QuicStreamType type) noexcept {
        return type == QuicStreamType::Bidirectional ? bidi_ : uni_;
    }
    [[nodiscard]] const Queue &queue_for(QuicStreamType type) const noexcept {
        return type == QuicStreamType::Bidirectional ? bidi_ : uni_;
    }

    void handle_change(QuicStreamType type) noexcept;
    void wake_waiters(QuicStreamType type) noexcept;
    void link_waiter(Waiter &waiter) noexcept;
    void unlink_waiter(Waiter &waiter) noexcept;
    void detach_waiter(Waiter &waiter) noexcept;

    QuicConnection *connection_ = nullptr;
    Queue bidi_{};
    Queue uni_{};
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_LOCAL_STREAM_GATE_H
