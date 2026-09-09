#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <string_view>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/ServerRequestFactory.h>

// Inspect cancellation and invoke the expiry boundary in a chosen order;
// normal stream transitions below are driven by actual protocol frames.
#define private public
#include <fiber/http/Http2ServerConnection.h>
#undef private

#include "HttpTransportStub.h"

namespace {
using namespace fiber;
using namespace std::chrono_literals;
using State = http::Http2Connection::State;
using Type = http::Http2FrameType;
using common::IoErr;
constexpr auto kIdle = 100ms;
constexpr auto kUnlimited = std::chrono::milliseconds::max();
constexpr std::string_view kPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

std::string frame(Type type, std::uint8_t flags = 0, std::uint32_t id = 0, std::string_view payload = {}) {
    std::string result(9 + payload.size(), '\0');
    http::encode_http2_frame_header(reinterpret_cast<std::uint8_t *>(result.data()), payload.size(), type, flags, id);
    result.replace(9, payload.size(), payload);
    return result;
}

std::string request(std::uint32_t id, bool end = false) {
    // Static indexed :method GET, :scheme http, :path /; literal :authority.
    return frame(Type::Headers, end ? 5 : 4, id, std::string_view("\x82\x86\x84\x01\x01x", 6));
}

std::uint32_t u32(const char *data) {
    const auto *p = reinterpret_cast<const unsigned char *>(data);
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

// Only the callback/poll paths used by Http2Connection are implemented.
// Growable strings are test wire captures, outside production request paths.
class WireTransport final : public test::HttpTransportStub {
public:
    bool has_pending_read() const noexcept override { return !incoming.empty() || eof; }
    void feed(std::string_view bytes) {
        incoming.append(bytes);
        notify_read_ready();
    }
    void peer_eof() {
        eof = true;
        notify_read_ready();
    }
    void unblock_write() {
        block_write = false;
        notify_write_ready();
    }
    IoErr poll_read_into(mem::IoBuf &buf, std::size_t &out, event::IoEvent &wait) noexcept override {
        out = std::min(buf.writable(), incoming.size());
        wait = event::IoEvent::None;
        if (out != 0) {
            std::memcpy(buf.writable_data(), incoming.data(), out);
            buf.commit(out);
            incoming.erase(0, out);
            last_read = loop().now();
            return IoErr::None;
        }
        if (eof) {
            return IoErr::None;
        }
        wait = event::IoEvent::Read;
        return IoErr::WouldBlock;
    }
    IoErr poll_writev(mem::IoBufChain &buf, std::size_t &out, event::IoEvent &wait) noexcept override {
        out = 0;
        wait = event::IoEvent::None;
        if (block_write) {
            wait = event::IoEvent::Write;
            return IoErr::WouldBlock;
        }
        while (const auto *front = buf.front()) {
            auto length = front->readable();
            if (length == 0) {
                buf.drop_empty_front();
                continue;
            }
            written.append(reinterpret_cast<const char *>(front->readable_data()), length);
            out += length;
            buf.consume_and_compact(length);
        }
        return IoErr::None;
    }
    async::Task<common::IoResult<void>> shutdown(std::chrono::milliseconds) override {
        co_return std::unexpected(IoErr::NotSupported);
    }
    async::Task<common::IoResult<void>> wait_readable(std::chrono::milliseconds) override {
        co_return std::unexpected(IoErr::NotSupported);
    }
    async::Task<common::IoResult<std::size_t>> read(void *, std::size_t, std::chrono::milliseconds) override {
        co_return std::unexpected(IoErr::NotSupported);
    }
    async::Task<common::IoResult<std::size_t>> read_into(mem::IoBuf &, std::chrono::milliseconds) override {
        co_return std::unexpected(IoErr::NotSupported);
    }
    async::Task<common::IoResult<std::size_t>> readv_into(mem::IoBufChain &, std::chrono::milliseconds) override {
        co_return std::unexpected(IoErr::NotSupported);
    }
    async::Task<common::IoResult<std::size_t>> write(const void *, std::size_t, std::chrono::milliseconds) override {
        co_return std::unexpected(IoErr::NotSupported);
    }
    async::Task<common::IoResult<std::size_t>> write(mem::IoBuf &, std::chrono::milliseconds) override {
        co_return std::unexpected(IoErr::NotSupported);
    }
    async::Task<common::IoResult<std::size_t>> writev(mem::IoBufChain &, std::chrono::milliseconds) override {
        co_return std::unexpected(IoErr::NotSupported);
    }
    void close() override {
        closed = true;
        notify_read_ready(IoErr::Canceled);
        notify_write_ready(IoErr::Canceled);
        notify_terminal(IoErr::Canceled);
    }
    bool valid() const noexcept override { return !closed; }
    int fd() const noexcept override { return -1; }
    std::string_view negotiated_alpn() const noexcept override { return "h2"; }
    const net::SocketAddress &remote_addr() const noexcept override { return peer_; }
    event::EventLoop &loop() const noexcept override { return event::EventLoop::current(); }

    std::size_t frame_count(Type type) const {
        std::size_t count = 0;
        for (std::size_t offset = 0; offset + 9 <= written.size();) {
            auto length = u32(written.data() + offset) >> 8;
            if (offset + 9 + length > written.size()) {
                break;
            }
            count += static_cast<unsigned char>(written[offset + 3]) == static_cast<unsigned char>(type);
            offset += 9 + length;
        }
        return count;
    }
    void expect_goaway(std::uint32_t last_id) const {
        EXPECT_EQ(frame_count(Type::Goaway), 1u);
        for (std::size_t offset = 0; offset + 9 <= written.size();) {
            auto length = u32(written.data() + offset) >> 8;
            ASSERT_LE(offset + 9 + length, written.size());
            if (static_cast<unsigned char>(written[offset + 3]) == static_cast<unsigned char>(Type::Goaway)) {
                ASSERT_GE(length, 8u);
                EXPECT_EQ(u32(written.data() + offset + 9), last_id);
                EXPECT_EQ(u32(written.data() + offset + 13), 0u);
            }
            offset += 9 + length;
        }
    }
    std::string incoming;
    std::string written;
    std::chrono::steady_clock::time_point last_read{};
    bool closed = false;
    bool block_write = false;
    bool eof = false;

private:
    net::SocketAddress peer_{};
};

template<class Predicate>
async::Task<bool> until(Predicate predicate, std::chrono::milliseconds timeout = 2s) {
    const auto deadline = event::EventLoop::current().now() + timeout;
    while (!predicate()) {
        if (event::EventLoop::current().now() >= deadline) {
            co_return false;
        }
        co_await async::sleep(2ms);
    }
    co_return true;
}

http::Http2Connection::Options io_options(std::chrono::milliseconds read = kUnlimited,
                                          std::chrono::milliseconds write = kUnlimited) {
    http::Http2Connection::Options options;
    options.read_timeout = read;
    options.write_timeout = write;
    return options;
}

async::Task<void> respond(http::HttpExchange &exchange) {
    // Sending the response first leaves the stream half closed until the peer
    // finishes the request body. The timer must not retire that connection.
    auto sent = co_await exchange.send_header({.status_code = 204, .end_stream = true});
    if (sent) {
        (void) co_await exchange.discard_body();
    }
}

struct Session {
    explicit Session(std::chrono::milliseconds idle = kIdle, http::Http2Connection::Options options = io_options(),
                     http::HttpHandler handler = respond) :
        factory(handler), connection(event::EventLoop::current(), options, factory, idle) {
        auto transport = std::make_unique<WireTransport>();
        wire = transport.get();
        EXPECT_EQ(connection.start(std::move(transport)), IoErr::None);
    }
    void preface(std::string_view extra = {}) {
        wire->feed(std::string(kPreface) + frame(Type::Settings) + std::string(extra));
    }
    async::Task<http::Http2Connection::CloseResult> join() {
        bool closed = co_await until([&] { return connection.http2().state() == State::Closed; });
        EXPECT_TRUE(closed);
        if (!closed) {
            connection.request_shutdown();
        }
        co_return co_await connection.wait_closed();
    }
    async::Task<void> finish(std::uint32_t last_id = 0) {
        EXPECT_TRUE((co_await join()).has_value());
        wire->expect_goaway(last_id);
        EXPECT_FALSE(connection.idle_timer_entry_.is_in_heap());
    }
    http::ServerRequestFactory factory;
    http::Http2ServerConnection connection;
    WireTransport *wire = nullptr;
};

template<class Fn>
void run(Fn fn) {
    event::EventLoopGroup group(1);
    std::promise<void> done;
    auto future = done.get_future();
    group.start();
    async::spawn(group.at(0), [&]() -> async::DetachedTask {
        co_await fn();
        done.set_value();
    });
    EXPECT_EQ(future.wait_for(10s), std::future_status::ready);
    group.stop();
    group.join();
}
} // namespace

TEST(Http2ServerConnectionTest, InitiallyEmptySessionClosesGracefully) {
    run([]() -> async::Task<void> {
        Session s;
        s.preface();
        co_await s.finish();
        EXPECT_GE(event::EventLoop::current().now() - s.wire->last_read, kIdle);
    });
}

TEST(Http2ServerConnectionTest, LastStreamRemovalStartsAFullIdleWindow) {
    run([]() -> async::Task<void> {
        Session s;
        s.preface(request(1) + request(3));
        EXPECT_TRUE(co_await until([&] { return s.wire->frame_count(Type::Headers) == 2; }));
        EXPECT_TRUE(s.connection.http2().has_active_streams());
        EXPECT_FALSE(s.connection.idle_timer_entry_.is_in_heap());
        s.wire->feed(frame(Type::Data, 1, 1));
        co_await async::sleep(kIdle * 2);
        EXPECT_EQ(s.connection.http2().state(), State::Running);
        EXPECT_TRUE(s.connection.http2().has_active_streams());
        EXPECT_FALSE(s.connection.idle_timer_entry_.is_in_heap());
        s.wire->feed(frame(Type::Data, 1, 3));
        co_await s.finish(3);
        EXPECT_GE(event::EventLoop::current().now() - s.wire->last_read, kIdle);
    });
}

TEST(Http2ServerConnectionTest, NewStreamCancelsExpiryAndResetRestartsIt) {
    run([]() -> async::Task<void> {
        // A long first deadline allows the boundary order to be controlled
        // without racing millisecond sleeps under a loaded test runner.
        Session s(5s);
        s.preface();
        EXPECT_TRUE(co_await until([&] { return s.connection.idle_timer_entry_.is_in_heap(); }));
        s.wire->feed(request(1));
        EXPECT_TRUE(co_await until([&] { return s.wire->frame_count(Type::Headers) == 1; }));
        EXPECT_FALSE(s.connection.idle_timer_entry_.is_in_heap());
        http::Http2ServerConnection::on_idle_timer(&s.connection);
        EXPECT_EQ(s.connection.http2().state(), State::Running);
        s.wire->feed(frame(Type::RstStream, 0, 1, std::string_view("\0\0\0\x08", 4)));
        EXPECT_TRUE(co_await until([&] { return !s.connection.http2().has_active_streams(); }));
        EXPECT_TRUE(s.connection.idle_timer_entry_.is_in_heap());
        s.connection.cancel_idle_timer();
        http::Http2ServerConnection::on_idle_timer(&s.connection);
        co_await s.finish(1);
    });
}

TEST(Http2ServerConnectionTest, ResetOfLastStreamWaitsBeforeRetirement) {
    run([]() -> async::Task<void> {
        Session s;
        s.preface(request(1));
        EXPECT_TRUE(co_await until([&] { return s.wire->frame_count(Type::Headers) == 1; }));
        s.wire->feed(frame(Type::RstStream, 0, 1, std::string_view("\0\0\0\x08", 4)));
        co_await s.finish(1);
        EXPECT_GE(event::EventLoop::current().now() - s.wire->last_read, kIdle);
    });
}

TEST(Http2ServerConnectionTest, RequestEndStreamDoesNotRetireAnUnfinishedResponse) {
    run([]() -> async::Task<void> {
        bool entered = false;
        Session s(kIdle, io_options(), [&](http::HttpExchange &exchange) -> async::Task<void> {
            entered = true;
            (void) co_await exchange.wait_response_channel_closed();
        });
        s.preface(request(1, true));
        EXPECT_TRUE(co_await until([&] { return entered; }));
        co_await async::sleep(kIdle * 2);
        EXPECT_EQ(s.connection.http2().state(), State::Running);
        EXPECT_TRUE(s.connection.http2().has_active_streams());
        EXPECT_FALSE(s.connection.idle_timer_entry_.is_in_heap());
        s.wire->feed(frame(Type::RstStream, 0, 1, std::string_view("\0\0\0\x08", 4)));
        co_await s.finish(1);
    });
}

TEST(Http2ServerConnectionTest, PingAndSettingsDoNotRefreshIdleDeadline) {
    run([]() -> async::Task<void> {
        Session s(200ms);
        s.preface();
        EXPECT_TRUE(co_await until([&] { return s.connection.http2().state() == State::Running; }));
        const auto started = event::EventLoop::current().now();
        unsigned settings = 1;
        while (s.connection.http2().state() == State::Running && event::EventLoop::current().now() - started < 600ms) {
            std::string setting("\0\x03\0\0\0\0", 6);
            setting[5] = static_cast<char>(settings++);
            s.wire->feed(frame(Type::Ping, 0, 0, "abcdefgh") + frame(Type::Settings, 0, 0, setting));
            co_await async::sleep(20ms);
        }
        // This check occurs while traffic is still being supplied. Waiting for
        // closure only after stopping traffic would let a refresh bug pass.
        EXPECT_NE(s.connection.http2().state(), State::Running);
        EXPECT_GT(s.wire->frame_count(Type::Ping), 1u);
        EXPECT_GT(s.wire->frame_count(Type::Settings), 2u);
        co_await s.finish();
    });
}

TEST(Http2ServerConnectionTest, DisabledIdlePolicyRetainsAnEmptySession) {
    run([]() -> async::Task<void> {
        Session s(kUnlimited);
        s.preface();
        co_await async::sleep(kIdle * 3);
        EXPECT_EQ(s.connection.http2().state(), State::Running);
        EXPECT_FALSE(s.connection.idle_timer_entry_.is_in_heap());
        EXPECT_EQ(s.wire->frame_count(Type::Goaway), 0u);
        s.connection.request_drain();
        co_await s.finish();
    });
}

TEST(Http2ServerConnectionTest, ZeroTimeoutIsDeferredAndCanBeCanceledByTheSameReadBatch) {
    run([]() -> async::Task<void> {
        Session s(0ms);
        s.preface(request(1));
        EXPECT_TRUE(co_await until([&] { return s.wire->frame_count(Type::Headers) == 1; }));
        EXPECT_EQ(s.connection.http2().state(), State::Running);
        EXPECT_TRUE(s.connection.http2().has_active_streams());
        s.wire->feed(frame(Type::Data, 1, 1));
        co_await s.finish(1);
    });
}

TEST(Http2ServerConnectionTest, ZeroTimeoutClosesAnInitiallyEmptySession) {
    run([]() -> async::Task<void> {
        Session s(0ms);
        s.preface();
        co_await s.finish();
    });
}

TEST(Http2ServerConnectionTest, PrefaceStillUsesReadTimeout) {
    run([]() -> async::Task<void> {
        Session s(0ms, io_options(100ms));
        EXPECT_EQ(s.connection.http2().state(), State::Start);
        EXPECT_FALSE(s.connection.idle_timer_entry_.is_in_heap());
        auto closed = co_await s.join();
        EXPECT_FALSE(closed);
        if (!closed)
            EXPECT_EQ(closed.error(), IoErr::TimedOut);
        EXPECT_EQ(s.wire->frame_count(Type::Goaway), 0u);
    });
}

TEST(Http2ServerConnectionTest, ExpiryFirstStopsAdmissionAndFlushesGoaway) {
    run([]() -> async::Task<void> {
        Session s(5s);
        s.preface();
        EXPECT_TRUE(co_await until([&] { return s.wire->frame_count(Type::Settings) == 2; }));
        s.wire->block_write = true;
        s.connection.cancel_idle_timer();
        http::Http2ServerConnection::on_idle_timer(&s.connection);
        EXPECT_EQ(s.connection.http2().state(), State::Closing);
        s.wire->feed(request(1, true));
        co_await async::sleep(20ms);
        EXPECT_FALSE(s.connection.http2().has_active_streams());
        EXPECT_EQ(s.wire->frame_count(Type::Headers), 0u);
        EXPECT_FALSE(s.connection.idle_timer_entry_.is_in_heap());
        s.wire->unblock_write();
        co_await s.finish();
    });
}

TEST(Http2ServerConnectionTest, BlockedGoawayIsBoundedByWriteTimeout) {
    run([]() -> async::Task<void> {
        Session s(kIdle, io_options(kUnlimited, 50ms));
        s.preface();
        EXPECT_TRUE(co_await until([&] { return s.wire->frame_count(Type::Settings) == 2; }));
        s.wire->block_write = true;
        auto closed = co_await s.join();
        EXPECT_FALSE(closed);
        if (!closed)
            EXPECT_EQ(closed.error(), IoErr::TimedOut);
        EXPECT_TRUE(s.wire->closed);
        EXPECT_FALSE(s.connection.idle_timer_entry_.is_in_heap());
    });
}

TEST(Http2ServerConnectionTest, AllClosePathsCancelIdleTimerBeforeOwnerDestruction) {
    run([]() -> async::Task<void> {
        for (int mode = 0; mode != 4; ++mode) {
            SCOPED_TRACE(mode);
            Session s(200ms);
            s.preface();
            EXPECT_TRUE(co_await until([&] { return s.connection.idle_timer_entry_.is_in_heap(); }));
            if (mode == 0)
                s.connection.request_drain();
            if (mode == 1)
                s.connection.request_shutdown();
            if (mode == 2)
                s.connection.http2().shutdown();
            if (mode == 3)
                s.wire->peer_eof();
            (void) co_await s.join();
            EXPECT_FALSE(s.connection.idle_timer_entry_.is_in_heap());
            s.connection.request_shutdown();
        }
        // Keep the loop running beyond the destroyed owners' former deadlines.
        co_await async::sleep(300ms);
    });
}

TEST(Http2ServerConnectionTest, DestructorCancelsArmedTimer) {
    run([]() -> async::Task<void> {
        {
            Session s;
            s.preface();
            EXPECT_TRUE(co_await until([&] { return s.connection.idle_timer_entry_.is_in_heap(); }));
        }
        co_await async::sleep(kIdle * 2);
    });
}

TEST(Http2ServerConnectionTest, UnstartedAndFailedStartHaveNoIdleTimer) {
    run([]() -> async::Task<void> {
        http::ServerRequestFactory factory(http::HttpHandler{respond});
        http::Http2ServerConnection connection(event::EventLoop::current(), io_options(), factory);
        EXPECT_EQ(connection.start(nullptr), IoErr::Invalid);
        EXPECT_EQ(connection.http2().state(), State::Init);
        EXPECT_FALSE(connection.idle_timer_entry_.is_in_heap());
        connection.request_shutdown();
        connection.request_shutdown();
        EXPECT_TRUE((co_await connection.wait_closed()).has_value());
    });
}
