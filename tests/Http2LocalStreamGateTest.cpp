#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include <fiber/common/IoError.h>
// EventLoop must be fully included before the `private public` hack below:
// reparsing it with private members exposed turns internal entries into
// aggregates whose NonCopyable base cannot be initialized.
#include <fiber/event/EventLoop.h>

#define private public
#include <fiber/http/Http2Connection.h>
#undef private

#include <fiber/http/Http2LocalStreamGate.h>
#include "Http2TestSupport.h"

// The queueing side of the gate is covered by the AwaitedLocalStreamAttach and
// ClientExchange tests in Http2ConnectionTest.cpp, which need a running loop and
// a transport. These cover the parts that need neither.
namespace {

struct ChainedCapacityObserver {
    std::size_t calls = 0;
    std::size_t last_slots = 0;

    static void on_capacity(void *ctx, fiber::http::Http2Connection &connection) noexcept {
        auto *self = static_cast<ChainedCapacityObserver *>(ctx);
        ++self->calls;
        self->last_slots = connection.available_local_stream_slots();
    }
};

fiber::http::Http2Connection::Options client_options(std::uint32_t peer_streams) noexcept {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.max_peer_concurrent_streams = peer_streams;
    return options;
}

} // namespace

TEST(Http2LocalStreamGateTest, TryAttachForwardsToTheConnectionWhileNobodyIsQueued) {
    fiber::http::Http2Connection connection(client_options(1), &test_http2_stream_factory(),
                                            TestHttp2StreamFactory::ops());
    connection.state_ = fiber::http::Http2Connection::State::Running;
    fiber::http::Http2LocalStreamGate gate(connection);

    auto *owner1 = TestHttp2StreamOwner::create_owner();
    auto *owner3 = TestHttp2StreamOwner::create_owner();
    ASSERT_NE(owner1, nullptr);
    ASSERT_NE(owner3, nullptr);

    auto stream1 = gate.try_attach(owner1->stream);
    ASSERT_TRUE(stream1.has_value());
    EXPECT_EQ((*stream1)->stream_id(), 1U);
    EXPECT_EQ(gate.waiter_count(), 0u);

    auto blocked = gate.try_attach(owner3->stream);
    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(blocked.error(), fiber::common::IoErr::Busy);

    (*stream1)->close(fiber::common::IoErr::Canceled);
    connection.try_release_stream(**stream1);
    stream1->reset();

    auto stream3 = gate.try_attach(owner3->stream);
    ASSERT_TRUE(stream3.has_value());
    EXPECT_EQ((*stream3)->stream_id(), 3U);

    connection.close_all_streams(fiber::common::IoErr::Canceled);
}

TEST(Http2LocalStreamGateTest, TryAttachReportsTheConnectionsTerminalStatusAfterPeerGoaway) {
    fiber::http::Http2Connection connection(client_options(4), &test_http2_stream_factory(),
                                            TestHttp2StreamFactory::ops());
    connection.state_ = fiber::http::Http2Connection::State::Running;
    // No transport here: pretend our own GOAWAY already went out, and keep one
    // stream attached so the connection stays in Draining instead of pumping.
    connection.local_goaway_sent_ = true;
    fiber::http::Http2LocalStreamGate gate(connection);

    auto *owner1 = TestHttp2StreamOwner::create_owner();
    auto *owner3 = TestHttp2StreamOwner::create_owner();
    ASSERT_NE(owner1, nullptr);
    ASSERT_NE(owner3, nullptr);
    auto stream1 = gate.try_attach(owner1->stream);
    ASSERT_TRUE(stream1.has_value());

    connection.handle_peer_goaway(1, fiber::http::Http2ErrorCode::NoError);

    auto refused = gate.try_attach(owner3->stream);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), fiber::common::IoErr::Canceled);
    delete owner3;
}

TEST(Http2LocalStreamGateTest, ChainedCapacityCallbackSeesEveryConnectionCapacityChange) {
    ChainedCapacityObserver observer;
    fiber::http::Http2Connection connection(client_options(2), &test_http2_stream_factory(),
                                            TestHttp2StreamFactory::ops());
    connection.state_ = fiber::http::Http2Connection::State::Running;
    fiber::http::Http2LocalStreamGate gate(connection);
    gate.set_capacity_callback(&ChainedCapacityObserver::on_capacity, &observer);

    ASSERT_EQ(connection.apply_settings_parameter(0x3, 4), fiber::common::IoErr::None);
    EXPECT_EQ(observer.calls, 1u);
    EXPECT_EQ(observer.last_slots, 4u);

    auto *owner1 = TestHttp2StreamOwner::create_owner();
    ASSERT_NE(owner1, nullptr);
    auto stream1 = gate.try_attach(owner1->stream);
    ASSERT_TRUE(stream1.has_value());
    // Attaching is the caller's own doing, so it raises no capacity change.
    EXPECT_EQ(observer.calls, 1u);

    (*stream1)->close(fiber::common::IoErr::Canceled);
    connection.try_release_stream(**stream1);
    stream1->reset();
    EXPECT_EQ(observer.calls, 2u);
    EXPECT_EQ(observer.last_slots, 4u);

    gate.clear_capacity_callback();
    ASSERT_EQ(connection.apply_settings_parameter(0x3, 1), fiber::common::IoErr::None);
    EXPECT_EQ(observer.calls, 2u);

    connection.close_all_streams(fiber::common::IoErr::Canceled);
}
