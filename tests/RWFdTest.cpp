#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>

#include <sys/socket.h>
#include <unistd.h>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "event/EventLoop.h"
#include "event/EventLoopGroup.h"

#define private public
#include "net/detail/RWFd.h"
#undef private

namespace {

using fiber::async::DetachedTask;
using namespace std::chrono_literals;

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

DetachedTask wait_readable_task(fiber::net::detail::RWFd *rwfd, std::promise<fiber::common::IoResult<void>> *done) {
    auto result = co_await rwfd->wait_readable();
    done->set_value(result);
    co_return;
}

DetachedTask wait_writable_task(fiber::net::detail::RWFd *rwfd, std::promise<fiber::common::IoResult<void>> *done) {
    auto result = co_await rwfd->wait_writable();
    done->set_value(result);
    co_return;
}

DetachedTask arm_concurrent_read_write_waiters(std::promise<fiber::common::IoResult<void>> *read_done,
                                               std::promise<fiber::common::IoResult<void>> *write_done,
                                               std::promise<void> *armed) {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) != 0) {
        read_done->set_value(std::unexpected(fiber::common::IoErr::Invalid));
        write_done->set_value(std::unexpected(fiber::common::IoErr::Invalid));
        armed->set_value();
        fiber::event::EventLoop::current().stop();
        co_return;
    }

    fiber::net::detail::RWFd rwfd(fiber::event::EventLoop::current(), fds[0]);
    fiber::async::spawn([&]() { return wait_readable_task(&rwfd, read_done); });
    fiber::async::spawn([&]() { return wait_writable_task(&rwfd, write_done); });
    armed->set_value();

    co_await fiber::async::sleep(10ms);

    const char byte = 'x';
    if (::write(fds[1], &byte, 1) != 1) {
        rwfd.close();
        (void) ::close(fds[1]);
        fiber::event::EventLoop::current().stop();
        co_return;
    }

    for (int i = 0; i < 200 && (rwfd.read_waiter_ != nullptr || rwfd.write_waiter_ != nullptr); ++i) {
        co_await fiber::async::sleep(1ms);
    }

    rwfd.close();
    (void) ::close(fds[1]);
    fiber::event::EventLoop::current().stop();
    co_return;
}

TEST(RWFdTest, AllowsConcurrentReadAndWriteWaiters) {
    fiber::event::EventLoopGroup group(1);
    std::promise<fiber::common::IoResult<void>> read_promise;
    std::promise<fiber::common::IoResult<void>> write_promise;
    std::promise<void> armed_promise;
    auto read_future = read_promise.get_future();
    auto write_future = write_promise.get_future();
    auto armed_future = armed_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() {
        return arm_concurrent_read_write_waiters(&read_promise, &write_promise, &armed_promise);
    });

    ASSERT_EQ(armed_future.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(read_future.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(write_future.wait_for(2s), std::future_status::ready);

    auto read_result = read_future.get();
    auto write_result = write_future.get();
    EXPECT_TRUE(read_result);
    EXPECT_TRUE(write_result);

    group.join();
}

} // namespace
