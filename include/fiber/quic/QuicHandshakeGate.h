#ifndef FIBER_QUIC_QUIC_HANDSHAKE_GATE_H
#define FIBER_QUIC_QUIC_HANDSHAKE_GATE_H

#include <chrono>
#include <cstddef>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"

namespace fiber::quic {

class QuicConnection;

// The coroutines parked on one connection's handshake progress.
//
// Unlike QuicLocalStreamGate this is owned by the connection itself rather
// than by the application, and deliberately so. Waiting on a handshake carries
// no policy for an owner to make: every waiter gets the same answer at the
// same instant, so there is no admission order, no budget, and nothing an
// application knows that the transport does not. And the party that waits --
// whoever called connect() -- may do so on a connection whose Ops are later
// replaced wholesale by set_app_ops; a gate wired through Ops would be handed
// off mid-handshake. What moves out here is the machinery, not the ownership.
//
// Lives on the connection's EventLoop and is not thread safe.
class QuicHandshakeGate : public common::NonCopyable, public common::NonMovable {
public:
    explicit QuicHandshakeGate(QuicConnection &connection) noexcept : connection_(&connection) {}
    // The connection asserts the gate is empty before it gets here: a waiter
    // outliving the connection it waits on is a lifetime bug, not a teardown
    // path to paper over.
    ~QuicHandshakeGate();

    // Suspends until the handshake reaches Established, or -- with confirmed --
    // until it is also confirmed. Timeout zero is a poll, timeout max waits
    // indefinitely. Any close resolves every waiter with why it closed.
    [[nodiscard]] async::Task<common::IoResult<void>> wait(bool confirmed, std::chrono::milliseconds timeout) noexcept;

    // Connection state moved. WouldBlock, the default, means "re-evaluate":
    // each waiter is completed only once wait_result() has a terminal answer
    // for it, which is why a waiter for confirmation sits through the
    // transition that satisfies one waiting only for Established. Any other
    // value completes every waiter with it.
    void notify(common::IoErr result = common::IoErr::WouldBlock) noexcept;

    // None once the handshake has reached what such a waiter asked for,
    // WouldBlock while it is still in flight, and otherwise why it will not:
    // the client connect failure if there is one, else what closed the
    // connection.
    [[nodiscard]] common::IoErr wait_result(bool confirmed) const noexcept;

    [[nodiscard]] bool has_waiters() const noexcept { return head_ != nullptr; }
    [[nodiscard]] std::size_t waiter_count() const noexcept { return count_; }

private:
    class Waiter;

    void link(Waiter &waiter) noexcept;
    void unlink(Waiter &waiter) noexcept;

    QuicConnection *connection_ = nullptr;
    Waiter *head_ = nullptr;
    Waiter *tail_ = nullptr;
    std::size_t count_ = 0;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_HANDSHAKE_GATE_H
