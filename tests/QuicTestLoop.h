#ifndef FIBER_TEST_QUIC_TEST_LOOP_H
#define FIBER_TEST_QUIC_TEST_LOOP_H

#include "event/EventLoop.h"
#include "quic/QuicConnection.h"

namespace fiber::test {

// EventLoop backing QuicConnection in unit tests that exercise codec /
// processor / connection logic synchronously, without ever running a loop.
//
// QuicConnection requires a non-null loop at construction time to host the
// IoBufNodePool it draws receive-extent nodes from (see recv_extent_pool()).
// For synchronous tests this loop is never run(): it only has to exist and
// expose a valid pool. QuicConnection's loop-dependent paths (timers, resume
// posts, loop-consistency asserts) all gate on EventLoop::current_or_null(),
// which is null on a plain test thread, so binding loop_ to this non-running
// loop does not change steady-state behaviour for these tests.
//
// Tests that drive coroutines or timers must instead bind options.loop to
// their own running EventLoopGroup loop (e.g. &group.at(0)).
inline fiber::event::EventLoop &quic_loop() noexcept {
    static fiber::event::EventLoop loop;
    return loop;
}

// Convenience: QuicConnection::Options with loop already bound to quic_loop(),
// for synchronous tests that construct a connection inline.
inline fiber::quic::QuicConnection::Options quic_options() noexcept {
    fiber::quic::QuicConnection::Options options{};
    options.loop = &quic_loop();
    return options;
}

} // namespace fiber::test

#endif // FIBER_TEST_QUIC_TEST_LOOP_H
