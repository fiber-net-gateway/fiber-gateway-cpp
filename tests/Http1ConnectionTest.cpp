#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/common/mem/IoBuf.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/Http1Connection.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpTransport.h>
#include "HttpTransportStub.h"

namespace {

using namespace std::chrono_literals;

enum class TransportEvent {
    WaitReadable,
    Read,
    Handler,
    Write,
    Shutdown,
    Close,
};

struct TransportMetrics {
    std::vector<TransportEvent> events;
    std::vector<std::chrono::milliseconds> wait_timeouts;
    std::vector<std::chrono::milliseconds> read_timeouts;
    std::size_t handler_calls = 0;
    std::size_t write_calls = 0;
    std::size_t body_remaining = 0;
    std::vector<std::size_t> partial_write_results;
    std::string written;
    bool body_complete = false;
    bool responses_ok = true;
};

class RecordingHttp1Transport final : public fiber::test::HttpTransportStub {
public:
    RecordingHttp1Transport(fiber::event::EventLoop &loop, TransportMetrics &metrics, std::string input,
                            std::size_t write_limit = std::numeric_limits<std::size_t>::max()) :
        loop_(loop), metrics_(metrics), input_(std::move(input)), write_limit_(write_limit) {}

    fiber::async::Task<fiber::common::IoResult<void>> handshake(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<void>> shutdown(std::chrono::milliseconds) override {
        metrics_.events.push_back(TransportEvent::Shutdown);
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<void>> wait_readable(std::chrono::milliseconds timeout) override {
        metrics_.events.push_back(TransportEvent::WaitReadable);
        metrics_.wait_timeouts.push_back(timeout);
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> read(void *, std::size_t,
                                                                  std::chrono::milliseconds) override {
        co_return static_cast<std::size_t>(0);
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> read_into(fiber::mem::IoBuf &buf,
                                                                       std::chrono::milliseconds timeout) override {
        metrics_.events.push_back(TransportEvent::Read);
        metrics_.read_timeouts.push_back(timeout);
        if (input_consumed_) {
            co_return static_cast<std::size_t>(0);
        }
        if (buf.writable() < input_.size()) {
            co_return std::unexpected(fiber::common::IoErr::MessageTooLarge);
        }
        std::memcpy(buf.writable_data(), input_.data(), input_.size());
        buf.commit(input_.size());
        input_consumed_ = true;
        co_return input_.size();
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> readv_into(fiber::mem::IoBufChain &,
                                                                        std::chrono::milliseconds) override {
        co_return std::unexpected(fiber::common::IoErr::NotSupported);
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> write(const void *buf, std::size_t len,
                                                                   std::chrono::milliseconds) override {
        const std::size_t written = std::min(len, write_limit_);
        metrics_.written.append(static_cast<const char *>(buf), written);
        record_write();
        co_return written;
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> write(fiber::mem::IoBuf &buf,
                                                                   std::chrono::milliseconds) override {
        const std::size_t len = std::min(buf.readable(), write_limit_);
        metrics_.written.append(reinterpret_cast<const char *>(buf.readable_data()), len);
        buf.consume(len);
        record_write();
        co_return len;
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> writev(fiber::mem::IoBufChain &buf,
                                                                    std::chrono::milliseconds) override {
        if (writev_error_ != fiber::common::IoErr::None) {
            if (writev_successes_before_error_ == 0) {
                record_write();
                co_return std::unexpected(writev_error_);
            }
            --writev_successes_before_error_;
        }
        const std::size_t len = std::min(buf.readable_bytes(), write_limit_);
        std::array<iovec, 16> iov{};
        const int count = buf.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
        std::size_t remaining = len;
        for (int i = 0; i < count && remaining != 0; ++i) {
            const std::size_t take = std::min(remaining, iov[static_cast<std::size_t>(i)].iov_len);
            metrics_.written.append(static_cast<const char *>(iov[static_cast<std::size_t>(i)].iov_base), take);
            remaining -= take;
        }
        buf.consume_and_compact(len);
        record_write();
        co_return len;
    }

    void close() override {
        if (closed_) {
            return;
        }
        closed_ = true;
        metrics_.events.push_back(TransportEvent::Close);
    }

    [[nodiscard]] bool valid() const noexcept override { return !closed_; }
    [[nodiscard]] int fd() const noexcept override { return -1; }
    [[nodiscard]] std::string_view negotiated_alpn() const noexcept override { return {}; }
    [[nodiscard]] const fiber::net::SocketAddress &remote_addr() const noexcept override { return remote_addr_; }
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept override { return loop_; }

    void simulate_terminal(fiber::common::IoErr err = fiber::common::IoErr::ConnReset) noexcept {
        notify_terminal(err);
    }
    [[nodiscard]] bool response_wait_registered() const noexcept { return terminal_callback_registered(); }
    void fail_writev_after(std::size_t successful_writes, fiber::common::IoErr error) noexcept {
        writev_successes_before_error_ = successful_writes;
        writev_error_ = error;
    }

private:
    void record_write() {
        metrics_.events.push_back(TransportEvent::Write);
        ++metrics_.write_calls;
    }

    fiber::event::EventLoop &loop_;
    TransportMetrics &metrics_;
    std::string input_;
    fiber::net::SocketAddress remote_addr_{};
    std::size_t write_limit_ = std::numeric_limits<std::size_t>::max();
    std::size_t writev_successes_before_error_ = 0;
    fiber::common::IoErr writev_error_ = fiber::common::IoErr::None;
    bool input_consumed_ = false;
    bool closed_ = false;
};

fiber::async::DetachedTask run_pipelined_http1_connection(fiber::event::EventLoop *loop, TransportMetrics *metrics,
                                                          std::promise<void> *done) {
    std::string requests = "GET /one HTTP/1.1\r\nHost: example.test\r\n\r\n"
                           "GET /two HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n";
    auto transport = std::make_unique<RecordingHttp1Transport>(*loop, *metrics, std::move(requests));

    fiber::http::HttpServerOptions options;
    options.keep_alive_timeout = 2s;
    options.header_timeout = 1s;

    fiber::http::HttpHandler handler = [metrics](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        metrics->events.push_back(TransportEvent::Handler);
        ++metrics->handler_calls;
        auto result = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 204,
                .body = fiber::http::ResponseBodySpec::None(),
                .end_stream = true,
        });
        metrics->responses_ok = metrics->responses_ok && result.has_value();
        co_return;
    };

    fiber::http::Http1Connection connection(nullptr, std::move(transport), std::move(handler), options);
    co_await connection.run();
    done->set_value();
    co_return;
}

TEST(Http1ConnectionTest, WaitsBeforeFirstReadUsesHeaderTimeoutAndSkipsWaitForPipelinedRequest) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    TransportMetrics metrics;
    std::promise<void> done_promise;
    auto done_future = done_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_pipelined_http1_connection(&group.at(0), &metrics, &done_promise); });

    ASSERT_EQ(done_future.wait_for(2s), std::future_status::ready);
    group.stop();
    group.join();

    ASSERT_EQ(metrics.wait_timeouts.size(), 1U);
    EXPECT_EQ(metrics.wait_timeouts.front(), 2s);
    ASSERT_EQ(metrics.read_timeouts.size(), 1U);
    EXPECT_EQ(metrics.read_timeouts.front(), 1s);
    EXPECT_EQ(metrics.handler_calls, 2U);
    EXPECT_EQ(metrics.write_calls, 2U);
    EXPECT_TRUE(metrics.responses_ok);

    auto wait_pos = std::find(metrics.events.begin(), metrics.events.end(), TransportEvent::WaitReadable);
    auto read_pos = std::find(metrics.events.begin(), metrics.events.end(), TransportEvent::Read);
    ASSERT_NE(wait_pos, metrics.events.end());
    ASSERT_NE(read_pos, metrics.events.end());
    EXPECT_LT(wait_pos, read_pos);
}

TEST(Http1ConnectionTest, ResponseChannelWaitUsesTransportTerminalCallback) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    auto wait_result = std::make_shared<fiber::common::IoResult<void>>(std::unexpected(fiber::common::IoErr::Invalid));
    auto initial_closed = std::make_shared<bool>(true);
    auto final_closed = std::make_shared<bool>(false);
    std::promise<void> done_promise;
    auto done_future = done_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        TransportMetrics metrics;
        auto transport = std::make_unique<RecordingHttp1Transport>(
                group.at(0), metrics, "GET /wait HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n");
        RecordingHttp1Transport *transport_ptr = transport.get();

        fiber::http::HttpHandler handler = [wait_result, initial_closed, final_closed](
                                                   fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            *initial_closed = exchange.response_channel_closed();
            *wait_result = co_await exchange.wait_response_channel_closed();
            *final_closed = exchange.response_channel_closed();
            co_return;
        };

        fiber::http::Http1Connection connection(nullptr, std::move(transport), std::move(handler), {});
        fiber::async::spawn([transport_ptr]() -> fiber::async::DetachedTask {
            while (!transport_ptr->response_wait_registered()) {
                co_await fiber::async::sleep(1ms);
            }
            transport_ptr->simulate_terminal();
        });
        co_await connection.run();
        done_promise.set_value();
        co_return;
    });

    ASSERT_EQ(done_future.wait_for(2s), std::future_status::ready);
    group.stop();
    group.join();

    EXPECT_FALSE(*initial_closed);
    EXPECT_TRUE(wait_result->has_value());
    EXPECT_TRUE(*final_closed);
}

TEST(Http1ConnectionTest, ChunkedWriteReturnsAfterPayloadProgressAndPreservesCallerTail) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    TransportMetrics metrics;
    std::promise<void> done_promise;
    auto done_future = done_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        auto transport = std::make_unique<RecordingHttp1Transport>(
                group.at(0), metrics, "GET /partial HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n", 2);

        fiber::http::HttpHandler handler = [&metrics](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            auto header = co_await exchange.send_header({
                    .kind = fiber::http::OutgoingHeaderKind::Final,
                    .status_code = 200,
                    .body = fiber::http::HttpBodySpec::Chunked(),
                    .end_stream = false,
            });
            if (!header) {
                metrics.responses_ok = false;
                co_return;
            }

            fiber::mem::IoBufChain chunk(fiber::event::EventLoop::current().io_buf_node_pool());
            fiber::mem::IoBuf body = fiber::mem::IoBuf::allocate(5);
            if (!body) {
                metrics.responses_ok = false;
                co_return;
            }
            std::memcpy(body.writable_data(), "hello", 5);
            body.commit(5);
            if (!chunk.append(std::move(body))) {
                metrics.responses_ok = false;
                co_return;
            }
            chunk.mark_complete();

            while (chunk.readable_bytes() != 0 || chunk.complete()) {
                auto written = co_await exchange.write(chunk);
                if (!written) {
                    metrics.responses_ok = false;
                    co_return;
                }
                metrics.partial_write_results.push_back(*written);
            }
            metrics.body_remaining = chunk.readable_bytes();
            metrics.body_complete = chunk.complete();
            co_return;
        };

        fiber::http::Http1Connection connection(nullptr, std::move(transport), std::move(handler), {});
        co_await connection.run();
        done_promise.set_value();
        co_return;
    });

    ASSERT_EQ(done_future.wait_for(2s), std::future_status::ready);
    group.stop();
    group.join();

    EXPECT_TRUE(metrics.responses_ok);
    EXPECT_EQ(metrics.partial_write_results, (std::vector<std::size_t>{1, 2, 2}));
    EXPECT_EQ(metrics.body_remaining, 0U);
    EXPECT_FALSE(metrics.body_complete);
    EXPECT_TRUE(metrics.written.ends_with("5\r\nhello\r\n0\r\n\r\n"));
}

TEST(Http1ConnectionTest, ChunkedWriteTimeoutAbortsResponseAndRejectsRetry) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    TransportMetrics metrics;
    auto first_result =
            std::make_shared<fiber::common::IoResult<std::size_t>>(std::unexpected(fiber::common::IoErr::Invalid));
    auto retry_result =
            std::make_shared<fiber::common::IoResult<std::size_t>>(std::unexpected(fiber::common::IoErr::Invalid));
    std::size_t writes_after_header = 0;
    std::size_t writes_after_timeout = 0;
    std::size_t writes_after_retry = 0;
    fiber::common::IoErr terminal_error = fiber::common::IoErr::None;
    std::promise<void> done_promise;
    auto done_future = done_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        auto transport = std::make_unique<RecordingHttp1Transport>(
                group.at(0), metrics, "GET /timeout HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n", 1);
        RecordingHttp1Transport *transport_ptr = transport.get();

        fiber::http::HttpHandler handler = [&](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            auto header = co_await exchange.send_header({
                    .kind = fiber::http::OutgoingHeaderKind::Final,
                    .status_code = 200,
                    .body = fiber::http::HttpBodySpec::Chunked(),
                    .end_stream = false,
            });
            if (!header) {
                co_return;
            }
            writes_after_header = metrics.write_calls;
            transport_ptr->fail_writev_after(1, fiber::common::IoErr::TimedOut);

            fiber::mem::IoBufChain chunk(fiber::event::EventLoop::current().io_buf_node_pool());
            fiber::mem::IoBuf body = fiber::mem::IoBuf::allocate(5);
            if (!body) {
                co_return;
            }
            std::memcpy(body.writable_data(), "hello", 5);
            body.commit(5);
            if (!chunk.append(std::move(body))) {
                co_return;
            }
            chunk.mark_complete();

            *first_result = co_await exchange.write(chunk, 1ms);
            writes_after_timeout = metrics.write_calls;
            *retry_result = co_await exchange.write(chunk, 1ms);
            writes_after_retry = metrics.write_calls;
            terminal_error = exchange.response_stats().terminal_error;
            co_return;
        };

        fiber::http::Http1Connection connection(nullptr, std::move(transport), std::move(handler), {});
        co_await connection.run();
        done_promise.set_value();
        co_return;
    });

    ASSERT_EQ(done_future.wait_for(2s), std::future_status::ready);
    group.stop();
    group.join();

    ASSERT_FALSE(first_result->has_value());
    EXPECT_EQ(first_result->error(), fiber::common::IoErr::TimedOut);
    ASSERT_FALSE(retry_result->has_value());
    EXPECT_EQ(retry_result->error(), fiber::common::IoErr::TimedOut);
    EXPECT_EQ(writes_after_timeout, writes_after_header + 2);
    EXPECT_EQ(writes_after_retry, writes_after_timeout);
    EXPECT_EQ(terminal_error, fiber::common::IoErr::TimedOut);
    EXPECT_EQ(std::count(metrics.events.begin(), metrics.events.end(), TransportEvent::Close), 1);
}

} // namespace
