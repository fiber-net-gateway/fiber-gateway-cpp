#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <netinet/in.h>
#include <new>
#include <utility>

#include <sys/socket.h>
#include <unistd.h>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>

#define private public
#include <fiber/net/detail/RWFd.h>
#undef private

namespace {

using fiber::async::DetachedTask;
using namespace std::chrono_literals;

void noop_ready_callback(void *, fiber::common::IoErr) noexcept {}

struct ReplaceDuringDispatchCtx {
    fiber::event::EventLoop *loop = nullptr;
    fiber::net::detail::RWFd *current = nullptr;
    void *storage = nullptr;
    int replacement_fd = -1;
    fiber::net::detail::RWFd *replacement = nullptr;
    bool called = false;
    fiber::common::IoErr clear_err = fiber::common::IoErr::Invalid;
};

void replace_rwfd_on_read(void *raw_ctx, fiber::common::IoErr) noexcept {
    auto *ctx = static_cast<ReplaceDuringDispatchCtx *>(raw_ctx);
    ctx->called = true;
    ctx->clear_err = ctx->current->clear_read_callback(&replace_rwfd_on_read, ctx);
    ctx->current->~RWFd();
    const int replacement_fd = std::exchange(ctx->replacement_fd, -1);
    ctx->replacement = new (ctx->storage) fiber::net::detail::RWFd(*ctx->loop, replacement_fd);
    // Keep the replacement unsubscribed. If the old dispatch touches the same
    // address after this callback, it will incorrectly notify this slot.
    ctx->replacement->read_event_.callback = &noop_ready_callback;
    ctx->replacement->read_event_.ctx = nullptr;
}

struct CallbackResult {
    int calls = 0;
    fiber::common::IoErr err = fiber::common::IoErr::Invalid;
};

void record_callback(void *raw_ctx, fiber::common::IoErr err) noexcept {
    auto *ctx = static_cast<CallbackResult *>(raw_ctx);
    ++ctx->calls;
    ctx->err = err;
}

// One raw recv through the RWFd I/O wrapper so the direction state records the
// syscall result with stream semantics: a short read proves the receive queue
// drained (Blocked); only EAGAIN-free full reads and errors leave it otherwise.
fiber::common::IoResult<std::size_t> raw_recv_once(fiber::net::detail::RWFd &rwfd, void *buf,
                                                   std::size_t len) noexcept {
    return rwfd.read(
            [&](fiber::net::detail::RWFd::IoStateUpdate &state) noexcept -> fiber::common::IoResult<std::size_t> {
                const auto out = ::recv(rwfd.fd(), buf, len, MSG_DONTWAIT);
                if (out >= 0) {
                    if (static_cast<std::size_t>(out) < len) {
                        state.mark_blocked();
                    } else {
                        state.mark_ready();
                    }
                    return static_cast<std::size_t>(out);
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    state.mark_blocked();
                    return std::unexpected(fiber::common::IoErr::WouldBlock);
                }
                return std::unexpected(fiber::common::io_err_from_errno(errno));
            });
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
    // Observe readiness by doing I/O through the wrapper: a short read marks
    // the direction Blocked, so the next packet edge notifies again.
    char data[16] = {};
    const auto result = raw_recv_once(*ctx->rwfd, data, sizeof(data));
    if (!result || *result != 1) {
        ctx->io_ok = false;
        finish_persistent_read_callback(ctx);
        return;
    }

    ++ctx->calls;
    if (ctx->calls == 1) {
        if (data[0] != 'x') {
            ctx->io_ok = false;
            finish_persistent_read_callback(ctx);
            return;
        }
        const char next = 'y';
        if (::write(ctx->peer_fd, &next, 1) != 1) {
            ctx->io_ok = false;
            finish_persistent_read_callback(ctx);
        }
        return;
    }
    if (data[0] != 'y') {
        ctx->io_ok = false;
    }
    finish_persistent_read_callback(ctx);
}

TEST(RWFdTest, StaysUnregisteredUntilFirstSubscription) {
    int fds[2] = {-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoop loop;
    fiber::net::detail::RWFd rwfd(loop, fds[0]);
    bool registered_after_attach = true;
    bool registered_after_subscribe = false;
    bool registered_after_clear = false;
    fiber::event::IoEvent interest_after_clear = fiber::event::IoEvent::Terminal;

    fiber::async::spawn(loop, [&]() -> DetachedTask {
        registered_after_attach = rwfd.efd_.registered();
        EXPECT_EQ(rwfd.set_read_callback(&record_callback, nullptr), fiber::common::IoErr::None);
        registered_after_subscribe = rwfd.efd_.registered();
        EXPECT_EQ(rwfd.clear_read_callback(&record_callback, nullptr), fiber::common::IoErr::None);
        registered_after_clear = rwfd.efd_.registered();
        interest_after_clear = rwfd.efd_.watching();
        rwfd.close();
        (void) ::close(fds[1]);
        loop.stop();
        co_return;
    });

    loop.run();
    EXPECT_FALSE(registered_after_attach);
    EXPECT_TRUE(registered_after_subscribe);
    // Clearing drops the subscription but keeps kernel interest: a later
    // re-subscription within the same ownership epoch must not lose edges.
    EXPECT_TRUE(registered_after_clear);
    EXPECT_EQ(interest_after_clear, fiber::event::IoEvent::Read);
}

TEST(RWFdTest, FirstSubscriptionReceivesAlreadyPendingData) {
    int fds[2] = {-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoop loop;
    fiber::net::detail::RWFd rwfd(loop, fds[0]);
    CallbackResult result;

    fiber::async::spawn(loop, [&]() -> DetachedTask {
        const char byte = 'x';
        EXPECT_EQ(::write(fds[1], &byte, 1), 1);
        EXPECT_EQ(rwfd.set_read_callback(&record_callback, &result), fiber::common::IoErr::None);
        for (int i = 0; i < 100 && result.calls == 0; ++i) {
            co_await fiber::async::sleep(1ms);
        }
        EXPECT_EQ(result.calls, 1);
        EXPECT_EQ(rwfd.clear_read_callback(&record_callback, &result), fiber::common::IoErr::None);
        rwfd.close();
        (void) ::close(fds[1]);
        loop.stop();
        co_return;
    });

    loop.run();
    EXPECT_EQ(result.calls, 1);
    EXPECT_EQ(result.err, fiber::common::IoErr::None);
}

TEST(RWFdTest, IgnoresLateEventWithoutWaiter) {
    int fds[2] = {-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoop loop;
    std::atomic<bool> completed = false;
    bool observer_cleared = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        fiber::net::detail::RWFd rwfd(loop, fds[0]);
        rwfd.handle_events(fiber::event::IoEvent::Read | fiber::event::IoEvent::Write);
        observer_cleared = rwfd.dispatch_destroyed_observer_ == nullptr;
        rwfd.close();
        ::close(fds[1]);
        completed.store(true, std::memory_order_release);
        loop.stop();
        co_return;
    });

    loop.run();
    EXPECT_TRUE(completed.load(std::memory_order_acquire));
    EXPECT_TRUE(observer_cleared);
}

TEST(RWFdTest, StopsDispatchAfterCallbackDestroysAndReplacesOwner) {
    int original_fds[2] = {-1, -1};
    int replacement_fds[2] = {-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, original_fds), 0);
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, replacement_fds), 0);

    fiber::event::EventLoop loop;
    alignas(fiber::net::detail::RWFd) std::byte storage[sizeof(fiber::net::detail::RWFd)];
    fiber::common::IoErr set_err = fiber::common::IoErr::Invalid;
    fiber::common::IoErr clear_err = fiber::common::IoErr::Invalid;
    fiber::event::IoEvent replacement_watching = fiber::event::IoEvent::Terminal;
    bool callback_called = false;
    bool replacement_constructed = false;

    fiber::async::spawn(loop, [&]() -> DetachedTask {
        auto *rwfd =
                new (static_cast<void *>(storage)) fiber::net::detail::RWFd(loop, std::exchange(original_fds[0], -1));
        ReplaceDuringDispatchCtx ctx{
                &loop,
                rwfd,
                storage,
                std::exchange(replacement_fds[0], -1),
                nullptr,
                false,
                fiber::common::IoErr::Invalid,
        };

        set_err = rwfd->set_read_callback(&replace_rwfd_on_read, &ctx);
        if (set_err == fiber::common::IoErr::None) {
            rwfd->handle_events(fiber::event::IoEvent::Read);
            callback_called = ctx.called;
            clear_err = ctx.clear_err;
            replacement_constructed = ctx.replacement != nullptr;
            if (ctx.replacement) {
                replacement_watching = ctx.replacement->efd_.watching();
                ctx.replacement->read_event_.callback = nullptr;
                ctx.replacement->read_event_.ctx = nullptr;
                ctx.replacement->close();
                ctx.replacement->~RWFd();
            }
        } else {
            rwfd->~RWFd();
            if (ctx.replacement_fd >= 0) {
                (void) ::close(ctx.replacement_fd);
                ctx.replacement_fd = -1;
            }
        }

        (void) ::close(original_fds[1]);
        original_fds[1] = -1;
        (void) ::close(replacement_fds[1]);
        replacement_fds[1] = -1;
        loop.stop();
        co_return;
    });

    loop.run();
    for (int fd: original_fds) {
        if (fd >= 0) {
            (void) ::close(fd);
        }
    }
    for (int fd: replacement_fds) {
        if (fd >= 0) {
            (void) ::close(fd);
        }
    }

    EXPECT_EQ(set_err, fiber::common::IoErr::None);
    EXPECT_TRUE(callback_called);
    EXPECT_TRUE(replacement_constructed);
    EXPECT_EQ(clear_err, fiber::common::IoErr::None);
    EXPECT_EQ(replacement_watching, fiber::event::IoEvent::None);
}

TEST(RWFdTest, PersistentReadCallbackSeesLaterEdges) {
    int fds[2] = {-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

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
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

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
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

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

TEST(RWFdTest, ReadyToReadyDoesNotRenotify) {
    int fds[2] = {-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoop loop;
    fiber::net::detail::RWFd rwfd(loop, fds[0]);
    CallbackResult result;
    fiber::net::detail::RWFd::State state_after_second = fiber::net::detail::RWFd::State::Unknown;

    fiber::async::spawn(loop, [&]() -> DetachedTask {
        EXPECT_EQ(rwfd.set_read_callback(&record_callback, &result), fiber::common::IoErr::None);
        rwfd.handle_events(fiber::event::IoEvent::Read);
        EXPECT_EQ(result.calls, 1);
        rwfd.handle_events(fiber::event::IoEvent::Read);
        state_after_second = rwfd.read_state();
        EXPECT_EQ(result.calls, 1);
        EXPECT_EQ(rwfd.clear_read_callback(&record_callback, &result), fiber::common::IoErr::None);
        rwfd.close();
        (void) ::close(fds[1]);
        loop.stop();
        co_return;
    });

    loop.run();
    EXPECT_EQ(result.calls, 1);
    EXPECT_EQ(state_after_second, fiber::net::detail::RWFd::State::Ready);
}

TEST(RWFdTest, BlockedToReadyEdgeNotifiesLateSubscriber) {
    int fds[2] = {-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoop loop;
    fiber::net::detail::RWFd rwfd(loop, fds[0]);
    CallbackResult result;
    fiber::net::detail::RWFd::State state_after_drain = fiber::net::detail::RWFd::State::Unknown;

    fiber::async::spawn(loop, [&]() -> DetachedTask {
        const char byte = 'x';
        EXPECT_EQ(::write(fds[1], &byte, 1), 1);

        // Observe the pending byte and drain it to exhaustion so the direction
        // is Blocked again; the caller advanced its own I/O, so no callback
        // subscription existed while it was Ready.
        char sink[8];
        EXPECT_TRUE(raw_recv_once(rwfd, sink, sizeof(sink)).has_value());
        EXPECT_EQ(raw_recv_once(rwfd, sink, sizeof(sink)).error(), fiber::common::IoErr::WouldBlock);
        state_after_drain = rwfd.read_state();

        EXPECT_EQ(rwfd.set_read_callback(&record_callback, &result), fiber::common::IoErr::None);
        const char next = 'y';
        EXPECT_EQ(::write(fds[1], &next, 1), 1);
        for (int i = 0; i < 100 && result.calls == 0; ++i) {
            co_await fiber::async::sleep(1ms);
        }
        EXPECT_EQ(result.calls, 1);
        EXPECT_EQ(result.err, fiber::common::IoErr::None);
        EXPECT_EQ(rwfd.clear_read_callback(&record_callback, &result), fiber::common::IoErr::None);
        rwfd.close();
        (void) ::close(fds[1]);
        loop.stop();
        co_return;
    });

    loop.run();
    EXPECT_EQ(state_after_drain, fiber::net::detail::RWFd::State::Blocked);
    EXPECT_EQ(result.calls, 1);
    EXPECT_EQ(result.err, fiber::common::IoErr::None);
}

TEST(RWFdTest, IoLambdaRecordsDirectionState) {
    int fds[2] = {-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoop loop;
    fiber::net::detail::RWFd rwfd(loop, fds[0]);
    char sink[8];
    fiber::common::IoResult<std::size_t> drained;
    fiber::common::IoResult<std::size_t> exhausted;
    fiber::net::detail::RWFd::State state_after_drain = fiber::net::detail::RWFd::State::Unknown;

    fiber::async::spawn(loop, [&]() -> DetachedTask {
        const char byte = 'x';
        EXPECT_EQ(::write(fds[1], &byte, 1), 1);
        // A full read (the request is exactly the pending byte count) keeps the
        // direction Ready; the follow-up probe hits EAGAIN and marks Blocked.
        drained = raw_recv_once(rwfd, sink, 1);
        state_after_drain = rwfd.read_state();
        exhausted = raw_recv_once(rwfd, sink, 1);
        rwfd.close();
        (void) ::close(fds[1]);
        loop.stop();
        co_return;
    });

    loop.run();
    EXPECT_TRUE(drained.has_value());
    EXPECT_EQ(*drained, 1u);
    EXPECT_EQ(state_after_drain, fiber::net::detail::RWFd::State::Ready);
    EXPECT_FALSE(exhausted.has_value());
    EXPECT_EQ(exhausted.error(), fiber::common::IoErr::WouldBlock);
    EXPECT_EQ(rwfd.read_state(), fiber::net::detail::RWFd::State::Blocked);
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

    for (int i = 0; i < 200 && (rwfd.read_event_.callback != nullptr || rwfd.write_event_.callback != nullptr); ++i) {
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

    EXPECT_EQ(armed_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(read_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(write_future.wait_for(2s), std::future_status::ready);

    auto read_result = read_future.get();
    auto write_result = write_future.get();
    EXPECT_TRUE(read_result);
    EXPECT_TRUE(write_result);

    group.join();
}

TEST(RWFdTest, ReadinessCompletesBeforeTimeout) {
    int fds[2] = {-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

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
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoop loop;
    fiber::net::detail::RWFd rwfd(loop, fds[0]);
    fiber::common::IoResult<void> result = std::unexpected(fiber::common::IoErr::Invalid);
    bool callback_cleared = false;

    fiber::async::spawn(loop, [&]() -> DetachedTask {
        result = co_await rwfd.wait_readable(10ms);
        callback_cleared = rwfd.read_event_.callback == nullptr;
        rwfd.close();
        (void) ::close(fds[1]);
        loop.stop();
        co_return;
    });

    loop.run();
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error(), fiber::common::IoErr::TimedOut);
    EXPECT_TRUE(callback_cleared);
}

TEST(RWFdTest, NonPositiveTimeoutDoesNotRegisterCallback) {
    int fds[2] = {-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoop loop;
    fiber::net::detail::RWFd rwfd(loop, fds[0]);
    fiber::common::IoResult<void> result = std::unexpected(fiber::common::IoErr::Invalid);
    bool callback_cleared = false;
    bool never_registered = true;

    fiber::async::spawn(loop, [&]() -> DetachedTask {
        never_registered = !rwfd.efd_.registered();
        result = co_await rwfd.wait_readable(0ms);
        callback_cleared = rwfd.read_event_.callback == nullptr;
        rwfd.close();
        (void) ::close(fds[1]);
        loop.stop();
        co_return;
    });

    loop.run();
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error(), fiber::common::IoErr::TimedOut);
    EXPECT_TRUE(callback_cleared);
    EXPECT_TRUE(never_registered);
}

TEST(RWFdTest, CloseCancelsTimedWaiter) {
    int fds[2] = {-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

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
        while (rwfd.read_event_.callback == nullptr) {
            co_await fiber::async::sleep(1ms);
        }
        rwfd.close();
        (void) ::close(fds[1]);
        loop.stop();
        co_return;
    });

    loop.run();
    EXPECT_TRUE(completed);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error(), fiber::common::IoErr::Canceled);
}

TEST(RWFdTest, RejectsSameDirectionCallbackAndWaiterCoexistence) {
    int fds[2] = {-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

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
        for (int i = 0; i < 20 && rwfd.read_event_.callback == nullptr; ++i) {
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
    EXPECT_FALSE(wait_while_callback);
    EXPECT_EQ(wait_while_callback.error(), fiber::common::IoErr::Busy);
    EXPECT_EQ(set_read_while_waiting_err, fiber::common::IoErr::Busy);
    EXPECT_EQ(set_write_while_reading_err, fiber::common::IoErr::None);
    EXPECT_TRUE(active_wait_done);
    EXPECT_FALSE(active_wait_result);
    EXPECT_EQ(active_wait_result.error(), fiber::common::IoErr::Canceled);
}

namespace {
struct ReplaceSubscription {
    fiber::net::detail::RWFd &fd;
    int writes = 0;
    static void write(void *ctx, fiber::common::IoErr err) noexcept {
        if (err == fiber::common::IoErr::None) {
            ++static_cast<ReplaceSubscription *>(ctx)->writes;
        }
    }
    static void read(void *ctx, fiber::common::IoErr) noexcept {
        auto &self = *static_cast<ReplaceSubscription *>(ctx);
        EXPECT_EQ(self.fd.clear_write_callback(&write, ctx), fiber::common::IoErr::None);
        // Advance the write direction by doing I/O before re-subscribing: a
        // WouldBlock marks it Blocked, which is the only legal subscription
        // moment under the contract.
        (void) self.fd.write(
                [](fiber::net::detail::RWFd::IoStateUpdate &state) noexcept -> fiber::common::IoResult<std::size_t> {
                    state.mark_blocked();
                    return std::unexpected(fiber::common::IoErr::WouldBlock);
                });
        EXPECT_EQ(self.fd.set_write_callback(&write, ctx), fiber::common::IoErr::None);
        EXPECT_EQ(self.fd.clear_read_callback(&read, ctx), fiber::common::IoErr::None);
    }
};
} // namespace

TEST(RWFdTest, SameBatchSubscriptionReplacementSkipsStaleEventWithoutRearming) {
    int fds[2];
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
    fiber::event::EventLoop loop;
    fiber::async::spawn(loop, [&]() -> DetachedTask {
        fiber::net::detail::RWFd fd(loop, fds[0], fiber::net::detail::RWFd::Kind::Stream);
        ReplaceSubscription context{fd};
        EXPECT_EQ(fd.set_read_callback(&ReplaceSubscription::read, &context), fiber::common::IoErr::None);
        EXPECT_EQ(fd.set_write_callback(&ReplaceSubscription::write, &context), fiber::common::IoErr::None);
        const auto epoch = fd.efd_.epoch();
        fd.handle_events(fiber::event::IoEvent::Read | fiber::event::IoEvent::Write);
        // The write slot was replaced during dispatch: its generation moved on,
        // so the in-flight write event is dropped instead of being replayed.
        EXPECT_EQ(context.writes, 0);
        EXPECT_EQ(fd.write_state(), fiber::net::detail::RWFd::State::Blocked);
        co_await fiber::async::sleep(2ms);
        // No deferred re-arm exists, but the freshly registered Write interest
        // legitimately reports the first kernel edge (the socket is writable).
        EXPECT_EQ(context.writes, 1);
        EXPECT_EQ(fd.efd_.epoch(), epoch);
        fd.close();
        ::close(fds[1]);
        loop.stop();
    });
    loop.run();
}

TEST(RWFdTest, HandoverDetachesAndReadoptsOnTargetLoop) {
    int fds[2] = {-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoop origin_loop;
    fiber::event::EventLoop target_loop;
    fiber::net::detail::RWFd rwfd(origin_loop, fds[0]);
    CallbackResult result;
    bool registered_after_detach = true;
    bool registered_after_adopt_subscribe = false;
    bool adopted_current_loop = false;

    fiber::async::spawn(origin_loop, [&]() -> DetachedTask {
        EXPECT_EQ(rwfd.set_read_callback(&record_callback, &result), fiber::common::IoErr::None);
        EXPECT_EQ(rwfd.clear_read_callback(&record_callback, &result), fiber::common::IoErr::None);
        EXPECT_EQ(rwfd.detach_for_handover(), fiber::common::IoErr::None);
        registered_after_detach = rwfd.efd_.registered();
        origin_loop.stop();
        co_return;
    });
    origin_loop.run();

    fiber::async::spawn(target_loop, [&]() -> DetachedTask {
        rwfd.adopt_loop(target_loop);
        adopted_current_loop = &rwfd.current_loop() == &target_loop;
        EXPECT_EQ(rwfd.set_read_callback(&record_callback, &result), fiber::common::IoErr::None);
        registered_after_adopt_subscribe = rwfd.efd_.registered();
        const char byte = 'x';
        EXPECT_EQ(::write(fds[1], &byte, 1), 1);
        for (int i = 0; i < 100 && result.calls == 0; ++i) {
            co_await fiber::async::sleep(1ms);
        }
        EXPECT_EQ(result.calls, 1);
        EXPECT_EQ(rwfd.clear_read_callback(&record_callback, &result), fiber::common::IoErr::None);
        rwfd.close();
        (void) ::close(fds[1]);
        fds[1] = -1;
        target_loop.stop();
        co_return;
    });
    target_loop.run();

    if (fds[1] >= 0) {
        (void) ::close(fds[1]);
    }
    EXPECT_FALSE(registered_after_detach);
    EXPECT_TRUE(adopted_current_loop);
    EXPECT_TRUE(registered_after_adopt_subscribe);
    EXPECT_EQ(result.calls, 1);
    EXPECT_EQ(result.err, fiber::common::IoErr::None);
}

TEST(RWFdTest, RejectsAdoptionOntoStoppingLoop) {
    int fds[2] = {-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoop origin_loop;
    fiber::event::EventLoop target_loop;
    fiber::net::detail::RWFd rwfd(origin_loop, fds[0]);

    fiber::async::spawn(origin_loop, [&]() -> DetachedTask {
        EXPECT_EQ(rwfd.detach_for_handover(), fiber::common::IoErr::None);
        origin_loop.stop();
        co_return;
    });
    origin_loop.run();

    fiber::async::spawn(target_loop, [&]() -> DetachedTask {
        // The target loop is asked to stop before the adoption runs: nobody
        // would clean up after it there, so adoption is refused and the fd is
        // closed by the adoption itself.
        target_loop.stop();
        EXPECT_EQ(rwfd.adopt_loop(target_loop), fiber::common::IoErr::Canceled);
        EXPECT_FALSE(rwfd.valid());
        co_return;
    });
    target_loop.run();

    if (fds[1] >= 0) {
        (void) ::close(fds[1]);
    }
}

namespace {
struct DetachedCloseWaiter : fiber::net::detail::RWFdWaiterBase {
    int completions = 0;
    fiber::common::IoErr err = fiber::common::IoErr::Invalid;
    static void done(fiber::net::detail::RWFdWaiterBase *base, fiber::common::IoErr err) noexcept {
        auto *self = static_cast<DetachedCloseWaiter *>(base);
        self->err = err;
        ++self->completions;
    }
};
struct DeletingWaiter : fiber::net::detail::RWFdWaiterBase {
    DetachedCloseWaiter *sibling = nullptr;
    int completions = 0;
    static void done(fiber::net::detail::RWFdWaiterBase *base, fiber::common::IoErr err) noexcept {
        auto *self = static_cast<DeletingWaiter *>(base);
        (void) err;
        ++self->completions;
        // Destroying the RWFd from the first completion must not affect the
        // second one: completions run detached, on locals only.
        delete self->rwfd_;
        self->rwfd_ = nullptr;
    }
};
} // namespace

TEST(RWFdTest, CloseCompletesDetachedWaitersWhenFirstCompletionDestroysOwner) {
    int fds[2];
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
    fiber::event::EventLoop loop;
    DetachedCloseWaiter write_waiter;
    auto *read_waiter = new DeletingWaiter();
    fiber::async::spawn(loop, [&]() -> DetachedTask {
        auto *fd = new fiber::net::detail::RWFd(loop, fds[0], fiber::net::detail::RWFd::Kind::Stream);
        read_waiter->rwfd_ = fd;
        read_waiter->event_ = fiber::event::IoEvent::Read;
        read_waiter->complete_callback_ = &DeletingWaiter::done;
        write_waiter.rwfd_ = fd;
        write_waiter.event_ = fiber::event::IoEvent::Write;
        write_waiter.complete_callback_ = &DetachedCloseWaiter::done;
        EXPECT_EQ(fd->begin_wait(read_waiter), fiber::common::IoErr::None);
        EXPECT_EQ(fd->begin_wait(&write_waiter), fiber::common::IoErr::None);
        fd->close();
        ::close(fds[1]);
        loop.stop();
        co_return;
    });
    loop.run();
    EXPECT_EQ(read_waiter->completions, 1);
    EXPECT_EQ(write_waiter.completions, 1);
    EXPECT_EQ(write_waiter.err, fiber::common::IoErr::Canceled);
    delete read_waiter;
}

} // namespace
