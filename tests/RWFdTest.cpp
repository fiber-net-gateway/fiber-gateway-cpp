#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <utility>

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

void noop_ready_callback(void *, fiber::common::IoErr) noexcept {}

struct CallbackResult {
    int calls = 0;
    fiber::common::IoErr err = fiber::common::IoErr::Invalid;
};

void record_callback(void *raw_ctx, fiber::common::IoErr err) noexcept {
    auto *ctx = static_cast<CallbackResult *>(raw_ctx);
    ++ctx->calls;
    ctx->err = err;
}

struct PersistentReadCallbackCtx {
    fiber::net::detail::RWFd *rwfd = nullptr;
    fiber::event::EventLoop *loop = nullptr;
    int peer_fd = -1;
    int calls = 0;
    bool io_ok = true;
    fiber::common::IoErr clear_err = fiber::common::IoErr::Invalid;
};

void on_persistent_read(void *raw_ctx, fiber::common::IoErr err) noexcept;

void finish_persistent_read_callback(PersistentReadCallbackCtx *ctx) noexcept {
    ctx->clear_err = ctx->rwfd->clear_read_callback(&on_persistent_read, ctx);
    ctx->rwfd->close();
    (void) ::close(ctx->peer_fd);
    ctx->peer_fd = -1;
    ctx->loop->stop();
}

void on_persistent_read(void *raw_ctx, fiber::common::IoErr err) noexcept {
    auto *ctx = static_cast<PersistentReadCallbackCtx *>(raw_ctx);
    if (err != fiber::common::IoErr::None) {
        ctx->io_ok = false;
        return;
    }
    char byte = 0;
    if (::read(ctx->rwfd->fd(), &byte, 1) != 1) {
        ctx->io_ok = false;
        finish_persistent_read_callback(ctx);
        return;
    }

    ++ctx->calls;
    if (ctx->calls == 1) {
        const char next = 'y';
        if (::write(ctx->peer_fd, &next, 1) != 1) {
            ctx->io_ok = false;
            finish_persistent_read_callback(ctx);
        }
        return;
    }
    finish_persistent_read_callback(ctx);
}

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

TEST(RWFdTest, RearmsPersistentReadCallbackAfterOneShotEvent) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoop loop;
    fiber::net::detail::RWFd rwfd(loop, fds[0]);
    PersistentReadCallbackCtx ctx{&rwfd, &loop, fds[1]};
    fiber::common::IoErr set_err = fiber::common::IoErr::Invalid;

    fiber::async::spawn(loop, [&]() -> DetachedTask {
        set_err = rwfd.set_read_callback(&on_persistent_read, &ctx);
        if (set_err != fiber::common::IoErr::None) {
            rwfd.close();
            (void) ::close(ctx.peer_fd);
            ctx.peer_fd = -1;
            loop.stop();
            co_return;
        }
        const char first = 'x';
        ctx.io_ok = ::write(ctx.peer_fd, &first, 1) == 1;
        if (!ctx.io_ok) {
            finish_persistent_read_callback(&ctx);
        }
        co_return;
    });

    loop.run();
    EXPECT_EQ(set_err, fiber::common::IoErr::None);
    EXPECT_TRUE(ctx.io_ok);
    EXPECT_EQ(ctx.calls, 2);
    EXPECT_EQ(ctx.clear_err, fiber::common::IoErr::None);
}

TEST(RWFdTest, CloseCancelsRegisteredCallbacks) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoop loop;
    fiber::net::detail::RWFd rwfd(loop, fds[0]);
    CallbackResult read_result;
    CallbackResult write_result;
    fiber::common::IoErr set_read_err = fiber::common::IoErr::Invalid;
    fiber::common::IoErr set_write_err = fiber::common::IoErr::Invalid;

    fiber::async::spawn(loop, [&]() -> DetachedTask {
        set_read_err = rwfd.set_read_callback(&record_callback, &read_result);
        set_write_err = rwfd.set_write_callback(&record_callback, &write_result);
        rwfd.close();
        (void) ::close(fds[1]);
        loop.stop();
        co_return;
    });

    loop.run();
    EXPECT_EQ(set_read_err, fiber::common::IoErr::None);
    EXPECT_EQ(set_write_err, fiber::common::IoErr::None);
    EXPECT_EQ(read_result.calls, 1);
    EXPECT_EQ(read_result.err, fiber::common::IoErr::Canceled);
    EXPECT_EQ(write_result.calls, 1);
    EXPECT_EQ(write_result.err, fiber::common::IoErr::Canceled);
}

TEST(RWFdTest, ClearCallbackRequiresMatchingPair) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoop loop;
    fiber::net::detail::RWFd rwfd(loop, fds[0]);
    CallbackResult registered_result;
    CallbackResult other_result;
    fiber::common::IoErr set_err = fiber::common::IoErr::Invalid;
    fiber::common::IoErr clear_other_err = fiber::common::IoErr::Invalid;

    fiber::async::spawn(loop, [&]() -> DetachedTask {
        set_err = rwfd.set_read_callback(&record_callback, &registered_result);
        clear_other_err = rwfd.clear_read_callback(&record_callback, &other_result);
        rwfd.close();
        (void) ::close(fds[1]);
        loop.stop();
        co_return;
    });

    loop.run();
    EXPECT_EQ(set_err, fiber::common::IoErr::None);
    EXPECT_EQ(clear_other_err, fiber::common::IoErr::None);
    EXPECT_EQ(registered_result.calls, 1);
    EXPECT_EQ(registered_result.err, fiber::common::IoErr::Canceled);
    EXPECT_EQ(other_result.calls, 0);
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

struct CrossThreadWaitResult {
    fiber::common::IoResult<void> result;
    bool resumed_on_origin = false;
};

DetachedTask cross_thread_wait_readable(fiber::net::detail::RWFd *rwfd, fiber::event::EventLoop *origin,
                                        std::promise<CrossThreadWaitResult> *done) {
    auto result = co_await rwfd->wait_readable();
    done->set_value(CrossThreadWaitResult{std::move(result), fiber::event::EventLoop::current_or_null() == origin});
    origin->stop();
    co_return;
}

DetachedTask cross_thread_wait_readable(fiber::net::detail::RWFd *rwfd, fiber::event::EventLoop *origin,
                                        std::chrono::milliseconds timeout, std::promise<CrossThreadWaitResult> *done) {
    auto result = co_await rwfd->wait_readable(timeout);
    done->set_value(CrossThreadWaitResult{std::move(result), fiber::event::EventLoop::current_or_null() == origin});
    origin->stop();
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

    for (int i = 0; i < 200 && (rwfd.read_callback_ != nullptr || rwfd.write_callback_ != nullptr); ++i) {
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

TEST(RWFdTest, ResumesCrossThreadWaiterOnOriginLoop) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoopGroup group(2);
    group.start();
    fiber::net::detail::RWFd rwfd(group.at(0), fds[0]);
    std::promise<CrossThreadWaitResult> wait_promise;
    auto wait_future = wait_promise.get_future();
    fiber::async::spawn(group.at(1), [&]() { return cross_thread_wait_readable(&rwfd, &group.at(1), &wait_promise); });

    const char byte = 'x';
    const ssize_t write_result = ::write(fds[1], &byte, 1);
    auto wait_status = write_result == 1 ? wait_future.wait_for(2s) : std::future_status::timeout;

    std::promise<void> cleanup_promise;
    auto cleanup_future = cleanup_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        rwfd.close();
        (void) ::close(fds[1]);
        cleanup_promise.set_value();
        group.at(0).stop();
        co_return;
    });
    auto cleanup_status = cleanup_future.wait_for(2s);
    group.stop();
    group.join();

    ASSERT_EQ(write_result, 1);
    ASSERT_EQ(cleanup_status, std::future_status::ready);
    ASSERT_EQ(wait_status, std::future_status::ready);
    auto wait_result = wait_future.get();
    EXPECT_TRUE(wait_result.result);
    EXPECT_TRUE(wait_result.resumed_on_origin);
}

TEST(RWFdTest, CrossThreadTimeoutResumesOnOriginLoop) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoopGroup group(2);
    group.start();
    fiber::net::detail::RWFd rwfd(group.at(0), fds[0]);
    std::promise<CrossThreadWaitResult> wait_promise;
    auto wait_future = wait_promise.get_future();
    fiber::async::spawn(group.at(1),
                        [&]() { return cross_thread_wait_readable(&rwfd, &group.at(1), 20ms, &wait_promise); });

    auto wait_status = wait_future.wait_for(2s);

    std::promise<void> cleanup_promise;
    auto cleanup_future = cleanup_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        rwfd.close();
        (void) ::close(fds[1]);
        cleanup_promise.set_value();
        group.at(0).stop();
        co_return;
    });
    auto cleanup_status = cleanup_future.wait_for(2s);
    group.stop();
    group.join();

    ASSERT_EQ(cleanup_status, std::future_status::ready);
    ASSERT_EQ(wait_status, std::future_status::ready);
    auto wait_result = wait_future.get();
    ASSERT_FALSE(wait_result.result);
    EXPECT_EQ(wait_result.result.error(), fiber::common::IoErr::TimedOut);
    EXPECT_TRUE(wait_result.resumed_on_origin);
}

TEST(RWFdTest, ReadinessCompletesBeforeTimeout) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoop loop;
    fiber::net::detail::RWFd rwfd(loop, fds[0]);
    fiber::common::IoResult<void> result = std::unexpected(fiber::common::IoErr::Invalid);

    fiber::async::spawn(loop, [&]() -> DetachedTask {
        const char byte = 'x';
        if (::write(fds[1], &byte, 1) != 1) {
            rwfd.close();
            (void) ::close(fds[1]);
            loop.stop();
            co_return;
        }
        result = co_await rwfd.wait_readable(200ms);
        rwfd.close();
        (void) ::close(fds[1]);
        loop.stop();
        co_return;
    });

    loop.run();
    EXPECT_TRUE(result);
}

TEST(RWFdTest, TimeoutClearsCallbackSlot) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoop loop;
    fiber::net::detail::RWFd rwfd(loop, fds[0]);
    fiber::common::IoResult<void> result = std::unexpected(fiber::common::IoErr::Invalid);
    bool callback_cleared = false;

    fiber::async::spawn(loop, [&]() -> DetachedTask {
        result = co_await rwfd.wait_readable(10ms);
        callback_cleared = rwfd.read_callback_ == nullptr;
        rwfd.close();
        (void) ::close(fds[1]);
        loop.stop();
        co_return;
    });

    loop.run();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), fiber::common::IoErr::TimedOut);
    EXPECT_TRUE(callback_cleared);
}

TEST(RWFdTest, NonPositiveTimeoutDoesNotRegisterCallback) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoop loop;
    fiber::net::detail::RWFd rwfd(loop, fds[0]);
    fiber::common::IoResult<void> result = std::unexpected(fiber::common::IoErr::Invalid);
    bool callback_cleared = false;

    fiber::async::spawn(loop, [&]() -> DetachedTask {
        result = co_await rwfd.wait_readable(0ms);
        callback_cleared = rwfd.read_callback_ == nullptr;
        rwfd.close();
        (void) ::close(fds[1]);
        loop.stop();
        co_return;
    });

    loop.run();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), fiber::common::IoErr::TimedOut);
    EXPECT_TRUE(callback_cleared);
}

TEST(RWFdTest, CloseCancelsTimedWaiter) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoop loop;
    fiber::net::detail::RWFd rwfd(loop, fds[0]);
    fiber::common::IoResult<void> result = std::unexpected(fiber::common::IoErr::Invalid);
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> DetachedTask {
        fiber::async::spawn([&]() -> DetachedTask {
            result = co_await rwfd.wait_readable(1s);
            completed = true;
            co_return;
        });
        while (rwfd.read_callback_ == nullptr) {
            co_await fiber::async::sleep(1ms);
        }
        rwfd.close();
        (void) ::close(fds[1]);
        loop.stop();
        co_return;
    });

    loop.run();
    EXPECT_TRUE(completed);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), fiber::common::IoErr::Canceled);
}

TEST(RWFdTest, RejectsSameDirectionCallbackAndWaiterCoexistence) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoop loop;
    fiber::net::detail::RWFd rwfd(loop, fds[0]);
    fiber::common::IoErr set_read_err = fiber::common::IoErr::Invalid;
    fiber::common::IoErr set_read_while_waiting_err = fiber::common::IoErr::Invalid;
    fiber::common::IoErr set_write_while_reading_err = fiber::common::IoErr::Invalid;
    fiber::common::IoResult<void> wait_while_callback = std::unexpected(fiber::common::IoErr::Invalid);
    fiber::common::IoResult<void> active_wait_result = std::unexpected(fiber::common::IoErr::Invalid);
    bool active_wait_done = false;

    fiber::async::spawn(loop, [&]() -> DetachedTask {
        set_read_err = rwfd.set_read_callback(&noop_ready_callback, nullptr);
        wait_while_callback = co_await rwfd.wait_readable();
        (void) rwfd.clear_read_callback(&noop_ready_callback, nullptr);

        fiber::async::spawn([&]() -> DetachedTask {
            active_wait_result = co_await rwfd.wait_readable();
            active_wait_done = true;
            co_return;
        });
        for (int i = 0; i < 20 && rwfd.read_callback_ == nullptr; ++i) {
            co_await fiber::async::sleep(1ms);
        }

        set_read_while_waiting_err = rwfd.set_read_callback(&noop_ready_callback, nullptr);
        set_write_while_reading_err = rwfd.set_write_callback(&noop_ready_callback, nullptr);
        (void) rwfd.clear_write_callback(&noop_ready_callback, nullptr);
        rwfd.close();
        (void) ::close(fds[1]);
        loop.stop();
        co_return;
    });

    loop.run();
    EXPECT_EQ(set_read_err, fiber::common::IoErr::None);
    ASSERT_FALSE(wait_while_callback);
    EXPECT_EQ(wait_while_callback.error(), fiber::common::IoErr::Busy);
    EXPECT_EQ(set_read_while_waiting_err, fiber::common::IoErr::Busy);
    EXPECT_EQ(set_write_while_reading_err, fiber::common::IoErr::None);
    EXPECT_TRUE(active_wait_done);
    ASSERT_FALSE(active_wait_result);
    EXPECT_EQ(active_wait_result.error(), fiber::common::IoErr::Canceled);
}

} // namespace
