#include <fiber/async/WaitGroup.h>
#include <fiber/http/ClientHttp3Exchange.h>

#include <cstddef>
#include <tuple>
#include "Http3ConnectionTestSupport.h"

namespace {

enum class StoppedWrite { All, Chain, Raw, AllFin, ChainFin, RawFin, Trailer, Header };
enum class ResponseTiming { AfterStop, BufferedBeforeStop, CompleteBeforeStop };

template<typename F>
void check_client_request(F &&check) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    auto options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Client;
    options.original_destination_connection_id = connection_id_from({1, 2, 3, 4});
    options.remote_connection_id = connection_id_from({5, 6, 7, 8});
    ClientFixture fixture(endpoint.get(), options);
    auto &h3 = fixture.connection();
    ASSERT_TRUE(start_h3_on_loop(group.at(0), h3.quic(), options, h3).ok);
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        co_await check(h3);
        done.set_value();
    });
    EXPECT_EQ(future.wait_for(5s), std::future_status::ready);
    fixture.finish();
    group.stop();
    group.join();
}

fiber::http::Http3RequestHead upload_head() {
    return {.method = fiber::http::HttpMethod::Post, .scheme = "https", .authority = "example.com", .path = "/upload"};
}

void feed_bad_gateway(fiber::http::Http3ClientConnection &h3, std::uint64_t stream_id) {
    auto response = headers_frame({{":status", "502"}, {"content-length", "11"}});
    append_frame(response, data_frame("bad gateway"));
    feed_stream(h3.quic(), stream_id, response, true);
}

fiber::async::Task<void> expect_bad_gateway(fiber::http::ClientHttp3Exchange &exchange) {
    auto head = co_await exchange.read_header(1s);
    EXPECT_TRUE(head);
    if (!head || *head == nullptr) {
        ADD_FAILURE() << "missing response header";
        co_return;
    }
    EXPECT_EQ((*head)->status_code, 502);
    std::string body;
    for (;;) {
        auto chunk = co_await exchange.read_body(4, 1s);
        EXPECT_TRUE(chunk);
        if (!chunk) {
            co_return;
        }
        const bool complete = chunk->complete();
        auto bytes = chain_to_bytes(std::move(*chunk));
        body.append(bytes.begin(), bytes.end());
        if (complete) {
            break;
        }
    }
    EXPECT_EQ(body, "bad gateway");
    EXPECT_EQ(exchange.outcome(), fiber::http::Http3RequestOutcome::Complete);
}

fiber::async::Task<fiber::common::IoResult<void>> write_until_stopped(fiber::http::ClientHttp3Exchange &exchange,
                                                                      StoppedWrite kind, std::size_t size) {
    fiber::mem::BufPool pool;
    if (kind == StoppedWrite::Header) {
        std::string path(size, 'p');
        path.front() = '/';
        auto head = upload_head();
        head.path = path;
        co_return co_await exchange.send_request_header(head, false, 1s);
    }
    if (kind == StoppedWrite::Trailer) {
        fiber::http::HttpHeaders trailers(pool);
        const std::string value(size, 't');
        trailers.set("digest", value);
        co_return co_await exchange.write_trailer(trailers, 1s);
    }
    const bool fin = kind == StoppedWrite::AllFin || kind == StoppedWrite::ChainFin || kind == StoppedWrite::RawFin;
    const std::string payload(fin ? 0 : size, 'x');
    const auto *data = reinterpret_cast<const std::uint8_t *>(payload.data());
    if (kind == StoppedWrite::All || kind == StoppedWrite::AllFin) {
        auto result = co_await exchange.write_all(data, payload.size(), fin, 1s);
        co_return result ? fiber::common::IoResult<void>{} : std::unexpected(result.error());
    }
    if (kind == StoppedWrite::Raw || kind == StoppedWrite::RawFin) {
        std::size_t sent = 0;
        do {
            auto result = co_await exchange.write(data + sent, payload.size() - sent, fin, 1s);
            if (!result) {
                co_return std::unexpected(result.error());
            }
            sent += *result;
        } while (sent != payload.size());
        co_return fiber::common::IoResult<void>{};
    }
    fiber::mem::IoBufChain chain;
    if (!payload.empty()) {
        auto buf = fiber::mem::IoBuf::allocate(payload.size());
        if (!buf) {
            co_return std::unexpected(fiber::common::IoErr::NoMem);
        }
        std::memcpy(buf.writable_data(), payload.data(), payload.size());
        buf.commit(payload.size());
        if (!chain.append(std::move(buf))) {
            co_return std::unexpected(fiber::common::IoErr::NoMem);
        }
    }
    if (fin) {
        chain.mark_complete();
    }
    do {
        auto result = co_await exchange.write(chain, 1s);
        if (!result) {
            co_return std::unexpected(result.error());
        }
    } while (chain.readable_bytes() != 0 || chain.complete());
    co_return fiber::common::IoResult<void>{};
}

class Http3StoppedUploadTest : public testing::TestWithParam<std::tuple<StoppedWrite, ResponseTiming>> {};

TEST_P(Http3StoppedUploadTest, PreservesResponseAfterWriteFailure) {
    const auto [kind, timing] = GetParam();
    check_client_request([&](fiber::http::Http3ClientConnection &h3) -> fiber::async::Task<void> {
        fiber::mem::BufPool pool;
        auto exchange = h3.open_exchange(pool);
        EXPECT_TRUE(co_await exchange.send_request_header(upload_head(), false, 1s));
        if (timing != ResponseTiming::AfterStop) {
            feed_bad_gateway(h3, exchange.stream_id());
        }
        if (timing == ResponseTiming::CompleteBeforeStop) {
            co_await expect_bad_gateway(exchange);
        }
        EXPECT_TRUE(h3.quic().recv_stop_sending_frame(
                {.id = exchange.stream_id(),
                 .error_code = static_cast<std::uint64_t>(fiber::http::Http3ErrorCode::NoError)}));
        auto written = co_await write_until_stopped(exchange, kind, 1);
        EXPECT_FALSE(written);
        if (!written) {
            EXPECT_EQ(written.error(), fiber::common::IoErr::BrokenPipe);
        }
        EXPECT_FALSE(exchange.stream()->stop_sending());
        if (timing == ResponseTiming::AfterStop) {
            feed_bad_gateway(h3, exchange.stream_id());
        }
        if (timing == ResponseTiming::CompleteBeforeStop) {
            EXPECT_EQ(exchange.outcome(), fiber::http::Http3RequestOutcome::Complete);
            auto body = co_await exchange.read_body(4, 1s);
            EXPECT_TRUE(body && body->complete());
        } else {
            EXPECT_EQ(exchange.outcome(), fiber::http::Http3RequestOutcome::PossiblyProcessed);
            co_await expect_bad_gateway(exchange);
        }
    });
}

INSTANTIATE_TEST_SUITE_P(WriteEntries, Http3StoppedUploadTest,
                         testing::Combine(testing::Values(StoppedWrite::All, StoppedWrite::Chain, StoppedWrite::Raw,
                                                          StoppedWrite::AllFin, StoppedWrite::ChainFin,
                                                          StoppedWrite::RawFin, StoppedWrite::Trailer),
                                          testing::Values(ResponseTiming::AfterStop, ResponseTiming::BufferedBeforeStop,
                                                          ResponseTiming::CompleteBeforeStop)));

class Http3BlockedUploadTest : public testing::TestWithParam<StoppedWrite> {};

TEST_P(Http3BlockedUploadTest, StopSendingWakesWriterAndPreservesResponse) {
    const auto kind = GetParam();
    check_client_request([&](fiber::http::Http3ClientConnection &h3) -> fiber::async::Task<void> {
        fiber::mem::BufPool pool;
        auto exchange = h3.open_exchange(pool);
        if (kind != StoppedWrite::Header) {
            EXPECT_TRUE(co_await exchange.send_request_header(upload_head(), false, 1s));
        }
        fiber::async::WaitGroup writer;
        writer.add();
        bool write_finished = false;
        fiber::async::spawn(fiber::event::EventLoop::current(), [&]() -> fiber::async::DetachedTask {
            auto written = co_await write_until_stopped(exchange, kind, 2048);
            EXPECT_FALSE(written);
            if (!written) {
                EXPECT_EQ(written.error(), fiber::common::IoErr::BrokenPipe);
            }
            write_finished = true;
            writer.done();
        });
        co_await fiber::async::sleep(10ms);
        EXPECT_FALSE(write_finished);
        EXPECT_TRUE(h3.quic().recv_stop_sending_frame(
                {.id = exchange.stream_id(),
                 .error_code = static_cast<std::uint64_t>(fiber::http::Http3ErrorCode::NoError)}));
        co_await writer.join();
        EXPECT_FALSE(exchange.stream()->stop_sending());
        // Retries must fail before validating any unfinished DATA frame suffix.
        auto retry = co_await exchange.write(nullptr, 0, true, 1s);
        EXPECT_FALSE(retry);
        if (!retry) {
            EXPECT_EQ(retry.error(), fiber::common::IoErr::BrokenPipe);
        }
        feed_bad_gateway(h3, exchange.stream_id());
        co_await expect_bad_gateway(exchange);
    });
}

INSTANTIATE_TEST_SUITE_P(WriteEntries, Http3BlockedUploadTest,
                         testing::Values(StoppedWrite::All, StoppedWrite::Chain, StoppedWrite::Raw,
                                         StoppedWrite::Trailer, StoppedWrite::Header));

enum class StoppedResponseFailure { Reset, Truncated, Timeout, Abort, Goaway, ConnectionClose };

class Http3StoppedResponseTest : public testing::TestWithParam<StoppedResponseFailure> {};

TEST_P(Http3StoppedResponseTest, StillReportsResponseFailure) {
    const auto failure = GetParam();
    check_client_request([&](fiber::http::Http3ClientConnection &h3) -> fiber::async::Task<void> {
        fiber::mem::BufPool pool;
        auto exchange = h3.open_exchange(pool);
        EXPECT_TRUE(co_await exchange.send_request_header(upload_head(), false, 1s));
        EXPECT_TRUE(h3.quic().recv_stop_sending_frame(
                {.id = exchange.stream_id(),
                 .error_code = static_cast<std::uint64_t>(fiber::http::Http3ErrorCode::NoError)}));
        EXPECT_FALSE(co_await write_until_stopped(exchange, StoppedWrite::All, 1));
        switch (failure) {
            case StoppedResponseFailure::Reset:
                EXPECT_TRUE(h3.quic().recv_reset_stream_frame(
                        {.id = exchange.stream_id(),
                         .error_code = static_cast<std::uint64_t>(fiber::http::Http3ErrorCode::RequestRejected),
                         .final_size = 0}));
                break;
            case StoppedResponseFailure::Truncated: {
                auto response = headers_frame({{":status", "502"}, {"content-length", "11"}});
                append_frame(response, data_frame("bad"));
                feed_stream(h3.quic(), exchange.stream_id(), response, true);
                auto head = co_await exchange.read_header(1s);
                EXPECT_TRUE(head && *head != nullptr);
                auto body = co_await exchange.read_body(1024, 1s);
                EXPECT_FALSE(body);
                EXPECT_NE(exchange.outcome(), fiber::http::Http3RequestOutcome::Complete);
                co_return;
            }
            case StoppedResponseFailure::Timeout:
                break;
            case StoppedResponseFailure::Abort:
                EXPECT_TRUE(exchange.abort());
                EXPECT_TRUE(exchange.stream()->stop_sending());
                break;
            case StoppedResponseFailure::Goaway: {
                auto control = control_settings_stream();
                append_control_varint_frame(control, fiber::http::Http3FrameType::Goaway, exchange.stream_id());
                feed_stream(h3.quic(), 3, control);
                co_await fiber::async::sleep(10ms);
                break;
            }
            case StoppedResponseFailure::ConnectionClose:
                fiber::http::Http3ClientConnectionTestAccess::close(h3);
                break;
        }
        auto head = co_await exchange.read_header(10ms);
        EXPECT_FALSE(head);
        EXPECT_EQ(exchange.outcome(),
                  failure == StoppedResponseFailure::Reset || failure == StoppedResponseFailure::Goaway
                          ? fiber::http::Http3RequestOutcome::Rejected
                          : fiber::http::Http3RequestOutcome::PossiblyProcessed);
    });
}

INSTANTIATE_TEST_SUITE_P(ResponseErrors, Http3StoppedResponseTest,
                         testing::Values(StoppedResponseFailure::Reset, StoppedResponseFailure::Truncated,
                                         StoppedResponseFailure::Timeout, StoppedResponseFailure::Abort,
                                         StoppedResponseFailure::Goaway, StoppedResponseFailure::ConnectionClose));

TEST(Http3ClientConnectionTest, UploadTimeoutStillAbortsResponse) {
    check_client_request([](fiber::http::Http3ClientConnection &h3) -> fiber::async::Task<void> {
        fiber::mem::BufPool pool;
        auto exchange = h3.open_exchange(pool);
        EXPECT_TRUE(co_await exchange.send_request_header(upload_head(), false, 1s));
        const std::string payload(2048, 'x');
        auto written = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(payload.data()),
                                                   payload.size(), false, 10ms);
        EXPECT_FALSE(written);
        if (!written) {
            EXPECT_EQ(written.error(), fiber::common::IoErr::TimedOut);
        }
        EXPECT_TRUE(exchange.stream()->stop_sending());
        auto head = co_await exchange.read_header(10ms);
        EXPECT_FALSE(head);
        if (!head) {
            EXPECT_EQ(head.error(), fiber::common::IoErr::TimedOut);
        }
    });
}

TEST(Http3ClientConnectionTest, OtherStopSendingCodesPreserveResponse) {
    check_client_request([](fiber::http::Http3ClientConnection &h3) -> fiber::async::Task<void> {
        for (const std::uint64_t code: {static_cast<std::uint64_t>(fiber::http::Http3ErrorCode::RequestCancelled),
                                        static_cast<std::uint64_t>(0xdead)}) {
            fiber::mem::BufPool pool;
            auto exchange = h3.open_exchange(pool);
            EXPECT_TRUE(co_await exchange.send_request_header(upload_head(), false, 1s));
            EXPECT_TRUE(h3.quic().recv_stop_sending_frame({.id = exchange.stream_id(), .error_code = code}));
            auto written = co_await exchange.write(nullptr, 0, true, 1s);
            EXPECT_FALSE(written);
            if (!written) {
                EXPECT_EQ(written.error(), fiber::common::IoErr::BrokenPipe);
            }
            feed_bad_gateway(h3, exchange.stream_id());
            co_await expect_bad_gateway(exchange);
        }
    });
}

TEST(Http3ClientConnectionTest, CompletedResponseSurvivesUploadTimeout) {
    check_client_request([](fiber::http::Http3ClientConnection &h3) -> fiber::async::Task<void> {
        fiber::mem::BufPool pool;
        auto exchange = h3.open_exchange(pool);
        EXPECT_TRUE(co_await exchange.send_request_header(upload_head(), false, 1s));
        feed_bad_gateway(h3, exchange.stream_id());
        co_await expect_bad_gateway(exchange);
        const std::string payload(2048, 'x');
        auto written = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(payload.data()),
                                                   payload.size(), false, 10ms);
        EXPECT_FALSE(written);
        if (!written) {
            EXPECT_EQ(written.error(), fiber::common::IoErr::TimedOut);
        }
        EXPECT_FALSE(exchange.stream()->stop_sending());
        EXPECT_TRUE(exchange.stream()->send_aborted());
        auto retry = co_await exchange.write(nullptr, 0, true, 1s);
        EXPECT_FALSE(retry);
        if (!retry) {
            EXPECT_EQ(retry.error(), fiber::common::IoErr::BrokenPipe);
        }
        EXPECT_EQ(exchange.outcome(), fiber::http::Http3RequestOutcome::Complete);
        auto body = co_await exchange.read_body(4, 1s);
        EXPECT_TRUE(body && body->complete());
    });
}

} // namespace

TEST(Http3ClientConnectionTest, LocalSettingsUseConfiguredFieldSectionLimit) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    auto quic_options = fiber::test::quic_options();
    quic_options.role = fiber::quic::QuicConnectionRole::Client;
    struct Case {
        std::size_t receive_limit;
        std::uint64_t advertised_limit;
        std::uint64_t expected;
    };
    const Case cases[] = {{.receive_limit = static_cast<std::size_t>(128 * 1024),
                           .advertised_limit = 0,
                           .expected = static_cast<std::uint64_t>(128 * 1024)},
                          {.receive_limit = 4096, .advertised_limit = 0, .expected = 4096},
                          {.receive_limit = 4096, .advertised_limit = 2048, .expected = 2048},
                          {.receive_limit = 0, .advertised_limit = 0, .expected = 0}};
    for (const auto &test: cases) {
        fiber::http::Http3Client::Options options{};
        options.max_field_section_size = test.receive_limit;
        options.local_settings.max_field_section_size = test.advertised_limit;
        ClientFixture fixture(endpoint.get(), quic_options, options);
        EXPECT_EQ(fixture.connection().local_settings().max_field_section_size, test.expected);
        fixture.finish();
    }
    group.stop();
    group.join();
}

TEST(Http3ClientConnectionTest, ClientStopsAcceptingRequestsWhenQuicShutdownBegins) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    quic_options.role = fiber::quic::QuicConnectionRole::Client;
    quic_options.original_destination_connection_id = connection_id_from({0x01, 0x02, 0x03, 0x04});
    quic_options.remote_connection_id = connection_id_from({0x11, 0x12, 0x13, 0x14});
    ClientFixture fixture(endpoint.get(), quic_options);
    auto &h3 = fixture.connection();
    auto &quic = h3.quic();

    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);
    ASSERT_TRUE(h3.accepting_requests());

    std::promise<bool> stopped_accepting;
    auto stopped_future = stopped_accepting.get_future();
    fiber::async::spawn(group.at(0), [&quic, &h3, &stopped_accepting]() -> fiber::async::DetachedTask {
        quic.shutdown();
        stopped_accepting.set_value(h3.state() == fiber::http::Http3ConnectionState::Running && quic.shutting_down() &&
                                    !h3.accepting_requests());
        co_return;
    });
    ASSERT_EQ(stopped_future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(stopped_future.get());

    fixture.finish();
    group.stop();
    group.join();
}

TEST(Http3ClientConnectionTest, ClientDrainsAndRejectsRequestsAtOrAbovePeerGoawayId) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    quic_options.role = fiber::quic::QuicConnectionRole::Client;
    quic_options.original_destination_connection_id = connection_id_from({0x01, 0x02, 0x03, 0x04});
    quic_options.remote_connection_id = connection_id_from({0x11, 0x12, 0x13, 0x14});
    ClientFixture fixture(endpoint.get(), quic_options);
    auto &h3 = fixture.connection();
    auto &quic = h3.quic();
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    ClientRequestNotification below_notification;
    ClientRequestNotification equal_notification;
    ClientRequestNotification above_notification;
    fiber::http::Http3ClientRequestEntry below{
            .owner = &below_notification,
            .on_rejected = &on_client_request_rejected,
            .on_connection_close = &on_client_request_closed,
            .stream_id = 0,
    };
    fiber::http::Http3ClientRequestEntry equal{
            .owner = &equal_notification,
            .on_rejected = &on_client_request_rejected,
            .on_connection_close = &on_client_request_closed,
            .stream_id = 4,
    };
    fiber::http::Http3ClientRequestEntry above{
            .owner = &above_notification,
            .on_rejected = &on_client_request_rejected,
            .on_connection_close = &on_client_request_closed,
            .stream_id = 8,
    };
    ASSERT_TRUE(fiber::http::Http3ClientConnectionTestAccess::register_client_request(h3, below));
    ASSERT_TRUE(fiber::http::Http3ClientConnectionTestAccess::register_client_request(h3, equal));
    ASSERT_TRUE(fiber::http::Http3ClientConnectionTestAccess::register_client_request(h3, above));

    auto control = control_settings_stream();
    append_control_varint_frame(control, fiber::http::Http3FrameType::Goaway, 4);
    std::promise<void> feed_done;
    auto feed_future = feed_done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &control, &feed_done]() -> fiber::async::DetachedTask {
        return feed_stream_then_delay(&quic, &control, 3, &feed_done);
    });

    ASSERT_EQ(feed_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(h3.state(), fiber::http::Http3ConnectionState::Draining);
    EXPECT_TRUE(h3.peer_goaway_received());
    EXPECT_EQ(h3.peer_goaway_id(), 4U);
    EXPECT_FALSE(h3.accepting_requests());
    EXPECT_EQ(below_notification.rejected, 0U);
    EXPECT_EQ(equal_notification.rejected, 1U);
    EXPECT_EQ(equal_notification.goaway_id, 4U);
    EXPECT_EQ(above_notification.rejected, 1U);
    EXPECT_EQ(above_notification.goaway_id, 4U);

    std::promise<void> closed;
    auto closed_future = closed.get_future();
    fiber::async::spawn(group.at(0), [&h3, &below, &closed]() -> fiber::async::DetachedTask {
        fiber::http::Http3ClientConnectionTestAccess::unregister_client_request(h3, below);
        h3.graceful_shutdown();
        co_await h3.wait_closed();
        closed.set_value();
        co_return;
    });
    ASSERT_EQ(closed_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(below_notification.closed, 0U);

    fixture.finish();
    group.stop();
    group.join();
}

TEST(Http3ClientConnectionTest, ClientRejectsIncreasingPeerGoawayId) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    quic_options.role = fiber::quic::QuicConnectionRole::Client;
    quic_options.original_destination_connection_id = connection_id_from({0x01, 0x02, 0x03, 0x04});
    quic_options.remote_connection_id = connection_id_from({0x11, 0x12, 0x13, 0x14});
    ClientFixture fixture(endpoint.get(), quic_options);
    auto &h3 = fixture.connection();
    auto &quic = h3.quic();
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    auto control = control_settings_stream();
    append_control_varint_frame(control, fiber::http::Http3FrameType::Goaway, 4);
    append_control_varint_frame(control, fiber::http::Http3FrameType::Goaway, 8);
    std::promise<void> closed;
    auto closed_future = closed.get_future();
    fiber::async::spawn(group.at(0), [&quic, &h3, &control, &closed]() -> fiber::async::DetachedTask {
        return feed_stream_then_wait_closed(&quic, &h3, &control, 3, &closed);
    });

    ASSERT_EQ(closed_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(h3.close_error(), fiber::http::Http3ErrorCode::IdError);

    fixture.finish();
    group.stop();
    group.join();
}

TEST(Http3ClientConnectionTest, ClientRejectsPushStreamWhenPushIsDisabled) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    quic_options.role = fiber::quic::QuicConnectionRole::Client;
    quic_options.original_destination_connection_id = connection_id_from({0x01, 0x02, 0x03, 0x04});
    quic_options.remote_connection_id = connection_id_from({0x11, 0x12, 0x13, 0x14});
    ClientFixture fixture(endpoint.get(), quic_options);
    auto &h3 = fixture.connection();
    auto &quic = h3.quic();
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    auto push = uni_stream_type(fiber::http::Http3StreamType::Push);
    append_varint(push, 0);
    std::promise<void> closed;
    auto closed_future = closed.get_future();
    fiber::async::spawn(group.at(0), [&quic, &h3, &push, &closed]() -> fiber::async::DetachedTask {
        return feed_stream_then_wait_closed(&quic, &h3, &push, 3, &closed);
    });

    ASSERT_EQ(closed_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(h3.close_error(), fiber::http::Http3ErrorCode::IdError);

    fixture.finish();
    group.stop();
    group.join();
}

TEST(Http3ClientConnectionTest, ClientRejectsMaxPushIdFrame) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    quic_options.role = fiber::quic::QuicConnectionRole::Client;
    quic_options.original_destination_connection_id = connection_id_from({0x01, 0x02, 0x03, 0x04});
    quic_options.remote_connection_id = connection_id_from({0x11, 0x12, 0x13, 0x14});
    ClientFixture fixture(endpoint.get(), quic_options);
    auto &h3 = fixture.connection();
    auto &quic = h3.quic();
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    auto control = control_settings_stream();
    append_control_varint_frame(control, fiber::http::Http3FrameType::MaxPushId, 0);
    std::promise<void> closed;
    auto closed_future = closed.get_future();
    fiber::async::spawn(group.at(0), [&quic, &h3, &control, &closed]() -> fiber::async::DetachedTask {
        return feed_stream_then_wait_closed(&quic, &h3, &control, 3, &closed);
    });

    ASSERT_EQ(closed_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(h3.close_error(), fiber::http::Http3ErrorCode::FrameUnexpected);

    fixture.finish();
    group.stop();
    group.join();
}

namespace {
void check_local_drain_with_registered_request(std::chrono::milliseconds drain_timeout, bool complete_in_flight) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    auto options = fiber::test::quic_options();
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    options.role = fiber::quic::QuicConnectionRole::Client;
    options.original_destination_connection_id = connection_id_from({1, 2, 3, 4});
    options.remote_connection_id = connection_id_from({5, 6, 7, 8});
    fiber::http::Http3Client::Options client_options{};
    client_options.drain_timeout = drain_timeout;
    ClientFixture fixture(endpoint.get(), options, client_options);
    auto &h3 = fixture.connection();
    ASSERT_TRUE(start_h3_on_loop(group.at(0), h3.quic(), options, h3).ok);
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        fiber::mem::BufPool pool;
        auto exchange = h3.open_exchange(pool);
        fiber::http::Http3RequestHead head{
                .method = fiber::http::HttpMethod::Get, .scheme = "https", .authority = "example.com", .path = "/"};
        EXPECT_TRUE(co_await exchange.send_request_header(head, true));
        EXPECT_TRUE(h3.accepting_requests());

        const auto drain_started = std::chrono::steady_clock::now();
        h3.graceful_shutdown();
        co_await fiber::async::sleep(20ms);
        EXPECT_EQ(h3.state(), fiber::http::Http3ConnectionState::Draining);

        if (complete_in_flight) {
            // The registered request finishes well inside the drain timeout, so
            // the drain must proceed as soon as its entry settles.
            auto response = headers_frame({{":status", "200"}, {"content-length", "0"}});
            feed_stream(h3.quic(), exchange.stream_id(), response, true);
            EXPECT_TRUE(co_await exchange.read_header(1s));
            auto body = co_await exchange.read_body(1024, 1s);
            EXPECT_TRUE(body);
            EXPECT_EQ(exchange.outcome(), fiber::http::Http3RequestOutcome::Complete);
        }

        co_await h3.wait_closed();
        const auto elapsed = std::chrono::steady_clock::now() - drain_started;
        EXPECT_EQ(h3.state(), fiber::http::Http3ConnectionState::Closed);
        EXPECT_EQ(h3.close_error(), fiber::http::Http3ErrorCode::NoError);
        if (complete_in_flight) {
            EXPECT_LT(elapsed, 2s);
        } else {
            // The registered request never settles on its own: the drain holds
            // until the timeout forces the close, then the entry is detached.
            EXPECT_GE(elapsed, drain_timeout - std::chrono::milliseconds(25));
            EXPECT_LT(elapsed, 5s);
            auto header = co_await exchange.read_header(1s);
            EXPECT_FALSE(header);
            EXPECT_EQ(exchange.outcome(), fiber::http::Http3RequestOutcome::PossiblyProcessed);
        }
        done.set_value();
    });
    ASSERT_EQ(future.wait_for(10s), std::future_status::ready);
    fixture.finish();
    group.stop();
    group.join();
}
} // namespace

TEST(Http3ClientConnectionTest, LocalDrainSettlesRegisteredRequestAtDrainTimeout) {
    check_local_drain_with_registered_request(std::chrono::milliseconds(120), false);
}

TEST(Http3ClientConnectionTest, LocalDrainClosesEarlyWhenRegisteredRequestsComplete) {
    check_local_drain_with_registered_request(std::chrono::seconds(10), true);
}


namespace {
void check_queued_request_drain(bool peer_goaway) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    auto options = fiber::test::quic_options();
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    options.role = fiber::quic::QuicConnectionRole::Client;
    options.original_destination_connection_id = connection_id_from({1, 2, 3, 4});
    options.remote_connection_id = connection_id_from({5, 6, 7, 8});
    ClientFixture fixture(endpoint.get(), options);
    auto &h3 = fixture.connection();
    ASSERT_TRUE(start_h3_on_loop(group.at(0), h3.quic(), options, h3).ok);
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&h3, peer_goaway, &done]() -> fiber::async::DetachedTask {
        fiber::mem::BufPool pool;
        auto exchange = h3.open_exchange(pool);
        for (int i = 0; i != 8; ++i) {
            auto stream = fiber::http::Http3ControlStreams::create_stream();
            EXPECT_TRUE(
                    h3.quic().try_attach_local_stream(std::move(stream), fiber::quic::QuicStreamType::Bidirectional));
        }
        fiber::async::WaitGroup sent;
        sent.add();
        fiber::async::spawn(fiber::event::EventLoop::current(), [&]() -> fiber::async::DetachedTask {
            auto result = co_await exchange.send_request_header({.method = fiber::http::HttpMethod::Get,
                                                                 .scheme = "https",
                                                                 .authority = "example.com",
                                                                 .path = "/"},
                                                                true);
            EXPECT_FALSE(result);
            if (!result) {
                EXPECT_EQ(result.error(), fiber::common::IoErr::Canceled);
            }
            EXPECT_EQ(exchange.outcome(), fiber::http::Http3RequestOutcome::NotSent);
            sent.done();
        });
        co_await fiber::async::sleep(1ms);
        EXPECT_EQ(h3.local_stream_gate().waiter_count(fiber::quic::QuicStreamType::Bidirectional), 1U);
        if (peer_goaway) {
            auto control = control_settings_stream();
            append_control_varint_frame(control, fiber::http::Http3FrameType::Goaway, 0);
            feed_stream(h3.quic(), 3, control);
        } else {
            h3.graceful_shutdown();
        }
        co_await sent.join();
        EXPECT_FALSE(h3.accepting_requests());
        EXPECT_FALSE(h3.local_stream_gate().has_waiters());
        h3.graceful_shutdown();
        co_await h3.wait_closed();
        done.set_value();
    });
    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    fixture.finish();
    group.stop();
    group.join();
}
} // namespace

TEST(Http3ClientConnectionTest, LocalDrainCancelsRequestWaitingForStreamCredit) { check_queued_request_drain(false); }
TEST(Http3ClientConnectionTest, PeerGoawayCancelsRequestWaitingForStreamCredit) { check_queued_request_drain(true); }

TEST(Http3ClientConnectionTest, SendRequestHeaderDeadlineSurvivesStreamAttachWait) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    auto options = fiber::test::quic_options();
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    options.role = fiber::quic::QuicConnectionRole::Client;
    options.original_destination_connection_id = connection_id_from({1, 2, 3, 4});
    options.remote_connection_id = connection_id_from({5, 6, 7, 8});
    ClientFixture fixture(endpoint.get(), options);
    auto &h3 = fixture.connection();
    // Zero bidi-remote flow-control credit parks the HEADERS write after the
    // stream gate admits the request, isolating the exchange-level deadline.
    ASSERT_TRUE(start_h3_on_loop(group.at(0), h3.quic(), options, h3, 1024, 0).ok);
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&h3, &done]() -> fiber::async::DetachedTask {
        fiber::mem::BufPool pool;
        auto exchange = h3.open_exchange(pool);
        for (int i = 0; i != 8; ++i) {
            auto stream = fiber::http::Http3ControlStreams::create_stream();
            EXPECT_TRUE(
                    h3.quic().try_attach_local_stream(std::move(stream), fiber::quic::QuicStreamType::Bidirectional));
        }

        fiber::async::WaitGroup sent;
        sent.add();
        const auto started_at = std::chrono::steady_clock::now();
        fiber::async::spawn(fiber::event::EventLoop::current(), [&]() -> fiber::async::DetachedTask {
            auto result = co_await exchange.send_request_header({.method = fiber::http::HttpMethod::Get,
                                                                 .scheme = "https",
                                                                 .authority = "example.com",
                                                                 .path = "/"},
                                                                false, 500ms);
            const auto elapsed = std::chrono::steady_clock::now() - started_at;
            EXPECT_FALSE(result);
            if (!result) {
                EXPECT_EQ(result.error(), fiber::common::IoErr::TimedOut);
            }
            // Credit is released at 400ms. A restarted budget would time out
            // near 900ms; the shared deadline must expire near 500ms.
            EXPECT_GE(elapsed, 480ms);
            EXPECT_LT(elapsed, 700ms);
            sent.done();
        });

        co_await fiber::async::sleep(400ms);
        EXPECT_EQ(h3.local_stream_gate().waiter_count(fiber::quic::QuicStreamType::Bidirectional), 1U);
        fiber::quic::QuicMaxStreamsFrame credit{};
        credit.limit = 9;
        credit.bidirectional = true;
        EXPECT_TRUE(h3.quic().recv_max_streams_frame(credit));
        co_await sent.join();
        h3.graceful_shutdown();
        co_await h3.wait_closed();
        done.set_value();
    });
    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    fixture.finish();
    group.stop();
    group.join();
}

TEST(Http3ClientConnectionTest, WaitClosedResolvesWhenStartupFails) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    auto options = fiber::test::quic_options();
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    options.role = fiber::quic::QuicConnectionRole::Client;
    options.original_destination_connection_id = connection_id_from({1, 2, 3, 4});
    options.remote_connection_id = connection_id_from({5, 6, 7, 8});
    ClientFixture fixture(endpoint.get(), options);
    auto &h3 = fixture.connection();
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        auto params = valid_peer_transport_params(options);
        // No unidirectional stream credit: the local control stream can never
        // open, so start() parks on the gate until the connection closes.
        params.initial_max_streams_uni = 0;
        params.has_original_destination_connection_id = true;
        params.original_destination_connection_id = options.original_destination_connection_id;
        EXPECT_TRUE(h3.quic().adopt_server_initial_source_connection_id(options.remote_connection_id));
        EXPECT_TRUE(h3.quic().apply_peer_transport_params(params));
        EXPECT_TRUE(h3.quic().mark_established());
        fiber::async::WaitGroup tasks;
        tasks.add(2);
        fiber::async::spawn(fiber::event::EventLoop::current(), [&]() -> fiber::async::DetachedTask {
            auto started = co_await fiber::http::Http3ClientConnectionTestAccess::start(h3);
            EXPECT_FALSE(started);
            tasks.done();
        });
        co_await fiber::async::sleep(1ms);
        EXPECT_EQ(h3.local_stream_gate().waiter_count(), 1U);
        fiber::async::spawn(fiber::event::EventLoop::current(), [&]() -> fiber::async::DetachedTask {
            co_await h3.wait_closed();
            EXPECT_EQ(h3.state(), fiber::http::Http3ConnectionState::Closed);
            tasks.done();
        });
        co_await fiber::async::sleep(1ms);
        h3.shutdown();
        h3.quic().mark_closed();
        co_await tasks.join();
        EXPECT_FALSE(h3.local_stream_gate().has_waiters());
        EXPECT_EQ(h3.quic().ref_count(), 1U);
        EXPECT_EQ(h3.close_error(), fiber::http::Http3ErrorCode::RequestCancelled);
        done.set_value();
    });
    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    fixture.finish();
    group.stop();
    group.join();
}
