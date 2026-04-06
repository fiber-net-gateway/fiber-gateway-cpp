#include <gtest/gtest.h>

#include <atomic>

#include <sys/socket.h>
#include <unistd.h>

#include "async/Spawn.h"
#include "event/EventLoop.h"

#define private public
#include "net/detail/RWFd.h"
#undef private

namespace {

TEST(RWFdTest, IgnoresLateEventWithoutWaiter) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoop loop;
    std::atomic<bool> completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        fiber::net::detail::RWFd rwfd(loop, fds[0]);
        rwfd.handle_events(fiber::event::IoEvent::Read | fiber::event::IoEvent::Write);
        rwfd.close();
        ::close(fds[1]);
        completed.store(true, std::memory_order_release);
        loop.stop();
        co_return;
    });

    loop.run();
    EXPECT_TRUE(completed.load(std::memory_order_acquire));
}

} // namespace
