#include <fiber/http/ClientHttp3Exchange.h>
#include "Http3ConnectionTestSupport.h"

TEST(Http3ClientConnectionTest, ClientStopsAcceptingRequestsWhenQuicShutdownBegins) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &group.at(0);
    quic_options.role = fiber::quic::QuicConnectionRole::Client;
    quic_options.original_destination_connection_id = connection_id_from({0x01, 0x02, 0x03, 0x04});
    quic_options.remote_connection_id = connection_id_from({0x11, 0x12, 0x13, 0x14});
    ClientFixture fixture(quic_options);
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
    quic_options.loop = &group.at(0);
    quic_options.role = fiber::quic::QuicConnectionRole::Client;
    quic_options.original_destination_connection_id = connection_id_from({0x01, 0x02, 0x03, 0x04});
    quic_options.remote_connection_id = connection_id_from({0x11, 0x12, 0x13, 0x14});
    ClientFixture fixture(quic_options);
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
    ASSERT_TRUE(h3.register_client_request(below));
    ASSERT_TRUE(h3.register_client_request(equal));
    ASSERT_TRUE(h3.register_client_request(above));

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
        h3.unregister_client_request(below);
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
    quic_options.loop = &group.at(0);
    quic_options.role = fiber::quic::QuicConnectionRole::Client;
    quic_options.original_destination_connection_id = connection_id_from({0x01, 0x02, 0x03, 0x04});
    quic_options.remote_connection_id = connection_id_from({0x11, 0x12, 0x13, 0x14});
    ClientFixture fixture(quic_options);
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
    quic_options.loop = &group.at(0);
    quic_options.role = fiber::quic::QuicConnectionRole::Client;
    quic_options.original_destination_connection_id = connection_id_from({0x01, 0x02, 0x03, 0x04});
    quic_options.remote_connection_id = connection_id_from({0x11, 0x12, 0x13, 0x14});
    ClientFixture fixture(quic_options);
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
    quic_options.loop = &group.at(0);
    quic_options.role = fiber::quic::QuicConnectionRole::Client;
    quic_options.original_destination_connection_id = connection_id_from({0x01, 0x02, 0x03, 0x04});
    quic_options.remote_connection_id = connection_id_from({0x11, 0x12, 0x13, 0x14});
    ClientFixture fixture(quic_options);
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
    options.loop = &group.at(0);
    options.role = fiber::quic::QuicConnectionRole::Client;
    options.original_destination_connection_id = connection_id_from({1, 2, 3, 4});
    options.remote_connection_id = connection_id_from({5, 6, 7, 8});
    fiber::http::Http3ClientConnectionImpl::Options http_options{};
    http_options.drain_timeout = drain_timeout;
    ClientFixture fixture(options, http_options);
    auto &h3 = fixture.connection();
    ASSERT_TRUE(start_h3_on_loop(group.at(0), h3.quic(), options, h3).ok);
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        auto handle = h3.make_handle(h3.quic().lease());
        fiber::mem::BufPool pool;
        auto exchange = handle.open_exchange(pool);
        fiber::http::Http3RequestHead head{
                .method = fiber::http::HttpMethod::Get, .scheme = "https", .authority = "example.com", .path = "/"};
        EXPECT_TRUE(co_await exchange.send_request_header(head, true));
        EXPECT_TRUE(handle.accepting_requests());

        const auto drain_started = std::chrono::steady_clock::now();
        handle.graceful_shutdown();
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

        co_await handle.wait_closed();
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
    options.loop = &group.at(0);
    options.role = fiber::quic::QuicConnectionRole::Client;
    options.original_destination_connection_id = connection_id_from({1, 2, 3, 4});
    options.remote_connection_id = connection_id_from({5, 6, 7, 8});
    ClientFixture fixture(options);
    auto &h3 = fixture.connection();
    ASSERT_TRUE(start_h3_on_loop(group.at(0), h3.quic(), options, h3).ok);
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&h3, peer_goaway, &done]() -> fiber::async::DetachedTask {
        auto handle = h3.make_handle(h3.quic().lease());
        fiber::mem::BufPool pool;
        auto exchange = handle.open_exchange(pool);
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
            handle.graceful_shutdown();
        }
        co_await sent.join();
        EXPECT_FALSE(handle.accepting_requests());
        EXPECT_FALSE(h3.local_stream_gate().has_waiters());
        handle.graceful_shutdown();
        co_await handle.wait_closed();
        EXPECT_TRUE(handle.valid());
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

TEST(Http3ClientConnectionTest, MovingHandlePreservesUnsentAndAttachedExchanges) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    auto options = fiber::test::quic_options();
    options.loop = &group.at(0);
    options.role = fiber::quic::QuicConnectionRole::Client;
    options.original_destination_connection_id = connection_id_from({1, 2, 3, 4});
    options.remote_connection_id = connection_id_from({5, 6, 7, 8});
    ClientFixture fixture(options);
    auto &h3 = fixture.connection();
    ASSERT_TRUE(start_h3_on_loop(group.at(0), h3.quic(), options, h3).ok);
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&h3, &done]() -> fiber::async::DetachedTask {
        fiber::mem::BufPool pool;
        auto original = h3.make_handle(h3.quic().lease());
        auto unsent = original.open_exchange(pool);
        auto attached = original.open_exchange(pool);
        fiber::http::Http3RequestHead head{
                .method = fiber::http::HttpMethod::Get, .scheme = "https", .authority = "example.com", .path = "/"};
        EXPECT_TRUE(co_await attached.send_request_header(head, true));
        auto moved = std::move(original);
        EXPECT_FALSE(original.valid());
        original = {};
        EXPECT_TRUE(moved.accepting_requests());
        EXPECT_EQ(&moved.quic(), &h3.quic());
        EXPECT_TRUE(co_await unsent.send_request_header(head, true));
        EXPECT_NE(attached.stream_id(), unsent.stream_id());
        auto response = headers_frame({{":status", "200"}, {"content-length", "0"}});
        feed_stream(h3.quic(), attached.stream_id(), response, true);
        EXPECT_TRUE(co_await attached.read_header(1s));
        auto body = co_await attached.read_body(1024, 1s);
        EXPECT_TRUE(body);
        if (body) {
            EXPECT_TRUE(body->complete());
        }
        EXPECT_EQ(attached.outcome(), fiber::http::Http3RequestOutcome::Complete);
        (void) unsent.abort();
        moved.shutdown();
        co_await moved.wait_closed();
        EXPECT_TRUE(moved.valid());
        done.set_value();
    });
    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    fixture.finish();
    group.stop();
    group.join();
}

TEST(Http3ClientConnectionTest, MoveAssignmentClosesPreviousConnection) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    auto options = fiber::test::quic_options();
    options.loop = &group.at(0);
    options.role = fiber::quic::QuicConnectionRole::Client;
    options.original_destination_connection_id = connection_id_from({1, 2, 3, 4});
    options.remote_connection_id = connection_id_from({5, 6, 7, 8});
    ClientFixture first(options), second(options);
    ASSERT_TRUE(start_h3_on_loop(group.at(0), first.connection().quic(), options, first.connection()).ok);
    ASSERT_TRUE(start_h3_on_loop(group.at(0), second.connection().quic(), options, second.connection()).ok);
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        auto destination = first.connection().make_handle(first.connection().quic().lease());
        auto source = second.connection().make_handle(second.connection().quic().lease());
        destination = std::move(source);
        EXPECT_FALSE(source.valid());
        EXPECT_TRUE(first.connection().quic().closing());
        EXPECT_EQ(&destination.quic(), &second.connection().quic());
        EXPECT_TRUE(destination.accepting_requests());
        destination.shutdown();
        co_await destination.wait_closed();
        done.set_value();
    });
    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    first.finish();
    second.finish();
    group.stop();
    group.join();
}

TEST(Http3ClientConnectionTest, RunningWaitRetainsConnectionWhenHandleIsResetDuringStartup) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    auto options = fiber::test::quic_options();
    options.loop = &group.at(0);
    options.role = fiber::quic::QuicConnectionRole::Client;
    options.original_destination_connection_id = connection_id_from({1, 2, 3, 4});
    options.remote_connection_id = connection_id_from({5, 6, 7, 8});
    ClientFixture fixture(options);
    auto &h3 = fixture.connection();
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        auto params = valid_peer_transport_params(options);
        params.initial_max_streams_uni = 0;
        params.has_original_destination_connection_id = true;
        params.original_destination_connection_id = options.original_destination_connection_id;
        EXPECT_TRUE(h3.quic().adopt_server_initial_source_connection_id(options.remote_connection_id));
        EXPECT_TRUE(h3.quic().apply_peer_transport_params(params));
        EXPECT_TRUE(h3.quic().mark_established());
        fiber::async::WaitGroup tasks;
        tasks.add(2);
        auto handle = h3.make_handle(h3.quic().lease());
        fiber::async::spawn(fiber::event::EventLoop::current(), [&]() -> fiber::async::DetachedTask {
            auto started = co_await h3.start();
            EXPECT_FALSE(started);
            tasks.done();
        });
        co_await fiber::async::sleep(1ms);
        EXPECT_EQ(h3.local_stream_gate().waiter_count(), 1U);
        const auto before_wait = h3.quic().ref_count();
        fiber::async::spawn(fiber::event::EventLoop::current(), [&]() -> fiber::async::DetachedTask {
            co_await handle.wait_closed();
            EXPECT_EQ(h3.state(), fiber::http::Http3ConnectionState::Closed);
            tasks.done();
        });
        co_await fiber::async::sleep(1ms);
        EXPECT_EQ(h3.quic().ref_count(), before_wait + 1);
        handle = {};
        EXPECT_FALSE(handle.valid());
        EXPECT_EQ(h3.quic().ref_count(), before_wait);
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
