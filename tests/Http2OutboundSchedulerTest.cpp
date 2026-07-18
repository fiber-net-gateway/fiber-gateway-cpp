#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <future>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "event/EventLoopGroup.h"
#define private public
#include "http/Http2OutboundScheduler.h"
#include "http/HttpTransport.h"
#undef private

#include "HttpTransportStub.h"

namespace {

using fiber::async::DetachedTask;

class RecordingTransport final : public fiber::test::HttpTransportStub {
public:
    explicit RecordingTransport(std::vector<std::size_t> write_steps = {},
                                fiber::common::IoErr write_error = fiber::common::IoErr::None) :
        write_steps_(std::move(write_steps)), write_error_(write_error) {}

    fiber::async::Task<fiber::common::IoResult<void>> handshake(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<void>> shutdown(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<void>> wait_readable(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::common::IoErr poll_write(const void *buf, size_t len, size_t &out,
                                    fiber::event::IoEvent &wait_event) noexcept override {
        out = 0;
        wait_event = fiber::event::IoEvent::None;
        if (closed_) {
            return fiber::common::IoErr::ConnReset;
        }
        if (write_error_ != fiber::common::IoErr::None) {
            return write_error_;
        }
        out = len;
        if (write_call_count_ < write_steps_.size()) {
            out = std::min(out, write_steps_[write_call_count_]);
        }
        const auto *ptr = static_cast<const char *>(buf);
        written_.append(ptr, out);
        ++write_call_count_;
        return fiber::common::IoErr::None;
    }

    fiber::common::IoErr poll_writev(fiber::mem::IoBufChain &buf, size_t &out,
                                     fiber::event::IoEvent &wait_event) noexcept override {
        auto *front = buf.first_readable();
        if (!front) {
            out = 0;
            wait_event = fiber::event::IoEvent::None;
            return fiber::common::IoErr::None;
        }
        fiber::common::IoErr err = poll_write(front->readable_data(), front->readable(), out, wait_event);
        if (err == fiber::common::IoErr::None) {
            buf.consume_and_compact(out);
        }
        return err;
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> read(void *, size_t, std::chrono::milliseconds) override {
        co_return std::unexpected(fiber::common::IoErr::NotSupported);
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> read_into(fiber::mem::IoBuf &,
                                                                  std::chrono::milliseconds) override {
        co_return std::unexpected(fiber::common::IoErr::NotSupported);
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> readv_into(fiber::mem::IoBufChain &,
                                                                   std::chrono::milliseconds) override {
        co_return std::unexpected(fiber::common::IoErr::NotSupported);
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> write(const void *buf, size_t len,
                                                              std::chrono::milliseconds) override {
        if (closed_) {
            co_return std::unexpected(fiber::common::IoErr::ConnReset);
        }
        if (write_error_ != fiber::common::IoErr::None) {
            co_return std::unexpected(write_error_);
        }

        std::size_t take = len;
        if (write_call_count_ < write_steps_.size()) {
            take = std::min(take, write_steps_[write_call_count_]);
        }
        const auto *ptr = static_cast<const char *>(buf);
        written_.append(ptr, take);
        ++write_call_count_;
        co_return take;
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> write(fiber::mem::IoBuf &buf,
                                                              std::chrono::milliseconds timeout) override {
        auto result = co_await write(buf.readable_data(), buf.readable(), timeout);
        if (result) {
            buf.consume(*result);
        }
        co_return result;
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> writev(fiber::mem::IoBufChain &buf,
                                                               std::chrono::milliseconds timeout) override {
        auto *front = buf.first_readable();
        if (!front) {
            co_return static_cast<size_t>(0);
        }
        auto result = co_await write(front->readable_data(), front->readable(), timeout);
        if (result) {
            buf.consume_and_compact(*result);
        }
        co_return result;
    }

    void close() override { closed_ = true; }
    [[nodiscard]] bool valid() const noexcept override { return !closed_; }
    [[nodiscard]] int fd() const noexcept override { return -1; }
    [[nodiscard]] std::string_view negotiated_alpn() const noexcept override { return "h2"; }
    [[nodiscard]] const fiber::net::SocketAddress &remote_addr() const noexcept override { return remote_addr_; }
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept override {
        if (loop_) {
            return *loop_;
        }
        if (auto *current = fiber::event::EventLoop::current_or_null()) {
            return *current;
        }
        return fallback_loop_;
    }
    [[nodiscard]] const std::string &written() const noexcept { return written_; }

private:
    std::vector<std::size_t> write_steps_;
    std::size_t write_call_count_ = 0;
    bool closed_ = false;
    std::string written_;
    fiber::common::IoErr write_error_ = fiber::common::IoErr::None;
    fiber::net::SocketAddress remote_addr_{};
    fiber::event::EventLoop *loop_ = fiber::event::EventLoop::current_or_null();
    mutable fiber::event::EventLoop fallback_loop_{};
};

struct DummyStreamOwner final : fiber::http::Http2OutboundOperation {
    std::string first_batch;
    std::string second_batch;
    bool block_on_zero_conn_window = false;
    bool close_after_first_batch = false;
    std::size_t encode_calls = 0;

    fiber::common::IoErr encode_outbound_batch(fiber::http::Http2Stream &,
                                               const fiber::http::Http2OutboundEncodeRequest &req,
                                               fiber::http::Http2OutboundEncodeTarget &target,
                                               fiber::http::Http2OutboundEncodeResult &result) noexcept override {
        ++encode_calls;
        if (encode_calls == 1) {
            if (block_on_zero_conn_window && req.conn_window_budget == 0) {
                result.status = fiber::http::Http2OutboundEncodeResult::Status::BlockedConnWindow;
                result.next_kind = fiber::http::Http2OutboundNextKind::Data;
                return fiber::common::IoErr::None;
            }
            if (!first_batch.empty()) {
                fiber::common::IoErr err = target.append_copy(first_batch.data(), first_batch.size());
                if (err != fiber::common::IoErr::None) {
                    return err;
                }
            }
            result.status = close_after_first_batch ? fiber::http::Http2OutboundEncodeResult::Status::Closed
                                                    : fiber::http::Http2OutboundEncodeResult::Status::Encoded;
            result.next_kind = second_batch.empty() ? fiber::http::Http2OutboundNextKind::None
                                                    : fiber::http::Http2OutboundNextKind::Data;
            result.flow_controlled_bytes = static_cast<std::uint32_t>(first_batch.size());
            return fiber::common::IoErr::None;
        }

        if (encode_calls == 2 && !second_batch.empty()) {
            fiber::common::IoErr err = target.append_copy(second_batch.data(), second_batch.size());
            if (err != fiber::common::IoErr::None) {
                return err;
            }
            result.status = fiber::http::Http2OutboundEncodeResult::Status::Encoded;
            result.next_kind = fiber::http::Http2OutboundNextKind::None;
            result.flow_controlled_bytes = static_cast<std::uint32_t>(second_batch.size());
            return fiber::common::IoErr::None;
        }

        result.status = fiber::http::Http2OutboundEncodeResult::Status::NoWork;
        result.next_kind = fiber::http::Http2OutboundNextKind::None;
        return fiber::common::IoErr::None;
    }

    void on_outbound_abort(fiber::common::IoErr) noexcept override {}
};

void on_destroy(void *) noexcept {}

fiber::common::IoErr on_header_block_start(void *, fiber::http::Http2HpackDecoder::Sink &) noexcept {
    return fiber::common::IoErr::None;
}

fiber::common::IoErr on_header_block_complete(void *, bool) noexcept { return fiber::common::IoErr::None; }

fiber::common::IoErr on_body(void *, fiber::mem::IoBuf &&, bool) noexcept { return fiber::common::IoErr::None; }

void on_abort(void *, fiber::common::IoErr) noexcept {}

const fiber::http::Http2Stream::Ops kStreamOps{
        &on_destroy, &on_header_block_start, &on_header_block_complete, &on_body, &on_abort,
};

DetachedTask run_scheduler_and_stop(fiber::http::Http2OutboundScheduler *scheduler, RecordingTransport *transport,
                                    std::promise<fiber::common::IoErr> *promise, fiber::event::EventLoopGroup *group) {
    while (!scheduler->stopped()) {
        auto result = scheduler->pump_write(*transport, 64, 256 * 1024);
        if (!result) {
            break;
        }
        if (!result->needs_reschedule && !scheduler->closed()) {
            co_await fiber::async::sleep(std::chrono::milliseconds(1));
        }
    }
    promise->set_value(scheduler->stop_reason());
    group->stop();
    co_return;
}

DetachedTask enqueue_control_and_close(fiber::http::Http2OutboundScheduler *scheduler,
                                       std::vector<std::string> frames) {
    for (const std::string &frame: frames) {
        EXPECT_EQ(scheduler->alloc_and_enqueue_control(
                          frame.size(),
                          [&](std::uint8_t *dst) noexcept { std::memcpy(dst, frame.data(), frame.size()); }),
                  fiber::common::IoErr::None);
    }
    scheduler->close();
    co_return;
}

DetachedTask request_stream_send_and_close(fiber::http::Http2OutboundScheduler *scheduler,
                                           fiber::http::Http2Stream *stream, fiber::http::Http2OutboundNextKind kind,
                                           DummyStreamOwner *owner) {
    EXPECT_TRUE(stream->try_arm_outbound(*owner));
    EXPECT_EQ(scheduler->request_send(*stream, kind), fiber::common::IoErr::None);
    scheduler->close();
    co_return;
}

DetachedTask unblock_conn_window_then_close(fiber::http::Http2OutboundScheduler *scheduler,
                                            fiber::http::Http2Stream *stream, DummyStreamOwner *owner,
                                            RecordingTransport *transport) {
    EXPECT_TRUE(stream->try_arm_outbound(*owner));
    EXPECT_EQ(scheduler->request_send(*stream, fiber::http::Http2OutboundNextKind::Data), fiber::common::IoErr::None);
    while (scheduler->waiting_conn_window_stream_count() == 0) {
        co_await fiber::async::sleep(std::chrono::milliseconds(1));
    }
    scheduler->set_connection_send_window(4);
    scheduler->on_connection_window_available();
    co_await fiber::async::sleep(std::chrono::milliseconds(5));
    (void) transport;
    scheduler->close();
    co_return;
}

} // namespace

TEST(Http2OutboundSchedulerTest, AllocatesControlBufferAtRequestedSizeAndReleasesItOnAbort) {
    struct Outcome {
        fiber::common::IoErr enqueue_result = fiber::common::IoErr::Invalid;
        std::size_t pending_before_abort = 0;
        std::size_t capacity = 0;
        std::size_t pending_after_abort = 0;
        bool chain_empty_after_abort = false;
        bool uses_event_loop_pool = false;
    };

    fiber::event::EventLoopGroup group(1);
    fiber::http::Http2OutboundScheduler scheduler;
    std::promise<Outcome> promise;
    auto future = promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        Outcome outcome;
        outcome.enqueue_result = scheduler.alloc_and_enqueue_control(
                13, [](std::uint8_t *dst, std::size_t bytes) noexcept { std::memset(dst, 0, bytes); });
        outcome.pending_before_abort = scheduler.pending_control_bytes();
        if (fiber::mem::IoBuf *control = scheduler.control_chain_.first_readable()) {
            outcome.capacity = control->capacity();
        }
        outcome.uses_event_loop_pool =
                &scheduler.control_chain_.node_pool() == &fiber::event::EventLoop::current().io_buf_node_pool();

        scheduler.abort();
        outcome.pending_after_abort = scheduler.pending_control_bytes();
        outcome.chain_empty_after_abort = scheduler.control_chain_.empty();
        promise.set_value(outcome);
        group.stop();
        co_return;
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "control allocation did not finish in time";
        return;
    }

    const Outcome outcome = future.get();
    group.join();

    EXPECT_EQ(outcome.enqueue_result, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.pending_before_abort, 13U);
    EXPECT_EQ(outcome.capacity, 13U);
    EXPECT_TRUE(outcome.uses_event_loop_pool);
    EXPECT_EQ(outcome.pending_after_abort, 0U);
    EXPECT_TRUE(outcome.chain_empty_after_abort);
}

TEST(Http2OutboundSchedulerTest, DrainsQueuedControlBytes) {
    fiber::event::EventLoopGroup group(1);
    RecordingTransport transport({3, 2, 16});
    fiber::http::Http2OutboundScheduler scheduler;

    std::promise<fiber::common::IoErr> done_promise;
    auto done_future = done_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_scheduler_and_stop(&scheduler, &transport, &done_promise, &group); });
    fiber::async::spawn(group.at(0), [&]() { return enqueue_control_and_close(&scheduler, {"PING", "ACK!"}); });

    if (done_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "scheduler pump did not finish in time";
        return;
    }

    EXPECT_EQ(done_future.get(), fiber::common::IoErr::None);
    group.join();

    EXPECT_EQ(transport.written(), "PINGACK!");
    EXPECT_TRUE(scheduler.idle());
    EXPECT_EQ(scheduler.pending_control_bytes(), 0U);
}

TEST(Http2OutboundSchedulerTest, StopsWhenCloseDrainExactlyConsumesOperationBudget) {
    struct Outcome {
        fiber::common::IoErr error = fiber::common::IoErr::Invalid;
        bool pump_stopped = false;
        bool scheduler_stopped = false;
    };

    fiber::event::EventLoopGroup group(1);
    RecordingTransport transport;
    fiber::http::Http2OutboundScheduler scheduler;
    std::promise<Outcome> promise;
    auto future = promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        fiber::common::IoErr err = scheduler.alloc_and_enqueue_control(
                1, [](std::uint8_t *dst) noexcept { dst[0] = static_cast<std::uint8_t>('x'); });
        scheduler.close();
        auto result = scheduler.pump_write(transport, 1, 1);
        Outcome outcome;
        outcome.error = result ? fiber::common::IoErr::None : result.error();
        outcome.pump_stopped = result && result->stopped;
        outcome.scheduler_stopped = scheduler.stopped();
        if (err != fiber::common::IoErr::None) {
            outcome.error = err;
        }
        promise.set_value(outcome);
        group.stop();
        co_return;
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "scheduler pump did not finish in time";
        return;
    }
    Outcome outcome = future.get();
    group.join();

    EXPECT_EQ(outcome.error, fiber::common::IoErr::None);
    EXPECT_TRUE(outcome.pump_stopped);
    EXPECT_TRUE(outcome.scheduler_stopped);
    EXPECT_EQ(transport.written(), "x");
}

TEST(Http2OutboundSchedulerTest, EncodesReservesBothWindowsAndSendsDataBatchBeforeClosing) {
    fiber::event::EventLoopGroup group(1);
    RecordingTransport transport({2, 8});
    fiber::http::Http2OutboundScheduler scheduler;
    scheduler.set_connection_send_window(16);

    DummyStreamOwner owner;
    owner.first_batch = "PAYLOAD";
    fiber::http::Http2Stream stream(&owner, kStreamOps);
    stream.stream_id_ = 1;

    std::promise<fiber::common::IoErr> done_promise;
    auto done_future = done_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_scheduler_and_stop(&scheduler, &transport, &done_promise, &group); });
    fiber::async::spawn(group.at(0), [&]() {
        return request_stream_send_and_close(&scheduler, &stream, fiber::http::Http2OutboundNextKind::Data, &owner);
    });

    if (done_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "scheduler pump did not finish in time";
        return;
    }

    EXPECT_EQ(done_future.get(), fiber::common::IoErr::None);
    group.join();

    EXPECT_EQ(transport.written(), "PAYLOAD");
    EXPECT_EQ(owner.encode_calls, 1U);
    EXPECT_EQ(scheduler.connection_send_window(), 9);
    EXPECT_EQ(stream.send_window(), 65528);
}

TEST(Http2OutboundSchedulerTest, MovesBlockedDataStreamBackToReadyAfterConnectionWindowUpdate) {
    fiber::event::EventLoopGroup group(1);
    RecordingTransport transport;
    fiber::http::Http2OutboundScheduler scheduler;
    scheduler.set_connection_send_window(0);

    DummyStreamOwner owner;
    owner.first_batch = "DATA";
    owner.block_on_zero_conn_window = true;
    fiber::http::Http2Stream stream(&owner, kStreamOps);
    stream.stream_id_ = 1;

    std::promise<fiber::common::IoErr> done_promise;
    auto done_future = done_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_scheduler_and_stop(&scheduler, &transport, &done_promise, &group); });
    fiber::async::spawn(group.at(0),
                        [&]() { return unblock_conn_window_then_close(&scheduler, &stream, &owner, &transport); });

    if (done_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "scheduler pump did not finish in time";
        return;
    }

    EXPECT_EQ(done_future.get(), fiber::common::IoErr::None);
    group.join();

    EXPECT_GE(owner.encode_calls, 1U);
    EXPECT_EQ(scheduler.waiting_conn_window_stream_count(), 0U);
    EXPECT_GE(scheduler.connection_send_window(), 0);
}

TEST(Http2OutboundSchedulerTest, RequestSendRejectsDuplicateQueuedRequest) {
    fiber::event::EventLoopGroup group(1);
    RecordingTransport transport;
    fiber::http::Http2OutboundScheduler scheduler;
    scheduler.set_connection_send_window(16);

    DummyStreamOwner owner;
    owner.first_batch = "OLD";
    fiber::http::Http2Stream stream(&owner, kStreamOps);
    stream.stream_id_ = 1;

    std::promise<fiber::common::IoErr> done_promise;
    auto done_future = done_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_scheduler_and_stop(&scheduler, &transport, &done_promise, &group); });
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        EXPECT_TRUE(stream.try_arm_outbound(owner));
        EXPECT_EQ(scheduler.request_send(stream, fiber::http::Http2OutboundNextKind::Headers),
                  fiber::common::IoErr::None);
        EXPECT_EQ(scheduler.request_send(stream, fiber::http::Http2OutboundNextKind::Headers),
                  fiber::common::IoErr::Already);
        scheduler.close();
        co_return;
    });

    if (done_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "scheduler pump did not finish in time";
        return;
    }

    EXPECT_EQ(done_future.get(), fiber::common::IoErr::None);
    group.join();

    EXPECT_EQ(transport.written(), "OLD");
    EXPECT_EQ(owner.encode_calls, 1U);
}

TEST(Http2OutboundSchedulerTest, CancelQueuedReadySendRemovesStreamFromQueue) {
    RecordingTransport transport;
    fiber::http::Http2OutboundScheduler scheduler;
    scheduler.set_connection_send_window(16);

    DummyStreamOwner owner;
    owner.first_batch = "HEADERS";
    fiber::http::Http2Stream stream(&owner, kStreamOps);
    stream.stream_id_ = 1;

    EXPECT_TRUE(stream.try_arm_outbound(owner));
    EXPECT_EQ(scheduler.request_send(stream, fiber::http::Http2OutboundNextKind::Headers), fiber::common::IoErr::None);
    EXPECT_EQ(scheduler.ready_stream_count(), 1U);

    EXPECT_TRUE(scheduler.cancel_queued_send(stream));
    EXPECT_EQ(scheduler.ready_stream_count(), 0U);
    EXPECT_FALSE(scheduler.cancel_queued_send(stream));
    EXPECT_EQ(owner.encode_calls, 0U);
}

TEST(Http2OutboundSchedulerTest, CancelQueuedWaitingConnWindowSendRemovesStreamFromQueue) {
    RecordingTransport transport;
    fiber::http::Http2OutboundScheduler scheduler;
    scheduler.set_connection_send_window(0);

    DummyStreamOwner owner;
    owner.first_batch = "DATA";
    fiber::http::Http2Stream stream(&owner, kStreamOps);
    stream.stream_id_ = 1;

    EXPECT_TRUE(stream.try_arm_outbound(owner));
    EXPECT_EQ(scheduler.request_send(stream, fiber::http::Http2OutboundNextKind::Data), fiber::common::IoErr::None);
    EXPECT_EQ(scheduler.waiting_conn_window_stream_count(), 1U);

    EXPECT_TRUE(scheduler.cancel_queued_send(stream));
    EXPECT_EQ(scheduler.waiting_conn_window_stream_count(), 0U);
    EXPECT_FALSE(scheduler.cancel_queued_send(stream));
    EXPECT_EQ(owner.encode_calls, 0U);
}

TEST(Http2OutboundSchedulerTest, PartialWriteCompletesInflightBatch) {
    fiber::event::EventLoopGroup group(1);
    RecordingTransport transport({2, 8});
    fiber::http::Http2OutboundScheduler scheduler;
    scheduler.set_connection_send_window(16);

    DummyStreamOwner owner;
    owner.first_batch = "HEADERS";
    fiber::http::Http2Stream stream(&owner, kStreamOps);
    stream.stream_id_ = 1;

    std::promise<fiber::common::IoErr> done_promise;
    auto done_future = done_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_scheduler_and_stop(&scheduler, &transport, &done_promise, &group); });
    fiber::async::spawn(group.at(0), [&]() {
        return request_stream_send_and_close(&scheduler, &stream, fiber::http::Http2OutboundNextKind::Headers, &owner);
    });

    if (done_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "scheduler pump did not finish in time";
        return;
    }

    EXPECT_EQ(done_future.get(), fiber::common::IoErr::None);
    group.join();

    EXPECT_EQ(transport.written(), "HEADERS");
    EXPECT_EQ(owner.encode_calls, 1U);
    EXPECT_TRUE(scheduler.idle());
}

TEST(Http2OutboundSchedulerTest, WriteFailureStopsScheduler) {
    fiber::event::EventLoopGroup group(1);
    RecordingTransport transport({}, fiber::common::IoErr::ConnReset);
    fiber::http::Http2OutboundScheduler scheduler;
    scheduler.set_connection_send_window(16);

    DummyStreamOwner owner;
    owner.first_batch = "HEADERS";
    fiber::http::Http2Stream stream(&owner, kStreamOps);
    stream.stream_id_ = 1;

    std::promise<fiber::common::IoErr> done_promise;
    auto done_future = done_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_scheduler_and_stop(&scheduler, &transport, &done_promise, &group); });
    fiber::async::spawn(group.at(0), [&]() {
        return request_stream_send_and_close(&scheduler, &stream, fiber::http::Http2OutboundNextKind::Headers, &owner);
    });

    if (done_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "scheduler pump did not finish in time";
        return;
    }

    EXPECT_EQ(done_future.get(), fiber::common::IoErr::ConnReset);
    group.join();

    EXPECT_EQ(owner.encode_calls, 1U);
    EXPECT_TRUE(scheduler.stopped());
}
