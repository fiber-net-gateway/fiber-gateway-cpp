#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <future>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "async/Spawn.h"
#include "async/Sleep.h"
#include "event/EventLoopGroup.h"
#include "http/Http2OutboundScheduler.h"
#include "http/HttpTransport.h"

namespace {

using fiber::async::DetachedTask;

class RecordingTransport final : public fiber::http::HttpTransport {
public:
    explicit RecordingTransport(std::vector<std::size_t> write_steps = {},
                                fiber::common::IoErr write_error = fiber::common::IoErr::None) :
        write_steps_(std::move(write_steps)),
        write_error_(write_error) {}

    fiber::async::Task<fiber::common::IoResult<void>> handshake(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<void>> shutdown(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
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
    [[nodiscard]] std::string negotiated_alpn() const noexcept override { return "h2"; }
    [[nodiscard]] const fiber::net::SocketAddress &remote_addr() const noexcept override { return remote_addr_; }
    [[nodiscard]] const std::string &written() const noexcept { return written_; }

private:
    std::vector<std::size_t> write_steps_;
    std::size_t write_call_count_ = 0;
    bool closed_ = false;
    std::string written_;
    fiber::common::IoErr write_error_ = fiber::common::IoErr::None;
    fiber::net::SocketAddress remote_addr_{};
};

struct DummyStreamOwner {
    std::string first_batch;
    std::string second_batch;
    bool block_on_zero_conn_window = false;
    bool close_after_first_batch = false;
    bool notify_done = false;
    std::size_t first_slot_available = 0;
    std::size_t encode_calls = 0;
    std::size_t done_calls = 0;
    std::optional<fiber::common::IoErr> done_result;
};

void on_destroy(void *) noexcept {}

fiber::common::IoErr on_header_block_start(void *, fiber::http::Http2HpackDecoder::Sink &) noexcept {
    return fiber::common::IoErr::None;
}

fiber::common::IoErr on_header_block_complete(void *, bool) noexcept {
    return fiber::common::IoErr::None;
}

fiber::common::IoErr on_body(void *, fiber::mem::IoBuf &&, bool) noexcept {
    return fiber::common::IoErr::None;
}

void on_abort(void *, fiber::common::IoErr) noexcept {}

const fiber::http::Http2Stream::Ops kStreamOps{
    &on_destroy,
    &on_header_block_start,
    &on_header_block_complete,
    &on_body,
    &on_abort,
    nullptr,
};

void on_stream_batch_done(void *ctx, fiber::common::IoErr result) noexcept {
    auto *owner = static_cast<DummyStreamOwner *>(ctx);
    if (!owner) {
        return;
    }
    ++owner->done_calls;
    owner->done_result = result;
}

fiber::common::IoErr encode_stream_batch(fiber::http::Http2Stream &, void *ctx,
                                         const fiber::http::Http2OutboundEncodeRequest &req,
                                         fiber::http::Http2OutboundEncodeTarget &target,
                                         fiber::http::Http2OutboundEncodeResult &result) noexcept {
    auto *owner = static_cast<DummyStreamOwner *>(ctx);
    if (!owner) {
        return fiber::common::IoErr::Invalid;
    }

    ++owner->encode_calls;
    if (owner->encode_calls == 1) {
        owner->first_slot_available = target.slot_available();
        if (owner->block_on_zero_conn_window && req.conn_window_budget <= 0) {
            result.status = fiber::http::Http2OutboundEncodeResult::Status::BlockedConnWindow;
            result.next_kind = fiber::http::Http2OutboundNextKind::Data;
            return fiber::common::IoErr::None;
        }
        if (!owner->first_batch.empty()) {
            std::uint8_t *dst = nullptr;
            if (owner->first_batch.size() <= target.slot_available()) {
                fiber::common::IoErr err = target.reserve_slot(owner->first_batch.size(), dst);
                if (err != fiber::common::IoErr::None) {
                    return err;
                }
                std::memcpy(dst, owner->first_batch.data(), owner->first_batch.size());
                target.commit_slot(owner->first_batch.size());
            } else {
                fiber::common::IoErr err = target.append_copy(owner->first_batch.data(), owner->first_batch.size());
                if (err != fiber::common::IoErr::None) {
                    return err;
                }
            }
        }
        if (owner->notify_done) {
            target.set_on_done(&on_stream_batch_done, owner);
        }
        result.status = owner->close_after_first_batch ? fiber::http::Http2OutboundEncodeResult::Status::Closed
                                                       : fiber::http::Http2OutboundEncodeResult::Status::Encoded;
        result.next_kind = owner->second_batch.empty() ? fiber::http::Http2OutboundNextKind::None
                                                       : fiber::http::Http2OutboundNextKind::Data;
        result.consumed_conn_window = static_cast<std::uint32_t>(owner->first_batch.size());
        return fiber::common::IoErr::None;
    }

    if (owner->encode_calls == 2 && !owner->second_batch.empty()) {
        fiber::common::IoErr err = target.append_copy(owner->second_batch.data(), owner->second_batch.size());
        if (err != fiber::common::IoErr::None) {
            return err;
        }
        result.status = fiber::http::Http2OutboundEncodeResult::Status::Encoded;
        result.next_kind = fiber::http::Http2OutboundNextKind::None;
        result.consumed_conn_window = static_cast<std::uint32_t>(owner->second_batch.size());
        return fiber::common::IoErr::None;
    }

    result.status = fiber::http::Http2OutboundEncodeResult::Status::NoWork;
    result.next_kind = fiber::http::Http2OutboundNextKind::None;
    return fiber::common::IoErr::None;
}

DetachedTask run_scheduler_and_stop(fiber::http::Http2OutboundScheduler *scheduler,
                                    std::promise<fiber::common::IoErr> *promise,
                                    fiber::event::EventLoopGroup *group) {
    co_await scheduler->send_loop();
    promise->set_value(scheduler->stop_reason());
    group->stop();
    co_return;
}

DetachedTask enqueue_control_and_close(fiber::http::Http2OutboundScheduler *scheduler,
                                       std::vector<std::string> frames) {
    for (const std::string &frame : frames) {
        EXPECT_EQ(scheduler->alloc_and_enqueue_control(frame.size(), [&](std::uint8_t *dst) noexcept {
            std::memcpy(dst, frame.data(), frame.size());
        }), fiber::common::IoErr::None);
    }
    scheduler->close();
    co_return;
}

DetachedTask request_stream_send_and_close(fiber::http::Http2OutboundScheduler *scheduler,
                                           fiber::http::Http2Stream *stream,
                                           fiber::http::Http2OutboundNextKind kind,
                                           DummyStreamOwner *owner) {
    EXPECT_EQ(scheduler->request_send(*stream, kind, &encode_stream_batch, owner), fiber::common::IoErr::None);
    scheduler->close();
    co_return;
}

DetachedTask unblock_conn_window_then_close(fiber::http::Http2OutboundScheduler *scheduler,
                                            fiber::http::Http2Stream *stream,
                                            DummyStreamOwner *owner,
                                            RecordingTransport *transport) {
    EXPECT_EQ(scheduler->request_send(*stream, fiber::http::Http2OutboundNextKind::Data, &encode_stream_batch, owner),
              fiber::common::IoErr::None);
    while (scheduler->waiting_conn_window_stream_count() == 0) {
        co_await fiber::async::sleep(std::chrono::milliseconds(1));
    }
    scheduler->set_connection_send_window(4);
    scheduler->on_connection_window_available();
    co_await fiber::async::sleep(std::chrono::milliseconds(5));
    (void)transport;
    scheduler->close();
    co_return;
}

} // namespace

TEST(Http2OutboundSchedulerTest, DrainsQueuedControlBytesAndKeepsOneCachedSlab) {
    fiber::event::EventLoopGroup group(1);
    RecordingTransport transport({3, 2, 16});
    fiber::http::Http2OutboundScheduler scheduler(&transport, 8, std::chrono::milliseconds(50));

    std::promise<fiber::common::IoErr> done_promise;
    auto done_future = done_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() {
        return run_scheduler_and_stop(&scheduler, &done_promise, &group);
    });
    fiber::async::spawn(group.at(0), [&]() {
        return enqueue_control_and_close(&scheduler, {"PING", "ACK!"});
    });

    if (done_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "scheduler send_loop did not finish in time";
        return;
    }

    EXPECT_EQ(done_future.get(), fiber::common::IoErr::None);
    group.join();

    EXPECT_EQ(transport.written(), "PINGACK!");
    EXPECT_TRUE(scheduler.idle());
    EXPECT_EQ(scheduler.active_slab_count(), 0U);
    EXPECT_TRUE(scheduler.has_cached_slab());
}

TEST(Http2OutboundSchedulerTest, EncodesAndSendsStreamBatchBeforeClosing) {
    fiber::event::EventLoopGroup group(1);
    RecordingTransport transport({2, 8});
    fiber::http::Http2OutboundScheduler scheduler(&transport, 8, std::chrono::milliseconds(50));
    scheduler.set_connection_send_window(16);

    DummyStreamOwner owner{
        .first_batch = "HEADERS",
        .second_batch = "",
    };
    fiber::http::Http2Stream stream(1, &owner, kStreamOps);

    std::promise<fiber::common::IoErr> done_promise;
    auto done_future = done_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() {
        return run_scheduler_and_stop(&scheduler, &done_promise, &group);
    });
    fiber::async::spawn(group.at(0), [&]() {
        return request_stream_send_and_close(&scheduler, &stream, fiber::http::Http2OutboundNextKind::Headers, &owner);
    });

    if (done_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "scheduler send_loop did not finish in time";
        return;
    }

    EXPECT_EQ(done_future.get(), fiber::common::IoErr::None);
    group.join();

    EXPECT_EQ(transport.written(), "HEADERS");
    EXPECT_EQ(owner.encode_calls, 1U);
    EXPECT_EQ(owner.first_slot_available, fiber::http::Http2OutboundScheduler::kPrimarySlabCapacity);
    EXPECT_EQ(scheduler.connection_send_window(), 9);
}

TEST(Http2OutboundSchedulerTest, MovesBlockedDataStreamBackToReadyAfterConnectionWindowUpdate) {
    fiber::event::EventLoopGroup group(1);
    RecordingTransport transport;
    fiber::http::Http2OutboundScheduler scheduler(&transport, 8, std::chrono::milliseconds(50));
    scheduler.set_connection_send_window(0);

    DummyStreamOwner owner{
        .first_batch = "DATA",
        .second_batch = "",
        .block_on_zero_conn_window = true,
    };
    fiber::http::Http2Stream stream(1, &owner, kStreamOps);

    std::promise<fiber::common::IoErr> done_promise;
    auto done_future = done_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() {
        return run_scheduler_and_stop(&scheduler, &done_promise, &group);
    });
    fiber::async::spawn(group.at(0), [&]() {
        return unblock_conn_window_then_close(&scheduler, &stream, &owner, &transport);
    });

    if (done_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "scheduler send_loop did not finish in time";
        return;
    }

    EXPECT_EQ(done_future.get(), fiber::common::IoErr::None);
    group.join();

    EXPECT_GE(owner.encode_calls, 1U);
    EXPECT_EQ(scheduler.waiting_conn_window_stream_count(), 0U);
    EXPECT_GE(scheduler.connection_send_window(), 0);
}

TEST(Http2OutboundSchedulerTest, RequestSendUpdatesQueuedStreamEncoder) {
    fiber::event::EventLoopGroup group(1);
    RecordingTransport transport;
    fiber::http::Http2OutboundScheduler scheduler(&transport, 8, std::chrono::milliseconds(50));
    scheduler.set_connection_send_window(16);

    DummyStreamOwner first{
        .first_batch = "OLD",
    };
    DummyStreamOwner second{
        .first_batch = "NEW",
    };
    fiber::http::Http2Stream stream(1, &first, kStreamOps);

    std::promise<fiber::common::IoErr> done_promise;
    auto done_future = done_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() {
        return run_scheduler_and_stop(&scheduler, &done_promise, &group);
    });
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        EXPECT_EQ(scheduler.request_send(stream, fiber::http::Http2OutboundNextKind::Headers, &encode_stream_batch, &first),
                  fiber::common::IoErr::None);
        EXPECT_EQ(scheduler.request_send(stream, fiber::http::Http2OutboundNextKind::Headers, &encode_stream_batch, &second),
                  fiber::common::IoErr::None);
        scheduler.close();
        co_return;
    });

    if (done_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "scheduler send_loop did not finish in time";
        return;
    }

    EXPECT_EQ(done_future.get(), fiber::common::IoErr::None);
    group.join();

    EXPECT_EQ(transport.written(), "NEW");
    EXPECT_EQ(first.encode_calls, 0U);
    EXPECT_EQ(second.encode_calls, 1U);
}

TEST(Http2OutboundSchedulerTest, CancelQueuedReadySendRemovesStreamFromQueue) {
    RecordingTransport transport;
    fiber::http::Http2OutboundScheduler scheduler(&transport, 8, std::chrono::milliseconds(50));
    scheduler.set_connection_send_window(16);

    DummyStreamOwner owner{
        .first_batch = "HEADERS",
    };
    fiber::http::Http2Stream stream(1, &owner, kStreamOps);

    EXPECT_EQ(scheduler.request_send(stream, fiber::http::Http2OutboundNextKind::Headers, &encode_stream_batch, &owner),
              fiber::common::IoErr::None);
    EXPECT_EQ(scheduler.ready_stream_count(), 1U);

    EXPECT_TRUE(scheduler.cancel_queued_send(stream));
    EXPECT_EQ(scheduler.ready_stream_count(), 0U);
    EXPECT_FALSE(scheduler.cancel_queued_send(stream));
    EXPECT_EQ(owner.encode_calls, 0U);
}

TEST(Http2OutboundSchedulerTest, CancelQueuedWaitingConnWindowSendRemovesStreamFromQueue) {
    RecordingTransport transport;
    fiber::http::Http2OutboundScheduler scheduler(&transport, 8, std::chrono::milliseconds(50));
    scheduler.set_connection_send_window(0);

    DummyStreamOwner owner{
        .first_batch = "DATA",
    };
    fiber::http::Http2Stream stream(1, &owner, kStreamOps);

    EXPECT_EQ(scheduler.request_send(stream, fiber::http::Http2OutboundNextKind::Data, &encode_stream_batch, &owner),
              fiber::common::IoErr::None);
    EXPECT_EQ(scheduler.waiting_conn_window_stream_count(), 1U);

    EXPECT_TRUE(scheduler.cancel_queued_send(stream));
    EXPECT_EQ(scheduler.waiting_conn_window_stream_count(), 0U);
    EXPECT_FALSE(scheduler.cancel_queued_send(stream));
    EXPECT_EQ(owner.encode_calls, 0U);
}

TEST(Http2OutboundSchedulerTest, OnDoneFiresAfterInflightBatchCompletes) {
    fiber::event::EventLoopGroup group(1);
    RecordingTransport transport({2, 8});
    fiber::http::Http2OutboundScheduler scheduler(&transport, 8, std::chrono::milliseconds(50));
    scheduler.set_connection_send_window(16);

    DummyStreamOwner owner{
        .first_batch = "HEADERS",
        .notify_done = true,
    };
    fiber::http::Http2Stream stream(1, &owner, kStreamOps);

    std::promise<fiber::common::IoErr> done_promise;
    auto done_future = done_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() {
        return run_scheduler_and_stop(&scheduler, &done_promise, &group);
    });
    fiber::async::spawn(group.at(0), [&]() {
        return request_stream_send_and_close(&scheduler, &stream, fiber::http::Http2OutboundNextKind::Headers, &owner);
    });

    if (done_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "scheduler send_loop did not finish in time";
        return;
    }

    EXPECT_EQ(done_future.get(), fiber::common::IoErr::None);
    group.join();

    ASSERT_TRUE(owner.done_result.has_value());
    EXPECT_EQ(*owner.done_result, fiber::common::IoErr::None);
    EXPECT_EQ(owner.done_calls, 1U);
}

TEST(Http2OutboundSchedulerTest, OnDoneFiresWithFailureWhenWriteFails) {
    fiber::event::EventLoopGroup group(1);
    RecordingTransport transport({}, fiber::common::IoErr::ConnReset);
    fiber::http::Http2OutboundScheduler scheduler(&transport, 8, std::chrono::milliseconds(50));
    scheduler.set_connection_send_window(16);

    DummyStreamOwner owner{
        .first_batch = "HEADERS",
        .notify_done = true,
    };
    fiber::http::Http2Stream stream(1, &owner, kStreamOps);

    std::promise<fiber::common::IoErr> done_promise;
    auto done_future = done_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() {
        return run_scheduler_and_stop(&scheduler, &done_promise, &group);
    });
    fiber::async::spawn(group.at(0), [&]() {
        return request_stream_send_and_close(&scheduler, &stream, fiber::http::Http2OutboundNextKind::Headers, &owner);
    });

    if (done_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "scheduler send_loop did not finish in time";
        return;
    }

    EXPECT_EQ(done_future.get(), fiber::common::IoErr::ConnReset);
    group.join();

    ASSERT_TRUE(owner.done_result.has_value());
    EXPECT_EQ(*owner.done_result, fiber::common::IoErr::ConnReset);
    EXPECT_EQ(owner.done_calls, 1U);
}
