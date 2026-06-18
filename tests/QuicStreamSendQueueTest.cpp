#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <string_view>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "event/EventLoopGroup.h"
#include "quic/QuicStreamSendQueue.h"

namespace fiber::quic {

struct QuicStreamSendQueueTestAccess {
    static common::IoResult<QuicStreamSendQueue::EncodedFrameResult>
    encode_stream_frame(QuicStreamSendQueue &queue, std::uint64_t stream_id, std::uint8_t *dst,
                        std::size_t capacity) noexcept {
        return queue.encode_stream_frame(stream_id, dst, capacity);
    }

    static common::IoResult<void> mark_acked(QuicStreamSendQueue &queue, std::size_t offset, std::size_t length,
                                             bool encoded_fin) noexcept {
        return queue.mark_acked(offset, length, encoded_fin);
    }

    static void update_max_stream_data(QuicStreamSendQueue &queue, std::uint64_t limit) noexcept {
        queue.update_max_stream_data(limit);
    }
};

} // namespace fiber::quic

namespace {

using DetachedTask = fiber::async::DetachedTask;

struct AppendResult {
    bool ok = false;
    std::size_t value = 0;
    fiber::common::IoErr error = fiber::common::IoErr::None;
};

fiber::mem::IoBuf iobuf_of(std::string_view value) {
    fiber::mem::IoBuf buf = fiber::mem::IoBuf::allocate(value.size());
    if (!buf) {
        return {};
    }
    if (!value.empty()) {
        std::memcpy(buf.writable_data(), value.data(), value.size());
        buf.commit(value.size());
    }
    return buf;
}

AppendResult to_append_result(fiber::common::IoResult<std::size_t> result) {
    if (result) {
        return {.ok = true, .value = *result};
    }
    return {.ok = false, .error = result.error()};
}

DetachedTask append_one(fiber::quic::QuicStreamSendQueue *queue, std::promise<AppendResult> *done) {
    auto result = co_await queue->append(iobuf_of("!"));
    done->set_value(to_append_result(result));
    fiber::event::EventLoop::current().stop();
}

DetachedTask append_one_with_timeout(fiber::quic::QuicStreamSendQueue *queue, std::promise<AppendResult> *done,
                                     std::chrono::milliseconds timeout) {
    auto result = co_await queue->append(iobuf_of("!"), false, timeout);
    done->set_value(to_append_result(result));
    fiber::event::EventLoop::current().stop();
}

DetachedTask ack_after_delay(fiber::quic::QuicStreamSendQueue *queue, std::atomic<bool> *waiter_seen) {
    co_await fiber::async::sleep(std::chrono::milliseconds(20));
    waiter_seen->store(queue->has_append_waiter(), std::memory_order_relaxed);

    std::array<std::uint8_t, 64> out{};
    auto encoded = fiber::quic::QuicStreamSendQueueTestAccess::encode_stream_frame(*queue, 4, out.data(), out.size());
    if (encoded && encoded->encoded) {
        (void) fiber::quic::QuicStreamSendQueueTestAccess::mark_acked(*queue, encoded->offset, encoded->data_len,
                                                                      encoded->fin);
    }
}

DetachedTask update_window_after_delay(fiber::quic::QuicStreamSendQueue *queue, std::atomic<bool> *waiter_seen,
                                       std::uint64_t limit) {
    co_await fiber::async::sleep(std::chrono::milliseconds(20));
    waiter_seen->store(queue->has_append_waiter(), std::memory_order_relaxed);
    fiber::quic::QuicStreamSendQueueTestAccess::update_max_stream_data(*queue, limit);
}

DetachedTask reset_after_delay(fiber::quic::QuicStreamSendQueue *queue, std::atomic<bool> *waiter_seen,
                               std::promise<fiber::common::IoResult<std::uint64_t>> *reset_done) {
    co_await fiber::async::sleep(std::chrono::milliseconds(20));
    waiter_seen->store(queue->has_append_waiter(), std::memory_order_relaxed);
    reset_done->set_value(queue->reset(42));
}

} // namespace

TEST(QuicStreamSendQueueTest, AppendWaitsForBufferedBytesToBeAcked) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamSendQueue queue(pool, {.buffer_limit = 5, .max_stream_data = 64});

    ASSERT_TRUE(queue.try_append(iobuf_of("hello")).has_value());
    EXPECT_EQ(queue.buffer_available(), 0u);

    fiber::event::EventLoopGroup group(1);
    std::promise<AppendResult> done;
    auto future = done.get_future();
    std::atomic<bool> waiter_seen{false};

    group.start();
    fiber::async::spawn(group.at(0), [&queue, &done]() { return append_one(&queue, &done); });
    fiber::async::spawn(group.at(0), [&queue, &waiter_seen]() { return ack_after_delay(&queue, &waiter_seen); });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "append did not resume after ack";
        return;
    }

    AppendResult result = future.get();
    EXPECT_TRUE(waiter_seen.load(std::memory_order_relaxed));
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.value, 1u);
    EXPECT_EQ(queue.ready_bytes(), 1u);
    group.join();
}

TEST(QuicStreamSendQueueTest, AppendWaitsForMaxStreamDataIncrease) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamSendQueue queue(pool, {.buffer_limit = 64, .max_stream_data = 5});

    ASSERT_TRUE(queue.try_append(iobuf_of("hello")).has_value());
    EXPECT_EQ(queue.stream_data_available(), 0u);

    fiber::event::EventLoopGroup group(1);
    std::promise<AppendResult> done;
    auto future = done.get_future();
    std::atomic<bool> waiter_seen{false};

    group.start();
    fiber::async::spawn(group.at(0), [&queue, &done]() { return append_one(&queue, &done); });
    fiber::async::spawn(group.at(0),
                        [&queue, &waiter_seen]() { return update_window_after_delay(&queue, &waiter_seen, 6); });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "append did not resume after max_stream_data update";
        return;
    }

    AppendResult result = future.get();
    EXPECT_TRUE(waiter_seen.load(std::memory_order_relaxed));
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.value, 1u);
    EXPECT_EQ(queue.total_appended_bytes(), 6u);
    group.join();
}

TEST(QuicStreamSendQueueTest, ResetWakesBlockedAppend) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamSendQueue queue(pool, {.buffer_limit = 5, .max_stream_data = 64});

    ASSERT_TRUE(queue.try_append(iobuf_of("hello")).has_value());

    fiber::event::EventLoopGroup group(1);
    std::promise<AppendResult> done;
    auto future = done.get_future();
    std::promise<fiber::common::IoResult<std::uint64_t>> reset_done;
    auto reset_future = reset_done.get_future();
    std::atomic<bool> waiter_seen{false};

    group.start();
    fiber::async::spawn(group.at(0), [&queue, &done]() { return append_one(&queue, &done); });
    fiber::async::spawn(group.at(0), [&queue, &waiter_seen, &reset_done]() {
        return reset_after_delay(&queue, &waiter_seen, &reset_done);
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready ||
        reset_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "reset did not wake blocked append";
        return;
    }

    auto reset = reset_future.get();
    ASSERT_TRUE(reset.has_value());
    EXPECT_EQ(*reset, 5u);

    AppendResult result = future.get();
    EXPECT_TRUE(waiter_seen.load(std::memory_order_relaxed));
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, fiber::common::IoErr::BrokenPipe);
    EXPECT_TRUE(queue.reset_sent());
    EXPECT_EQ(queue.final_size(), 5u);
    EXPECT_EQ(queue.reset_error_code(), 42u);
    group.join();
}

TEST(QuicStreamSendQueueTest, AppendLargerThanBufferLimitFailsImmediately) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamSendQueue queue(pool, {.buffer_limit = 2, .max_stream_data = 64});

    auto result = queue.try_append(iobuf_of("abc"));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), fiber::common::IoErr::MessageTooLarge);
}

TEST(QuicStreamSendQueueTest, AppendTimesOutWhenBlockedTooLong) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamSendQueue queue(pool, {.buffer_limit = 5, .max_stream_data = 64});

    ASSERT_TRUE(queue.try_append(iobuf_of("hello")).has_value());

    fiber::event::EventLoopGroup group(1);
    std::promise<AppendResult> done;
    auto future = done.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&queue, &done]() {
        return append_one_with_timeout(&queue, &done, std::chrono::milliseconds(20));
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "append did not time out";
        return;
    }

    AppendResult result = future.get();
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, fiber::common::IoErr::TimedOut);
    EXPECT_FALSE(queue.has_append_waiter());
    group.join();
}

TEST(QuicStreamSendQueueTest, AppendZeroTimeoutFailsImmediatelyWhenBlocked) {
    fiber::mem::IoBufNodePool pool;
    fiber::quic::QuicStreamSendQueue queue(pool, {.buffer_limit = 5, .max_stream_data = 64});

    ASSERT_TRUE(queue.try_append(iobuf_of("hello")).has_value());

    fiber::event::EventLoopGroup group(1);
    std::promise<AppendResult> done;
    auto future = done.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&queue, &done]() {
        return append_one_with_timeout(&queue, &done, std::chrono::milliseconds(0));
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "append did not return zero-timeout result";
        return;
    }

    AppendResult result = future.get();
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, fiber::common::IoErr::TimedOut);
    EXPECT_FALSE(queue.has_append_waiter());
    group.join();
}
