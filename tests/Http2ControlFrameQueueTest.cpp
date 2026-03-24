#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <future>
#include <string>
#include <utility>
#include <vector>

#include "async/Spawn.h"
#include "event/EventLoopGroup.h"
#include "http/Http2ControlFrameQueue.h"
#include "http/HttpTransport.h"

namespace {

using fiber::async::DetachedTask;

class RecordingTransport final : public fiber::http::HttpTransport {
public:
    explicit RecordingTransport(std::vector<std::size_t> write_steps = {}) : write_steps_(std::move(write_steps)) {}

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

    fiber::async::Task<fiber::common::IoResult<size_t>> writev(fiber::mem::IoBufChain &,
                                                               std::chrono::milliseconds) override {
        co_return std::unexpected(fiber::common::IoErr::NotSupported);
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
    fiber::net::SocketAddress remote_addr_{};
};

DetachedTask run_queue_and_stop(fiber::http::Http2ControlFrameQueue *queue,
                                std::promise<fiber::common::IoErr> *promise,
                                fiber::event::EventLoopGroup *group) {
    co_await queue->send_loop();
    promise->set_value(queue->stop_reason());
    group->stop();
    co_return;
}

DetachedTask enqueue_frames_and_close(fiber::http::Http2ControlFrameQueue *queue,
                                      std::vector<std::string> frames) {
    for (const std::string &frame : frames) {
        EXPECT_EQ(queue->alloc_and_enqueue(frame.size(), [&](std::uint8_t *dst) noexcept {
            std::memcpy(dst, frame.data(), frame.size());
        }), fiber::common::IoErr::None);
    }
    queue->close();
    co_return;
}

} // namespace

TEST(Http2ControlFrameQueueTest, DrainsQueuedBytesAndKeepsOneCachedSlab) {
    fiber::event::EventLoopGroup group(1);
    RecordingTransport transport({3, 2, 16});
    fiber::http::Http2ControlFrameQueue queue(&transport, 8, std::chrono::milliseconds(50));

    std::promise<fiber::common::IoErr> done_promise;
    auto done_future = done_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() {
        return run_queue_and_stop(&queue, &done_promise, &group);
    });
    fiber::async::spawn(group.at(0), [&]() {
        return enqueue_frames_and_close(&queue, {"PING", "ACK!"});
    });

    if (done_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "queue send_loop did not finish in time";
        return;
    }

    EXPECT_EQ(done_future.get(), fiber::common::IoErr::None);
    group.join();

    EXPECT_EQ(transport.written(), "PINGACK!");
    EXPECT_TRUE(queue.idle());
    EXPECT_EQ(queue.active_slab_count(), 0U);
    EXPECT_TRUE(queue.has_cached_slab());
}

TEST(Http2ControlFrameQueueTest, AllocatesAdditionalSlabWhenTailHasNoSpace) {
    fiber::event::EventLoopGroup group(1);
    RecordingTransport transport;
    fiber::http::Http2ControlFrameQueue queue(&transport, 8, std::chrono::milliseconds(50));

    std::promise<fiber::common::IoErr> done_promise;
    auto done_future = done_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() {
        return run_queue_and_stop(&queue, &done_promise, &group);
    });
    fiber::async::spawn(group.at(0), [&]() {
        return enqueue_frames_and_close(&queue, {"ABCDEF", "GHIJKL"});
    });

    if (done_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "queue send_loop did not finish in time";
        return;
    }

    EXPECT_EQ(done_future.get(), fiber::common::IoErr::None);
    group.join();

    EXPECT_EQ(transport.written(), "ABCDEFGHIJKL");
    EXPECT_TRUE(queue.idle());
    EXPECT_TRUE(queue.has_cached_slab());
}

TEST(Http2ControlFrameQueueTest, RejectsOversizedReservation) {
    fiber::http::Http2ControlFrameQueue queue(nullptr, 8);

    EXPECT_EQ(queue.alloc_and_enqueue(9, [](std::uint8_t *) noexcept {}), fiber::common::IoErr::Invalid);
    EXPECT_TRUE(queue.idle());
}
