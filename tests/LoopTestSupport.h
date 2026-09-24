#ifndef FIBER_TEST_LOOP_TEST_SUPPORT_H
#define FIBER_TEST_LOOP_TEST_SUPPORT_H

#include <utility>

#include <fiber/async/Spawn.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/event/EventLoop.h>

namespace fiber::test {

// Runs a synchronous test body inside a running EventLoop, so IoBufChain node
// allocation/release — which resolves the CURRENT thread's loop per operation
// — has a loop to resolve (IoBufChainTest et al; see IoBufChain.h). ASSERT_*
// inside `body` only returns from the coroutine lambda; loop.stop() still runs.
//
// `body` receives the loop's IoBufNodePool for units-under-test that keep an
// explicit pool member (QuicStreamSendQueue and friends). Chains created in
// the body must be destroyed before it returns: after run_in_loop() the thread
// has no current loop again, and destroying a non-empty chain would assert.
template<typename F>
void run_in_loop(F &&body) {
    fiber::event::EventLoop loop;
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        body(loop.io_buf_node_pool());
        loop.stop();
        co_return;
    });
    loop.run();
}

} // namespace fiber::test

#endif // FIBER_TEST_LOOP_TEST_SUPPORT_H
