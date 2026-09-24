#ifndef FIBER_TEST_QUIC_TEST_LOOP_H
#define FIBER_TEST_QUIC_TEST_LOOP_H

#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>
#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/quic/QuicConnection.h>
#include <fiber/quic/QuicUdpEndpoint.h>

namespace fiber::test {

// EventLoop backing QuicConnection in unit tests that exercise codec /
// processor / connection logic synchronously, without ever running a loop.
//
// QuicConnection takes its loop from the hosting endpoint, and needs a
// non-null one at construction time to host the IoBufNodePool it draws
// receive-extent nodes from (see recv_extent_pool()). For synchronous tests
// this loop is never run(): it only has to exist and expose a valid pool.
// QuicConnection's loop-dependent paths (timers, resume posts,
// loop-consistency asserts) all gate on EventLoop::current_or_null(), which is
// null on a plain test thread, so binding loop_ to this non-running loop does
// not change steady-state behaviour for these tests.
//
// Tests that drive coroutines or timers must instead host their connections
// on a QuicTestEndpoint bound to their own running EventLoopGroup loop
// (e.g. group.at(0)).
inline fiber::event::EventLoop &quic_loop() noexcept {
    static fiber::event::EventLoop loop;
    return loop;
}

// Runs a synchronous test body on quic_loop() ITSELF, so chains resolve that
// loop's node pool and QuicConnection::assert_loop_affinity sees current() ==
// the loop the connections bind to (null current no longer holds once the
// body allocates chains). The static loop survives across tests; run()
// re-arms stop_requested each call.
template<typename F>
void run_in_quic_loop(F &&body) {
    fiber::event::EventLoop &loop = quic_loop();
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        body(loop.io_buf_node_pool());
        loop.stop();
        co_return;
    });
    loop.run();
}

// Coroutine variant of run_in_quic_loop() for tests that must let the loop turn
// between setup and assertions: transitions the connection defers to a later
// tick (e.g. the GracefulClosing -> Closing close-timer handoff, audit #3) only
// fire once run_due_timers() drains again, which a co_await inside the body
// makes happen. The body is a factory returning Task<void> that receives the
// loop.
template<typename F>
void run_in_quic_loop_task(F &&body) {
    fiber::event::EventLoop &loop = quic_loop();
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        co_await body(loop);
        loop.stop();
        co_return;
    });
    loop.run();
}

// Initialized, never started QuicUdpEndpoint hosting standalone QuicConnection
// instances: supplies the loop, frame/crypto pools and receive-storage budget.
// Connections built on it are never attached (indexed), so the endpoint never
// sends, receives or issues connection IDs for them; it is only a host. The
// socket is bound to an ephemeral loopback port and never registered with the
// loop, so the endpoint may be torn down from any thread.
class QuicTestEndpoint {
public:
    explicit QuicTestEndpoint(fiber::event::EventLoop &loop,
                              fiber::quic::QuicUdpEndpoint::EndpointOptions options = {}) noexcept : endpoint_(loop) {
        options.bind_addr = fiber::net::SocketAddress{fiber::net::IpAddress::loopback_v4(), 0};
        auto initialized = endpoint_.init(options);
        FIBER_ASSERT(initialized.has_value());
    }

    [[nodiscard]] fiber::quic::QuicUdpEndpoint &get() noexcept { return endpoint_; }
    [[nodiscard]] fiber::event::EventLoop &loop() noexcept { return endpoint_.loop(); }

private:
    fiber::quic::QuicUdpEndpoint endpoint_;
};

// Process-wide host endpoint on quic_loop(), for synchronous tests that
// construct a connection inline.
inline fiber::quic::QuicUdpEndpoint &quic_endpoint() noexcept {
    static QuicTestEndpoint endpoint(quic_loop());
    return endpoint.get();
}

// Convenience: default QuicConnection::Options for synchronous tests. Pair
// with quic_endpoint() when constructing the connection.
inline fiber::quic::QuicConnection::Options quic_options() noexcept { return {}; }

} // namespace fiber::test

#endif // FIBER_TEST_QUIC_TEST_LOOP_H
