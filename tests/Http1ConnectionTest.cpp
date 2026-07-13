#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "async/Spawn.h"
#include "async/Task.h"
#include "common/IoError.h"
#include "common/mem/IoBuf.h"
#include "common/mem/IoBufChain.h"
#include "event/EventLoopGroup.h"
#include "http/Http1Connection.h"
#include "http/HttpExchange.h"
#include "http/HttpTransport.h"

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
    bool responses_ok = true;
};

class RecordingHttp1Transport final : public fiber::http::HttpTransport {
public:
    RecordingHttp1Transport(fiber::event::EventLoop &loop, TransportMetrics &metrics, std::string input) :
        loop_(loop), metrics_(metrics), input_(std::move(input)) {}

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

    fiber::async::Task<fiber::common::IoResult<std::size_t>> write(const void *, std::size_t len,
                                                                   std::chrono::milliseconds) override {
        record_write();
        co_return len;
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> write(fiber::mem::IoBuf &buf,
                                                                   std::chrono::milliseconds) override {
        std::size_t len = buf.readable();
        buf.consume(len);
        record_write();
        co_return len;
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> writev(fiber::mem::IoBufChain &buf,
                                                                    std::chrono::milliseconds) override {
        std::size_t len = buf.readable_bytes();
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

private:
    void record_write() {
        metrics_.events.push_back(TransportEvent::Write);
        ++metrics_.write_calls;
    }

    fiber::event::EventLoop &loop_;
    TransportMetrics &metrics_;
    std::string input_;
    fiber::net::SocketAddress remote_addr_{};
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

} // namespace
