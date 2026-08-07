#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <exception>
#include <future>
#include <utility>

#include <fiber/async/CoroutinePromiseBase.h>
#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/common/IoError.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/Http2Stream.h>
#include <fiber/http/detail/Http2BodyRecvState.h>
#include <fiber/http/detail/Http2HeaderBlockQueue.h>

namespace {

using namespace std::chrono_literals;
using fiber::async::DetachedTask;
using fiber::common::IoErr;
using fiber::event::EventLoop;
using fiber::http::Http2Stream;
using fiber::http::detail::Http2BodyRecvState;
using fiber::http::detail::Http2HeaderBlockQueue;

class ManualTask {
public:
    struct promise_type : fiber::async::CoroutinePromiseBase {
        ManualTask get_return_object() noexcept { return ManualTask(handle_type::from_promise(*this)); }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() { std::terminate(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit ManualTask(handle_type handle) noexcept : handle_(handle) {}
    ManualTask(const ManualTask &) = delete;
    ManualTask &operator=(const ManualTask &) = delete;
    ManualTask(ManualTask &&other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    ~ManualTask() { reset(); }

    void resume() const noexcept { handle_.resume(); }

    void reset() noexcept {
        if (!handle_) {
            return;
        }
        handle_type handle = std::exchange(handle_, nullptr);
        handle.destroy();
    }

private:
    handle_type handle_{};
};

struct DeferredHeaderAbort {
    Http2HeaderBlockQueue *queue = nullptr;
    EventLoop::DeferEntry entry;

    static void run(DeferredHeaderAbort *abort) noexcept { abort->queue->abort(IoErr::Canceled); }
};

struct DeferredBodyAbort {
    Http2BodyRecvState *state = nullptr;
    EventLoop::DeferEntry entry;

    static void run(DeferredBodyAbort *abort) noexcept { abort->state->abort(IoErr::Canceled); }
};

void on_stream_destroy(void *) noexcept {}

IoErr on_header_block_start(void *, fiber::http::Http2HpackDecoder::Sink &) noexcept { return IoErr::None; }

IoErr on_header_block_complete(void *, bool) noexcept { return IoErr::None; }

IoErr on_body(void *, fiber::mem::IoBuf &&, bool) noexcept { return IoErr::None; }

constexpr Http2Stream::Ops kStreamOps{
        .on_destroy = &on_stream_destroy,
        .on_header_block_start = &on_header_block_start,
        .on_header_block_complete = &on_header_block_complete,
        .on_body = &on_body,
};

DetachedTask abort_pending_header_read(std::promise<IoErr> *result) {
    fiber::mem::BufPool pool;
    Http2HeaderBlockQueue queue(pool);
    DeferredHeaderAbort abort{.queue = &queue};
    EventLoop &loop = EventLoop::current();
    loop.post_local<DeferredHeaderAbort, &DeferredHeaderAbort::entry, &DeferredHeaderAbort::run>(abort);

    auto read = co_await queue.read_header(std::chrono::milliseconds::max());
    result->set_value(read ? IoErr::None : read.error());
}

DetachedTask abort_pending_body_read(std::promise<IoErr> *result) {
    EventLoop &loop = EventLoop::current();
    Http2BodyRecvState state(loop.io_buf_node_pool());
    int owner = 0;
    Http2Stream stream(&owner, kStreamOps);
    DeferredBodyAbort abort{.state = &state};
    loop.post_local<DeferredBodyAbort, &DeferredBodyAbort::entry, &DeferredBodyAbort::run>(abort);

    auto read = co_await state.read_body(stream, 1, std::chrono::milliseconds::max());
    result->set_value(read ? IoErr::None : read.error());
}

ManualTask wait_for_header(Http2HeaderBlockQueue *queue, std::atomic<int> *resumed) {
    (void) co_await queue->read_header(std::chrono::milliseconds::max());
    resumed->fetch_add(1, std::memory_order_relaxed);
}

ManualTask wait_for_body(Http2BodyRecvState *state, Http2Stream *stream, std::atomic<int> *resumed) {
    (void) co_await state->read_body(*stream, 1, std::chrono::milliseconds::max());
    resumed->fetch_add(1, std::memory_order_relaxed);
}

DetachedTask destroy_notified_header_read(std::promise<int> *result) {
    fiber::mem::BufPool pool;
    Http2HeaderBlockQueue queue(pool);
    std::atomic<int> resumed{0};
    ManualTask pending = wait_for_header(&queue, &resumed);
    pending.resume();

    queue.abort(IoErr::Canceled);
    pending.reset();
    co_await fiber::async::sleep(1ms);
    result->set_value(resumed.load(std::memory_order_relaxed));
}

DetachedTask destroy_notified_body_read(std::promise<int> *result) {
    EventLoop &loop = EventLoop::current();
    Http2BodyRecvState state(loop.io_buf_node_pool());
    int owner = 0;
    Http2Stream stream(&owner, kStreamOps);
    std::atomic<int> resumed{0};
    ManualTask pending = wait_for_body(&state, &stream, &resumed);
    pending.resume();

    state.abort(IoErr::Canceled);
    pending.reset();
    co_await fiber::async::sleep(1ms);
    result->set_value(resumed.load(std::memory_order_relaxed));
}

TEST(Http2ReceiveAwaiterTest, HeaderAbortResumesFromSameLoopDefer) {
    fiber::event::EventLoopGroup group(1);
    std::promise<IoErr> result;
    auto future = result.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() { return abort_pending_header_read(&result); });
    const auto status = future.wait_for(1s);
    group.stop();
    group.join();

    ASSERT_EQ(status, std::future_status::ready);
    EXPECT_EQ(future.get(), IoErr::Canceled);
}

TEST(Http2ReceiveAwaiterTest, BodyAbortResumesFromSameLoopDefer) {
    fiber::event::EventLoopGroup group(1);
    std::promise<IoErr> result;
    auto future = result.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() { return abort_pending_body_read(&result); });
    const auto status = future.wait_for(1s);
    group.stop();
    group.join();

    ASSERT_EQ(status, std::future_status::ready);
    EXPECT_EQ(future.get(), IoErr::Canceled);
}

TEST(Http2ReceiveAwaiterTest, DestroyingNotifiedHeaderReadCancelsDeferredResume) {
    fiber::event::EventLoopGroup group(1);
    std::promise<int> result;
    auto future = result.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() { return destroy_notified_header_read(&result); });
    const auto status = future.wait_for(1s);
    group.stop();
    group.join();

    ASSERT_EQ(status, std::future_status::ready);
    EXPECT_EQ(future.get(), 0);
}

TEST(Http2ReceiveAwaiterTest, DestroyingNotifiedBodyReadCancelsDeferredResume) {
    fiber::event::EventLoopGroup group(1);
    std::promise<int> result;
    auto future = result.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() { return destroy_notified_body_read(&result); });
    const auto status = future.wait_for(1s);
    group.stop();
    group.join();

    ASSERT_EQ(status, std::future_status::ready);
    EXPECT_EQ(future.get(), 0);
}

} // namespace
