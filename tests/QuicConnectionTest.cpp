#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <new>
#include <string>
#include <string_view>
#include <thread>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "event/EventLoopGroup.h"
#include "quic/QuicAckHandler.h"
#include "quic/QuicConnection.h"
#include "quic/QuicLossRecovery.h"
#include "quic/QuicProtocol.h"
#include "quic/QuicTransportCodec.h"
#include "quic/QuicTransportParamsCodec.h"

#include "QuicTestLoop.h"

namespace {

fiber::net::SocketAddress loopback(std::uint16_t port) { return {fiber::net::IpAddress::loopback_v4(), port}; }

fiber::net::SocketAddress v4_addr(std::array<std::uint8_t, 4> ip, std::uint16_t port) {
    return {fiber::net::IpAddress::v4(ip), port};
}

fiber::quic::QuicConnectionId cid_from(std::initializer_list<std::uint8_t> bytes) {
    auto cid = fiber::quic::QuicConnectionId::from_bytes(bytes.begin(), bytes.size());
    return cid.value_or(fiber::quic::QuicConnectionId{});
}

fiber::quic::QuicTransportParams valid_server_peer_params(const fiber::quic::QuicConnection::Options &options) {
    fiber::quic::QuicTransportParams params{};
    params.has_initial_source_connection_id = true;
    params.initial_source_connection_id = options.remote_connection_id;
    params.max_udp_payload_size = fiber::quic::kMinInitialDatagramSize;
    params.active_connection_id_limit = 2;
    return params;
}

fiber::quic::QuicSlice slice_of(std::string_view value) {
    return {reinterpret_cast<const std::uint8_t *>(value.data()), value.size()};
}

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

struct StreamCallbackState {
    std::uint32_t calls = 0;
    std::uint64_t last_stream_id = 0;
    bool return_empty = false;
    fiber::quic::QuicStream::Lease lease{};
};

struct IdleTimerSnapshot {
    bool send_timer_after_send = false;
    bool idle_armed_after_send = false;
    bool send_timer_after_receive = true;
    bool idle_armed_after_receive = false;
};

struct KeepaliveTimerSnapshot {
    std::size_t pending_ping_count = 0;
};

std::size_t count_pending_frame_type(const fiber::quic::QuicConnection &conn, fiber::quic::QuicFrameType type);

void destroy_test_stream(void *, fiber::quic::QuicStream &stream) noexcept { delete &stream; }

fiber::quic::QuicStream::Lease make_test_stream() noexcept {
    return fiber::quic::QuicStream::Lease::adopt(new (std::nothrow)
                                                         fiber::quic::QuicStream(nullptr, destroy_test_stream));
}

fiber::quic::QuicStream::Lease create_stream_record(void *owner) noexcept {
    auto *state = static_cast<StreamCallbackState *>(owner);
    ++state->calls;
    if (state->return_empty) {
        return {};
    }
    return make_test_stream();
}

fiber::quic::QuicStream::Lease create_stream_retain(void *owner) noexcept {
    auto *state = static_cast<StreamCallbackState *>(owner);
    ++state->calls;
    auto *stream = new (std::nothrow) fiber::quic::QuicStream(nullptr, destroy_test_stream);
    if (stream == nullptr) {
        return {};
    }
    return fiber::quic::QuicStream::Lease::adopt(stream);
}

void on_peer_stream_attached_record(void *owner, fiber::quic::QuicStream &stream) noexcept {
    auto *state = static_cast<StreamCallbackState *>(owner);
    state->last_stream_id = stream.stream_id();
}

void on_peer_stream_attached_retain(void *owner, fiber::quic::QuicStream &stream) noexcept {
    auto *state = static_cast<StreamCallbackState *>(owner);
    state->last_stream_id = stream.stream_id();
    state->lease = stream.lease();
}

fiber::quic::QuicConnection::Options server_options_with_factory(StreamCallbackState &state) noexcept {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Server;
    options.owner = &state;
    options.ops.create_stream = create_stream_record;
    options.ops.on_peer_stream_attached = on_peer_stream_attached_record;
    return options;
}

struct WriteResult {
    bool ok = false;
    std::size_t value = 0;
    fiber::common::IoErr error = fiber::common::IoErr::None;
};

struct AttachResult {
    bool ok = false;
    std::uint64_t stream_id = 0;
    fiber::common::IoErr error = fiber::common::IoErr::None;
};

WriteResult to_write_result(fiber::common::IoResult<std::size_t> result) {
    if (result) {
        return {.ok = true, .value = *result};
    }
    return {.ok = false, .error = result.error()};
}

AttachResult to_attach_result(fiber::common::IoResult<fiber::quic::QuicStream *> result) {
    if (result) {
        return {.ok = true, .stream_id = (*result)->stream_id()};
    }
    return {.ok = false, .error = result.error()};
}

fiber::async::DetachedTask attach_local_stream(fiber::quic::QuicConnection *conn, fiber::quic::QuicStream::Lease stream,
                                               fiber::quic::QuicStreamType type, std::chrono::milliseconds timeout,
                                               std::promise<AttachResult> *done) {
    auto result = co_await conn->attach_local_stream(std::move(stream), type, timeout);
    done->set_value(to_attach_result(result));
    fiber::event::EventLoop::current().stop();
}

fiber::async::DetachedTask grant_max_streams_after_delay(fiber::quic::QuicConnection *conn,
                                                         fiber::quic::QuicStreamType type, std::uint64_t limit,
                                                         std::atomic<bool> *started) {
    co_await fiber::async::sleep(std::chrono::milliseconds(20));
    started->store(true, std::memory_order_relaxed);
    fiber::quic::QuicMaxStreamsFrame frame{};
    frame.bidirectional = type == fiber::quic::QuicStreamType::Bidirectional;
    frame.limit = limit;
    (void) conn->recv_max_streams_frame(frame);
}

fiber::async::DetachedTask write_one(fiber::quic::QuicStream *stream, std::promise<WriteResult> *done) {
    auto result = co_await stream->write(iobuf_of("!"));
    done->set_value(to_write_result(result));
    fiber::event::EventLoop::current().stop();
}

fiber::async::DetachedTask write_one_with_timeout(fiber::quic::QuicStream *stream, std::chrono::milliseconds timeout,
                                                  std::promise<WriteResult> *done) {
    auto result = co_await stream->write(iobuf_of("!"), false, timeout);
    done->set_value(to_write_result(result));
    fiber::event::EventLoop::current().stop();
}

fiber::async::DetachedTask grant_max_stream_data_after_delay(fiber::quic::QuicConnection *conn, std::uint64_t stream_id,
                                                             std::uint64_t limit, std::atomic<bool> *started) {
    co_await fiber::async::sleep(std::chrono::milliseconds(20));
    started->store(true, std::memory_order_relaxed);
    fiber::quic::QuicMaxStreamDataFrame frame{};
    frame.id = stream_id;
    frame.limit = limit;
    (void) conn->recv_max_stream_data_frame(frame);
}

fiber::async::DetachedTask grant_max_data_after_delay(fiber::quic::QuicConnection *conn, std::uint64_t limit,
                                                      std::atomic<bool> *started) {
    co_await fiber::async::sleep(std::chrono::milliseconds(20));
    started->store(true, std::memory_order_relaxed);
    fiber::quic::QuicMaxDataFrame frame{};
    frame.max_data = limit;
    (void) conn->recv_max_data_frame(frame);
}

fiber::async::DetachedTask record_idle_timer_lifecycle(fiber::quic::QuicConnection *conn,
                                                       std::promise<IdleTimerSnapshot> *done) {
    fiber::event::EventLoop &loop = fiber::event::EventLoop::current();
    conn->on_ack_eliciting_packet_sent(loop);
    IdleTimerSnapshot snapshot{};
    snapshot.send_timer_after_send = conn->idle_send_timer_set();
    snapshot.idle_armed_after_send = conn->idle_timer_armed();
    conn->on_packet_processed(loop);
    snapshot.send_timer_after_receive = conn->idle_send_timer_set();
    snapshot.idle_armed_after_receive = conn->idle_timer_armed();
    conn->cancel_idle_timer(loop);
    conn->cancel_keepalive_timer(loop);
    done->set_value(snapshot);
    loop.stop();
    co_return;
}

fiber::async::DetachedTask run_idle_timeout(fiber::quic::QuicConnection *conn, std::chrono::milliseconds delay,
                                            std::promise<fiber::quic::QuicConnectionState> *done) {
    conn->arm_idle_timer(fiber::event::EventLoop::current());
    co_await fiber::async::sleep(delay);
    done->set_value(conn->state());
    fiber::event::EventLoop::current().stop();
}

fiber::async::DetachedTask run_keepalive_timer(fiber::quic::QuicConnection *conn, std::chrono::milliseconds delay,
                                               std::promise<KeepaliveTimerSnapshot> *done) {
    conn->arm_keepalive_timer(fiber::event::EventLoop::current());
    co_await fiber::async::sleep(delay);
    KeepaliveTimerSnapshot snapshot{};
    snapshot.pending_ping_count = count_pending_frame_type(*conn, fiber::quic::QuicFrameType::Ping);
    done->set_value(snapshot);
    fiber::event::EventLoop::current().stop();
}

void grant_max_stream_data(fiber::quic::QuicConnection &conn, std::uint64_t stream_id, std::uint64_t limit) {
    fiber::quic::QuicMaxStreamDataFrame frame{};
    frame.id = stream_id;
    frame.limit = limit;
    ASSERT_TRUE(conn.recv_max_stream_data_frame(frame).has_value());
}

void grant_max_data(fiber::quic::QuicConnection &conn, std::uint64_t limit) {
    fiber::quic::QuicMaxDataFrame frame{};
    frame.max_data = limit;
    ASSERT_TRUE(conn.recv_max_data_frame(frame).has_value());
}

std::size_t count_pending_frame_type(const fiber::quic::QuicConnection &conn, fiber::quic::QuicFrameType type) {
    const auto &space = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    std::size_t count = 0;
    for (const fiber::quic::QuicOutputFrame *frame = space.pending_frames.front(); frame != nullptr;
         frame = space.pending_frames.next_of(*frame)) {
        if (frame->type == type) {
            ++count;
        }
    }
    return count;
}

std::size_t count_path_pending_frame_type(const fiber::quic::QuicPath &path, fiber::quic::QuicFrameType type) {
    std::size_t count = 0;
    for (const fiber::quic::QuicOutputFrame *frame = path.pending_frames.front(); frame != nullptr;
         frame = path.pending_frames.next_of(*frame)) {
        if (frame->type == type) {
            ++count;
        }
    }
    return count;
}

std::size_t count_pending_data_blocked(const fiber::quic::QuicConnection &conn, std::uint64_t limit) {
    const auto &space = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    std::size_t count = 0;
    for (const fiber::quic::QuicOutputFrame *frame = space.pending_frames.front(); frame != nullptr;
         frame = space.pending_frames.next_of(*frame)) {
        if (frame->type == fiber::quic::QuicFrameType::DataBlocked && frame->u.data_blocked.limit == limit) {
            ++count;
        }
    }
    return count;
}

std::size_t count_pending_stream_data_blocked(const fiber::quic::QuicConnection &conn, std::uint64_t stream_id,
                                              std::uint64_t limit) {
    const auto &space = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    std::size_t count = 0;
    for (const fiber::quic::QuicOutputFrame *frame = space.pending_frames.front(); frame != nullptr;
         frame = space.pending_frames.next_of(*frame)) {
        if (frame->type == fiber::quic::QuicFrameType::StreamDataBlocked &&
            frame->u.stream_data_blocked.id == stream_id && frame->u.stream_data_blocked.limit == limit) {
            ++count;
        }
    }
    return count;
}

std::size_t count_pending_max_streams(const fiber::quic::QuicConnection &conn, fiber::quic::QuicFrameType type,
                                      std::uint64_t limit) {
    const auto &space = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    std::size_t count = 0;
    for (const fiber::quic::QuicOutputFrame *frame = space.pending_frames.front(); frame != nullptr;
         frame = space.pending_frames.next_of(*frame)) {
        if (frame->type == type && frame->u.max_streams.limit == limit) {
            ++count;
        }
    }
    return count;
}

std::size_t count_pending_streams_blocked(const fiber::quic::QuicConnection &conn, fiber::quic::QuicFrameType type,
                                          std::uint64_t limit) {
    const auto &space = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    std::size_t count = 0;
    for (const fiber::quic::QuicOutputFrame *frame = space.pending_frames.front(); frame != nullptr;
         frame = space.pending_frames.next_of(*frame)) {
        if (frame->type == type && frame->u.streams_blocked.limit == limit) {
            ++count;
        }
    }
    return count;
}

} // namespace

TEST(QuicConnectionTest, BuildsConnectionIdFromBytes) {
    const std::array<std::uint8_t, 4> bytes{0x01, 0x02, 0x03, 0x04};

    auto conn_id = fiber::quic::QuicConnectionId::from_bytes(bytes.data(), bytes.size());

    ASSERT_TRUE(conn_id.has_value());
    EXPECT_EQ(conn_id->size(), bytes.size());
    EXPECT_EQ(conn_id->data()[0], 0x01);
    EXPECT_EQ(conn_id->data()[3], 0x04);
}

TEST(QuicConnectionTest, AllocatesClientInitiatedStreamIds) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Client;
    options.max_local_bidirectional_streams = 2;
    fiber::quic::QuicConnection conn(options);

    auto first = conn.next_local_stream_id(fiber::quic::QuicStreamType::Bidirectional);
    auto second = conn.next_local_stream_id(fiber::quic::QuicStreamType::Bidirectional);
    auto third = conn.next_local_stream_id(fiber::quic::QuicStreamType::Bidirectional);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, 0U);
    EXPECT_EQ(*second, 4U);
    EXPECT_FALSE(third.has_value());
}

TEST(QuicConnectionTest, AllocatesServerInitiatedUnidirectionalStreamIds) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Server;
    fiber::quic::QuicConnection conn(options);

    auto stream_id = conn.next_local_stream_id(fiber::quic::QuicStreamType::Unidirectional);

    ASSERT_TRUE(stream_id.has_value());
    EXPECT_EQ(*stream_id, 3U);
    EXPECT_TRUE(fiber::quic::QuicConnection::is_unidirectional_stream(*stream_id));
    EXPECT_TRUE(conn.is_local_stream(*stream_id));
}

TEST(QuicConnectionTest, TryAttachLocalStreamAssignsClientBidirectionalStream) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Client;
    options.max_local_bidirectional_streams = 2;
    fiber::quic::QuicConnection conn(options);
    ASSERT_TRUE(conn.mark_established());

    auto stream = make_test_stream();
    ASSERT_TRUE(stream);

    auto attached = conn.try_attach_local_stream(std::move(stream), fiber::quic::QuicStreamType::Bidirectional);

    ASSERT_TRUE(attached.has_value()) << static_cast<int>(attached.error());
    EXPECT_FALSE(stream);
    EXPECT_EQ((*attached)->stream_id(), 0U);
    EXPECT_TRUE((*attached)->stream_id_assigned());
    EXPECT_EQ(conn.find_stream(0), *attached);
    EXPECT_EQ(conn.active_stream_count(), 1U);
}

TEST(QuicConnectionTest, TryAttachLocalStreamReturnsBusyBeforeEstablished) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Client;
    fiber::quic::QuicConnection conn(options);
    auto stream = make_test_stream();
    ASSERT_TRUE(stream);

    auto attached = conn.try_attach_local_stream(std::move(stream), fiber::quic::QuicStreamType::Bidirectional);

    ASSERT_FALSE(attached.has_value());
    EXPECT_EQ(attached.error(), fiber::common::IoErr::Busy);
    ASSERT_TRUE(stream);
    EXPECT_FALSE(stream->stream_id_assigned());
    EXPECT_EQ(conn.active_stream_count(), 0U);
}

TEST(QuicConnectionTest, TryAttachLocalStreamQueuesStreamsBlockedAtLimit) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Client;
    options.max_local_bidirectional_streams = 0;
    fiber::quic::QuicConnection conn(options);
    ASSERT_TRUE(conn.mark_established());
    auto stream = make_test_stream();
    ASSERT_TRUE(stream);

    auto attached = conn.try_attach_local_stream(std::move(stream), fiber::quic::QuicStreamType::Bidirectional);

    ASSERT_FALSE(attached.has_value());
    EXPECT_EQ(attached.error(), fiber::common::IoErr::Busy);
    ASSERT_TRUE(stream);
    EXPECT_FALSE(stream->stream_id_assigned());
    EXPECT_EQ(conn.active_stream_count(), 0U);
    EXPECT_EQ(count_pending_streams_blocked(conn, fiber::quic::QuicFrameType::StreamsBlockedBidi, 0), 1U);
}

TEST(QuicConnectionTest, AttachLocalStreamResumesAfterMaxStreams) {
    fiber::event::EventLoopGroup group(1);
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Client;
    options.max_local_bidirectional_streams = 0;
    options.loop = &group.at(0);
    fiber::quic::QuicConnection conn(options);
    ASSERT_TRUE(conn.mark_established());

    std::promise<AttachResult> done;
    auto future = done.get_future();
    std::atomic<bool> grant_seen{false};
    auto stream = make_test_stream();
    ASSERT_TRUE(stream);

    group.start();
    fiber::async::spawn(group.at(0), [&conn, stream = std::move(stream), &done]() mutable {
        return attach_local_stream(&conn, std::move(stream), fiber::quic::QuicStreamType::Bidirectional,
                                   std::chrono::seconds(1), &done);
    });
    fiber::async::spawn(group.at(0), [&conn, &grant_seen]() {
        return grant_max_streams_after_delay(&conn, fiber::quic::QuicStreamType::Bidirectional, 1, &grant_seen);
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "attach did not resume after MAX_STREAMS";
        return;
    }

    const AttachResult result = future.get();
    EXPECT_TRUE(grant_seen.load(std::memory_order_relaxed));
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.stream_id, 0U);
    EXPECT_NE(conn.find_stream(0), nullptr);
    EXPECT_EQ(conn.active_stream_count(), 1U);
    EXPECT_EQ(count_pending_streams_blocked(conn, fiber::quic::QuicFrameType::StreamsBlockedBidi, 0), 1U);
    group.join();
}

TEST(QuicConnectionTest, InitializesThreePacketNumberSpaces) {
    fiber::quic::QuicConnection conn(fiber::test::quic_options());

    auto &initial = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial);
    auto &handshake = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Handshake);
    auto &application = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);

    EXPECT_EQ(initial.level, fiber::quic::QuicEncryptionLevel::Initial);
    EXPECT_EQ(handshake.level, fiber::quic::QuicEncryptionLevel::Handshake);
    EXPECT_EQ(application.level, fiber::quic::QuicEncryptionLevel::Application);
    EXPECT_EQ(initial.next_packet_number, 0U);
    EXPECT_EQ(handshake.next_packet_number, 0U);
    EXPECT_EQ(application.next_packet_number, 0U);
    EXPECT_EQ(initial.largest_received_packet_number, fiber::quic::kUnsetPacketNumber);
    EXPECT_EQ(handshake.largest_acked_packet_number, fiber::quic::kUnsetPacketNumber);
    EXPECT_EQ(application.pending_ack, fiber::quic::kUnsetPacketNumber);
}

TEST(QuicConnectionTest, MapsEarlyDataToApplicationPacketNumberSpace) {
    fiber::quic::QuicConnection conn(fiber::test::quic_options());

    auto &early = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::EarlyData);
    auto &application = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);

    EXPECT_EQ(&early, &application);
    EXPECT_EQ(fiber::quic::QuicConnection::packet_number_space_index(fiber::quic::QuicEncryptionLevel::Initial), 0U);
    EXPECT_EQ(fiber::quic::QuicConnection::packet_number_space_index(fiber::quic::QuicEncryptionLevel::Handshake), 1U);
    EXPECT_EQ(fiber::quic::QuicConnection::packet_number_space_index(fiber::quic::QuicEncryptionLevel::EarlyData), 2U);
    EXPECT_EQ(fiber::quic::QuicConnection::packet_number_space_index(fiber::quic::QuicEncryptionLevel::Application),
              2U);
}

TEST(QuicConnectionTest, AdvancesPacketNumbersIndependentlyPerSpace) {
    fiber::quic::QuicConnection conn(fiber::test::quic_options());

    auto &initial = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial);
    auto &handshake = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Handshake);

    EXPECT_EQ(fiber::quic::quic_use_next_packet_number(initial), 0U);
    EXPECT_EQ(fiber::quic::quic_use_next_packet_number(initial), 1U);
    EXPECT_EQ(fiber::quic::quic_use_next_packet_number(handshake), 0U);
    EXPECT_EQ(initial.next_packet_number, 2U);
    EXPECT_EQ(handshake.next_packet_number, 1U);
}

TEST(QuicConnectionTest, QueuesFramesIntrusively) {
    fiber::quic::QuicConnection conn(fiber::test::quic_options());
    auto &space = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial);
    fiber::quic::QuicOutputFrame *first = space.alloc_frame();
    fiber::quic::QuicOutputFrame *second = space.alloc_frame();
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    space.pending_frames.push_back(*first);
    space.pending_frames.push_back(*second);

    EXPECT_FALSE(space.pending_frames.empty());
    EXPECT_EQ(space.pending_frames.front(), first);
    EXPECT_EQ(space.pending_frames.back(), second);

    space.pending_frames.erase(*first);
    space.release_frame(*first);

    EXPECT_EQ(space.pending_frames.front(), second);
    EXPECT_EQ(space.pending_frames.back(), second);
}

TEST(QuicConnectionTest, StreamWriteSubmitsConnectionSendWork) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    fiber::quic::QuicConnection conn(options);

    auto stream = conn.get_or_create_peer_stream(0);
    ASSERT_TRUE(stream.has_value());
    fiber::quic::QuicMaxStreamDataFrame limit{};
    limit.id = 0;
    limit.limit = 1024;
    ASSERT_TRUE(conn.recv_max_stream_data_frame(limit).has_value());
    grant_max_data(conn, 1024);

    auto written = (*stream)->try_write(iobuf_of("abc"));

    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(*written, 3u);
    auto &space = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    ASSERT_FALSE(space.pending_frames.empty());
    ASSERT_NE(space.pending_frames.front(), nullptr);
    EXPECT_EQ(space.pending_frames.front()->type, fiber::quic::QuicFrameType::Stream);
    EXPECT_EQ(space.pending_frames.front()->u.stream.stream_id, 0u);
}

TEST(QuicConnectionTest, RecvFlowDefaultsInitializeLocalTransportAndLimit) {
    fiber::quic::QuicConnection conn(fiber::test::quic_options());

    EXPECT_EQ(conn.recv_data_limit(), fiber::quic::kQuicDefaultConnRecvLimit);
    EXPECT_EQ(conn.local_transport().initial_max_data, fiber::quic::kQuicDefaultConnRecvLimit);
    EXPECT_EQ(conn.local_transport().initial_max_stream_data_bidi_local, fiber::quic::kQuicDefaultStreamBufferSize);
    EXPECT_EQ(conn.local_transport().initial_max_stream_data_bidi_remote, fiber::quic::kQuicDefaultStreamBufferSize);
    EXPECT_EQ(conn.local_transport().initial_max_stream_data_uni, fiber::quic::kQuicDefaultStreamBufferSize);
    EXPECT_EQ(conn.local_transport().initial_max_streams_bidi, fiber::quic::kQuicDefaultMaxBidirectionalStreams);
    EXPECT_EQ(conn.local_transport().initial_max_streams_uni, fiber::quic::kQuicDefaultMaxUnidirectionalStreams);
}

TEST(QuicConnectionTest, CreatesInitialActivePathFromOptions) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.local_addr = loopback(4433);
    options.remote_addr = loopback(5555);
    options.remote_connection_id = cid_from({0x01, 0x02, 0x03, 0x04});

    fiber::quic::QuicConnection conn(options);

    auto *path = conn.active_path();
    ASSERT_NE(path, nullptr);
    EXPECT_EQ(conn.path_count(), 1U);
    EXPECT_EQ(path->tag, fiber::quic::QuicPathTag::Active);
    EXPECT_EQ(path->remote.port(), 5555);
    EXPECT_EQ(path->local.port(), 4433);
    EXPECT_EQ(path->remote_connection_id.size(), options.remote_connection_id.size());
    EXPECT_EQ(conn.remote_addr().port(), 5555);
}

TEST(QuicConnectionTest, TracksPathReceiveSendAndAntiAmplificationLimit) {
    fiber::quic::QuicConnection conn(fiber::test::quic_options());
    auto *path = conn.active_path();
    ASSERT_NE(path, nullptr);

    EXPECT_EQ(fiber::quic::QuicConnection::path_send_limit(*path, 1200), 0U);

    conn.record_path_received(*path, 400);
    EXPECT_EQ(fiber::quic::QuicConnection::path_send_limit(*path, 2000), 1200U);

    conn.record_path_sent(*path, 500);
    EXPECT_EQ(fiber::quic::QuicConnection::path_send_limit(*path, 2000), 700U);

    path->validated = true;
    EXPECT_EQ(fiber::quic::QuicConnection::path_send_limit(*path, 2000), 2000U);
}

TEST(QuicConnectionTest, PeerTransportStartsMtuDiscoveryOnValidatedPath) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Server;
    options.remote_connection_id = cid_from({0x11, 0x22});
    options.initial_path_validated = true;
    fiber::quic::QuicConnection conn(options);
    auto *path = conn.active_path();
    ASSERT_NE(path, nullptr);

    fiber::quic::QuicTransportParams params{};
    params.has_initial_source_connection_id = true;
    params.initial_source_connection_id = options.remote_connection_id;
    params.max_udp_payload_size = 2400;
    params.active_connection_id_limit = 2;

    auto applied = conn.apply_peer_transport_params(params);

    ASSERT_TRUE(applied.has_value()) << static_cast<int>(applied.error());
    EXPECT_EQ(path->state, fiber::quic::QuicPathState::WaitingMtuProbe);
    EXPECT_EQ(path->mtud, 2400U);
    EXPECT_EQ(path->max_mtu, 2400U);
    EXPECT_EQ(path->expires, fiber::quic::QuicTime{100});
}

TEST(QuicConnectionTest, MtuDelayQueuesPingProbe) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Server;
    options.remote_connection_id = cid_from({0x11, 0x22});
    options.initial_path_validated = true;
    fiber::quic::QuicConnection conn(options);
    auto *path = conn.active_path();
    ASSERT_NE(path, nullptr);

    path->state = fiber::quic::QuicPathState::WaitingMtuProbe;
    path->mtud = 2400;

    auto expired = conn.paths().expire_mtu_delay(*path, fiber::quic::QuicTime{100});

    ASSERT_TRUE(expired.has_value()) << static_cast<int>(expired.error());
    EXPECT_TRUE(*expired);
    EXPECT_EQ(path->state, fiber::quic::QuicPathState::MtuDiscovery);
    ASSERT_FALSE(path->pending_frames.empty());
    const auto *probe = path->pending_frames.front();
    ASSERT_NE(probe, nullptr);
    EXPECT_EQ(probe->type, fiber::quic::QuicFrameType::Ping);
    EXPECT_TRUE(probe->mtu_probe);
    EXPECT_TRUE(probe->ignore_loss);
    EXPECT_TRUE(probe->ignore_congestion);
    EXPECT_EQ(probe->min_packet_len, 2400U);
    EXPECT_EQ(probe->path, path);
}

TEST(QuicConnectionTest, MtuAckRaisesPathMtuAndContinuesDiscovery) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Server;
    options.remote_connection_id = cid_from({0x11, 0x22});
    options.initial_path_validated = true;
    fiber::quic::QuicConnection conn(options);
    auto *path = conn.active_path();
    ASSERT_NE(path, nullptr);

    fiber::quic::QuicTransportParams params{};
    params.has_initial_source_connection_id = true;
    params.initial_source_connection_id = options.remote_connection_id;
    params.max_udp_payload_size = 3000;
    params.active_connection_id_limit = 2;
    ASSERT_TRUE(conn.apply_peer_transport_params(params).has_value());

    path->state = fiber::quic::QuicPathState::MtuDiscovery;
    path->mtu = fiber::quic::kMinInitialDatagramSize;
    path->mtud = 2400;
    path->max_mtu = 0;
    path->mtu_packet_numbers[0] = 42;

    auto handled = conn.paths().handle_mtu_ack(40, 50, fiber::quic::QuicTime{200});

    ASSERT_TRUE(handled.has_value()) << static_cast<int>(handled.error());
    EXPECT_TRUE(*handled);
    EXPECT_EQ(path->mtu, 2400U);
    EXPECT_EQ(conn.congestion().mtu, 2400U);
    EXPECT_EQ(path->state, fiber::quic::QuicPathState::WaitingMtuProbe);
    EXPECT_EQ(path->mtud, 3000U);
    EXPECT_EQ(path->max_mtu, 3000U);
}

TEST(QuicConnectionTest, MtuProbeFailureSetsUpperBoundAndBisects) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Server;
    options.remote_connection_id = cid_from({0x11, 0x22});
    options.initial_path_validated = true;
    fiber::quic::QuicConnection conn(options);
    auto *path = conn.active_path();
    ASSERT_NE(path, nullptr);

    fiber::quic::QuicTransportParams params{};
    params.has_initial_source_connection_id = true;
    params.initial_source_connection_id = options.remote_connection_id;
    params.max_udp_payload_size = 3000;
    params.active_connection_id_limit = 2;
    ASSERT_TRUE(conn.apply_peer_transport_params(params).has_value());

    path->state = fiber::quic::QuicPathState::MtuDiscovery;
    path->mtu = fiber::quic::kMinInitialDatagramSize;
    path->mtud = 2400;
    path->max_mtu = 0;

    auto handled = conn.paths().handle_mtu_probe_send_failed(*path, fiber::quic::QuicTime{200});

    ASSERT_TRUE(handled.has_value()) << static_cast<int>(handled.error());
    EXPECT_TRUE(*handled);
    EXPECT_EQ(path->max_mtu, 2400U);
    EXPECT_EQ(path->mtud, 1800U);
    EXPECT_EQ(path->state, fiber::quic::QuicPathState::WaitingMtuProbe);
}

TEST(QuicConnectionTest, AckHandlerPromotesMtuProbePacket) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Server;
    options.remote_connection_id = cid_from({0x11, 0x22});
    options.initial_path_validated = true;
    fiber::quic::QuicConnection conn(options);
    auto *path = conn.active_path();
    ASSERT_NE(path, nullptr);

    fiber::quic::QuicTransportParams params{};
    params.has_initial_source_connection_id = true;
    params.initial_source_connection_id = options.remote_connection_id;
    params.max_udp_payload_size = 2400;
    params.active_connection_id_limit = 2;
    ASSERT_TRUE(conn.apply_peer_transport_params(params).has_value());

    auto &space = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    auto *probe = space.alloc_frame();
    ASSERT_NE(probe, nullptr);
    probe->type = fiber::quic::QuicFrameType::Ping;
    probe->path = path;
    probe->packet_number = 7;
    probe->send_time = fiber::quic::QuicTime{100};
    probe->packet_ack_eliciting = true;
    probe->mtu_probe = true;
    space.sent_frames.push_back(*probe);
    space.next_packet_number = 8;

    path->state = fiber::quic::QuicPathState::MtuDiscovery;
    path->mtu = fiber::quic::kMinInitialDatagramSize;
    path->mtud = 2400;
    path->max_mtu = 2400;
    path->mtu_packet_numbers[0] = 7;

    fiber::quic::QuicInputFrame ack{};
    ack.type = fiber::quic::QuicFrameType::Ack;
    ack.level = fiber::quic::QuicEncryptionLevel::Application;
    ack.u.ack.largest = 7;
    ack.u.ack.first_range = 0;

    auto handled = fiber::quic::quic_handle_ack_frame(conn, fiber::quic::QuicEncryptionLevel::Application, ack,
                                                      fiber::quic::QuicTime{150});

    ASSERT_TRUE(handled.has_value()) << static_cast<int>(handled.error());
    EXPECT_TRUE(handled->acked_frames);
    EXPECT_EQ(path->mtu, 2400U);
    EXPECT_EQ(path->state, fiber::quic::QuicPathState::Idle);
}

TEST(QuicConnectionTest, ReplacesProbePathWhenCreatingAnotherProbe) {
    fiber::quic::QuicConnection conn(fiber::test::quic_options());
    const auto cid = cid_from({0x11, 0x22});

    auto *first = conn.create_path(loopback(6001), loopback(4433), cid, fiber::quic::QuicPathTag::Probe);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(conn.path_count(), 2U);

    if (auto *probe = conn.find_path(fiber::quic::QuicPathTag::Probe)) {
        conn.free_path(*probe);
    }
    auto *second = conn.create_path(loopback(6002), loopback(4433), cid, fiber::quic::QuicPathTag::Probe);

    ASSERT_NE(second, nullptr);
    EXPECT_EQ(conn.path_count(), 2U);
    EXPECT_EQ(second->remote.port(), 6002);
    EXPECT_EQ(conn.find_path(loopback(6001), loopback(4433)), nullptr);
}

TEST(QuicConnectionTest, RecvPathChallengeQueuesPathResponseOnSamePath) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.local_addr = loopback(4433);
    options.remote_addr = loopback(5555);
    options.remote_connection_id = cid_from({0x01, 0x02, 0x03, 0x04});
    fiber::quic::QuicConnection conn(options);
    auto *path = conn.active_path();
    ASSERT_NE(path, nullptr);
    path->validated = true;

    fiber::quic::QuicPathChallengeFrame challenge{};
    for (std::size_t i = 0; i < sizeof(challenge.data); ++i) {
        challenge.data[i] = static_cast<std::uint8_t>(0xa0 + i);
    }

    auto handled = conn.recv_path_challenge_frame(*path, challenge);

    ASSERT_TRUE(handled.has_value()) << static_cast<int>(handled.error());
    ASSERT_FALSE(path->pending_frames.empty());
    const fiber::quic::QuicOutputFrame *response = path->pending_frames.front();
    ASSERT_NE(response, nullptr);
    EXPECT_EQ(response->type, fiber::quic::QuicFrameType::PathResponse);
    EXPECT_EQ(response->path, path);
    EXPECT_EQ(response->min_packet_len, fiber::quic::kMinInitialDatagramSize);
    EXPECT_EQ(std::memcmp(response->u.path_response.data, challenge.data, sizeof(challenge.data)), 0);
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicFrameType::Ping), 1U);
}

TEST(QuicConnectionTest, MigrationQueuesPathChallengesAndValidatesPortOnlyRebindWithoutCongestionReset) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.local_addr = loopback(4433);
    options.remote_addr = loopback(5555);
    options.remote_connection_id = cid_from({0x11, 0x22, 0x33, 0x44});
    fiber::quic::QuicConnection conn(options);
    auto *old = conn.active_path();
    ASSERT_NE(old, nullptr);
    old->validated = true;
    old->mtu = 1400;
    conn.congestion().window = 77777;

    auto *next = conn.create_path(loopback(6666), loopback(4433), options.remote_connection_id,
                                  fiber::quic::QuicPathTag::Probe);
    ASSERT_NE(next, nullptr);

    auto migrated = conn.handle_migration(*next, false, fiber::quic::QuicTime{10000});

    ASSERT_TRUE(migrated.has_value()) << static_cast<int>(migrated.error());
    EXPECT_EQ(conn.active_path(), next);
    EXPECT_EQ(old->tag, fiber::quic::QuicPathTag::Backup);
    EXPECT_EQ(next->state, fiber::quic::QuicPathState::Validating);
    EXPECT_EQ(count_path_pending_frame_type(*next, fiber::quic::QuicFrameType::PathChallenge), 2U);

    fiber::quic::QuicPathChallengeFrame response{};
    std::memcpy(response.data, next->challenge[0], sizeof(response.data));
    auto validated = conn.recv_path_response_frame(response, fiber::quic::QuicTime{11000});

    ASSERT_TRUE(validated.has_value()) << static_cast<int>(validated.error());
    EXPECT_TRUE(*validated);
    EXPECT_TRUE(next->validated);
    EXPECT_EQ(next->state, fiber::quic::QuicPathState::Idle);
    EXPECT_EQ(next->mtu, 1400U);
    EXPECT_EQ(conn.congestion().window, 77777U);
    EXPECT_EQ(count_path_pending_frame_type(*next, fiber::quic::QuicFrameType::PathChallenge), 0U);
}

TEST(QuicConnectionTest, ValidatingMigratedIpResetsCongestionAndRtt) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.local_addr = loopback(4433);
    options.remote_addr = v4_addr({127, 0, 0, 1}, 5555);
    options.remote_connection_id = cid_from({0x11, 0x22, 0x33, 0x44});
    fiber::quic::QuicConnection conn(options);
    auto *old = conn.active_path();
    ASSERT_NE(old, nullptr);
    old->validated = true;

    conn.congestion().window = 77777;
    conn.rtt().avg_rtt = fiber::quic::QuicTime{42};

    auto *next = conn.create_path(v4_addr({127, 0, 0, 2}, 6666), loopback(4433), options.remote_connection_id,
                                  fiber::quic::QuicPathTag::Probe);
    ASSERT_NE(next, nullptr);
    ASSERT_TRUE(conn.handle_migration(*next, false, fiber::quic::QuicTime{10000}).has_value());

    fiber::quic::QuicPathChallengeFrame response{};
    std::memcpy(response.data, next->challenge[1], sizeof(response.data));
    auto validated = conn.recv_path_response_frame(response, fiber::quic::QuicTime{11000});

    ASSERT_TRUE(validated.has_value()) << static_cast<int>(validated.error());
    EXPECT_TRUE(*validated);
    EXPECT_TRUE(next->validated);
    EXPECT_NE(conn.congestion().window, 77777U);
    EXPECT_EQ(conn.rtt().avg_rtt, fiber::quic::QuicTime{fiber::quic::kQuicCongestionInitialRttMs});
}

TEST(QuicConnectionTest, AppliesPeerTransportParamsAndUpdatesLocalStreamLimits) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Server;
    options.remote_connection_id = cid_from({0x11, 0x22, 0x33, 0x44});
    fiber::quic::QuicConnection conn(options);

    fiber::quic::QuicTransportParams params{};
    params.has_initial_source_connection_id = true;
    params.initial_source_connection_id = options.remote_connection_id;
    params.max_udp_payload_size = fiber::quic::kMinInitialDatagramSize;
    params.active_connection_id_limit = 2;
    params.ack_delay_exponent = 7;
    params.max_ack_delay = 33;
    params.initial_max_data = 4096;
    params.initial_max_stream_data_bidi_local = 1024;
    params.initial_max_stream_data_bidi_remote = 2048;
    params.initial_max_stream_data_uni = 512;
    params.initial_max_streams_bidi = 2;
    params.initial_max_streams_uni = 1;

    auto applied = conn.apply_peer_transport_params(params);

    ASSERT_TRUE(applied.has_value());
    EXPECT_TRUE(conn.peer_transport_params_received());
    EXPECT_EQ(conn.peer_transport().params.ack_delay_exponent, 7U);
    EXPECT_EQ(conn.peer_transport().params.max_ack_delay, std::chrono::milliseconds(33));

    auto first = conn.next_local_stream_id(fiber::quic::QuicStreamType::Bidirectional);
    auto second = conn.next_local_stream_id(fiber::quic::QuicStreamType::Bidirectional);
    auto third = conn.next_local_stream_id(fiber::quic::QuicStreamType::Bidirectional);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_FALSE(third.has_value());
}

TEST(QuicConnectionTest, PeerIdleTimeoutUsesMinimumNonZeroTransportValue) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.remote_connection_id = cid_from({0x11, 0x22, 0x33, 0x44});
    options.transport.max_idle_timeout = std::chrono::milliseconds(30000);
    fiber::quic::QuicConnection conn(options);

    auto params = valid_server_peer_params(options);
    params.max_idle_timeout = 12000;
    auto applied = conn.apply_peer_transport_params(params);

    ASSERT_TRUE(applied.has_value()) << static_cast<int>(applied.error());
    EXPECT_EQ(conn.effective_idle_timeout(), std::chrono::milliseconds(12000));
}

TEST(QuicConnectionTest, ZeroLocalIdleTimeoutAllowsPeerIdleTimeout) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.remote_connection_id = cid_from({0x11, 0x22, 0x33, 0x44});
    options.transport.max_idle_timeout = std::chrono::milliseconds(0);
    fiber::quic::QuicConnection conn(options);

    auto params = valid_server_peer_params(options);
    params.max_idle_timeout = 9000;
    auto applied = conn.apply_peer_transport_params(params);

    ASSERT_TRUE(applied.has_value()) << static_cast<int>(applied.error());
    EXPECT_EQ(conn.effective_idle_timeout(), std::chrono::milliseconds(9000));
}

TEST(QuicConnectionTest, ZeroPeerIdleTimeoutKeepsLocalIdleTimeout) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.remote_connection_id = cid_from({0x11, 0x22, 0x33, 0x44});
    options.transport.max_idle_timeout = std::chrono::milliseconds(7000);
    fiber::quic::QuicConnection conn(options);

    auto params = valid_server_peer_params(options);
    params.max_idle_timeout = 0;
    auto applied = conn.apply_peer_transport_params(params);

    ASSERT_TRUE(applied.has_value()) << static_cast<int>(applied.error());
    EXPECT_EQ(conn.effective_idle_timeout(), std::chrono::milliseconds(7000));
}

TEST(QuicConnectionTest, ReceiveClearsSendSideIdleTimerState) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.loop = &group.at(0);
    options.transport.max_idle_timeout = std::chrono::milliseconds(1000);
    fiber::quic::QuicConnection conn(options);

    std::promise<IdleTimerSnapshot> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&]() { return record_idle_timer_lifecycle(&conn, &done); });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const IdleTimerSnapshot snapshot = future.get();
    EXPECT_TRUE(snapshot.send_timer_after_send);
    EXPECT_TRUE(snapshot.idle_armed_after_send);
    EXPECT_FALSE(snapshot.send_timer_after_receive);
    EXPECT_TRUE(snapshot.idle_armed_after_receive);

    group.join();
}

TEST(QuicConnectionTest, IdleTimerMarksClosed) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.loop = &group.at(0);
    options.transport.max_idle_timeout = std::chrono::milliseconds(5);
    fiber::quic::QuicConnection conn(options);

    std::promise<fiber::quic::QuicConnectionState> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_idle_timeout(&conn, std::chrono::milliseconds(25), &done); });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(future.get(), fiber::quic::QuicConnectionState::Closed);
    EXPECT_EQ(conn.state(), fiber::quic::QuicConnectionState::Closed);

    group.join();
}

TEST(QuicConnectionTest, KeepaliveTimerQueuesApplicationPingWhenEstablished) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.loop = &group.at(0);
    options.keepalive_interval = std::chrono::milliseconds(5);
    fiber::quic::QuicConnection conn(options);
    ASSERT_TRUE(conn.mark_established());
    conn.crypto().application_write.ready = true;

    std::promise<KeepaliveTimerSnapshot> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_keepalive_timer(&conn, std::chrono::milliseconds(25), &done); });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const KeepaliveTimerSnapshot snapshot = future.get();
    EXPECT_EQ(snapshot.pending_ping_count, 1U);

    group.join();
}

TEST(QuicConnectionTest, RejectsPeerTransportParamsWithMismatchedInitialScid) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.remote_connection_id = cid_from({0x11, 0x22});
    fiber::quic::QuicConnection conn(options);

    fiber::quic::QuicTransportParams params{};
    params.has_initial_source_connection_id = true;
    params.initial_source_connection_id = cid_from({0x33, 0x44});
    params.max_udp_payload_size = fiber::quic::kMinInitialDatagramSize;
    params.active_connection_id_limit = 2;

    auto applied = conn.apply_peer_transport_params(params);

    EXPECT_FALSE(applied.has_value());
    EXPECT_FALSE(conn.peer_transport_params_received());
}

TEST(QuicConnectionTest, ClientAcceptsServerTransportParamsWithMatchingRetryIds) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Client;
    options.original_destination_connection_id = cid_from({0x01, 0x02, 0x03, 0x04});
    options.remote_connection_id = cid_from({0x11, 0x22, 0x33, 0x44});
    options.retry_source_connection_id = cid_from({0xaa, 0xbb, 0xcc, 0xdd});
    options.has_retry_source_connection_id = true;
    fiber::quic::QuicConnection conn(options);

    fiber::quic::QuicTransportParams params{};
    params.has_initial_source_connection_id = true;
    params.initial_source_connection_id = options.remote_connection_id;
    params.has_original_destination_connection_id = true;
    params.original_destination_connection_id = options.original_destination_connection_id;
    params.has_retry_source_connection_id = true;
    params.retry_source_connection_id = options.retry_source_connection_id;

    auto applied = conn.apply_peer_transport_params(params);

    EXPECT_TRUE(applied.has_value()) << static_cast<int>(applied.error());
    EXPECT_TRUE(conn.peer_transport_params_received());
}

TEST(QuicConnectionTest, ClientRejectsUnexpectedRetrySourceConnectionId) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Client;
    options.original_destination_connection_id = cid_from({0x01, 0x02, 0x03, 0x04});
    options.remote_connection_id = cid_from({0x11, 0x22, 0x33, 0x44});
    fiber::quic::QuicConnection conn(options);

    fiber::quic::QuicTransportParams params{};
    params.has_initial_source_connection_id = true;
    params.initial_source_connection_id = options.remote_connection_id;
    params.has_original_destination_connection_id = true;
    params.original_destination_connection_id = options.original_destination_connection_id;
    params.has_retry_source_connection_id = true;
    params.retry_source_connection_id = cid_from({0xaa, 0xbb, 0xcc, 0xdd});

    auto applied = conn.apply_peer_transport_params(params);

    EXPECT_FALSE(applied.has_value());
    EXPECT_FALSE(conn.peer_transport_params_received());
}

TEST(QuicConnectionTest, RecvStreamFrameCreatesPeerInitiatedStream) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    fiber::quic::QuicConnection conn(options);
    fiber::quic::QuicStreamFrame frame{};
    frame.stream_id = 0;
    frame.length = 3;

    auto received = conn.recv_stream_frame(frame, slice_of("abc"));

    ASSERT_TRUE(received.has_value());
    auto *stream = conn.find_stream(0);
    ASSERT_NE(stream, nullptr);
    EXPECT_EQ(conn.active_stream_count(), 1U);
    fiber::mem::IoBufChain out(conn.recv_extent_pool());
    auto taken = stream->try_read(3, out);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, 3U);
    EXPECT_FALSE(stream->has_final_size());
}

TEST(QuicConnectionTest, RecvStreamFrameCountsEndOffsetGrowthForConnectionFlowControl) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    options.recv_flow.conn_recv_limit = 13;
    options.recv_flow.conn_recv_low_water = 0;
    fiber::quic::QuicConnection conn(options);
    fiber::quic::QuicStreamFrame frame{};
    frame.stream_id = 0;
    frame.offset = 10;
    frame.has_offset = true;
    frame.length = 3;

    auto received = conn.recv_stream_frame(frame, slice_of("abc"));
    auto duplicate = conn.recv_stream_frame(frame, slice_of("abc"));
    frame.offset = 13;
    frame.length = 1;
    auto over_limit = conn.recv_stream_frame(frame, slice_of("x"));

    ASSERT_TRUE(received.has_value());
    ASSERT_TRUE(duplicate.has_value());
    EXPECT_EQ(conn.recv_data_consumed(), 13U);
    EXPECT_FALSE(over_limit.has_value());
    EXPECT_EQ(over_limit.error(), fiber::common::IoErr::MessageTooLarge);
    EXPECT_EQ(conn.recv_data_consumed(), 13U);
}

TEST(QuicConnectionTest, RecvStreamFrameExtendsConnectionFlowControlAtLowWater) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    options.recv_flow.conn_recv_limit = 20;
    options.recv_flow.conn_recv_low_water = 5;
    fiber::quic::QuicConnection conn(options);
    fiber::quic::QuicStreamFrame frame{};
    frame.stream_id = 0;
    frame.offset = 15;
    frame.has_offset = true;
    frame.length = 1;

    auto received = conn.recv_stream_frame(frame, slice_of("x"));

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(conn.recv_data_consumed(), 16U);
    EXPECT_EQ(conn.recv_data_limit(), 40U);
    auto &space = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    ASSERT_NE(space.pending_frames.front(), nullptr);
    EXPECT_EQ(space.pending_frames.front()->type, fiber::quic::QuicFrameType::MaxData);
    EXPECT_EQ(space.pending_frames.front()->u.max_data.max_data, 40U);
}

TEST(QuicConnectionTest, StreamReadExtendsStreamFlowControlOnly) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    options.recv_flow.conn_recv_limit = 100;
    options.recv_flow.conn_recv_low_water = 0;
    options.recv_flow.stream_buffer_limit = 8;
    options.recv_flow.stream_low_water = 3;
    fiber::quic::QuicConnection conn(options);
    fiber::quic::QuicStreamFrame frame{};
    frame.stream_id = 0;
    frame.length = 6;
    ASSERT_TRUE(conn.recv_stream_frame(frame, slice_of("abcdef")).has_value());
    auto *stream = conn.find_stream(0);
    ASSERT_NE(stream, nullptr);

    fiber::mem::IoBufChain out(conn.recv_extent_pool());
    auto taken = stream->try_read(6, out);

    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, 6U);
    EXPECT_EQ(conn.recv_data_limit(), 100U);
    auto &space = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    ASSERT_NE(space.pending_frames.front(), nullptr);
    EXPECT_EQ(space.pending_frames.front()->type, fiber::quic::QuicFrameType::MaxStreamData);
    EXPECT_EQ(space.pending_frames.front()->u.max_stream_data.id, 0U);
    EXPECT_EQ(space.pending_frames.front()->u.max_stream_data.limit, 14U);
}

TEST(QuicConnectionTest, RejectsPassiveStreamWhenConnectionOpsIsMissing) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Server;
    fiber::quic::QuicConnection conn(options);
    fiber::quic::QuicStreamFrame frame{};
    frame.stream_id = 0;

    auto received = conn.recv_stream_frame(frame, {});

    EXPECT_FALSE(received.has_value());
    EXPECT_EQ(received.error(), fiber::common::IoErr::NotSupported);
    EXPECT_EQ(conn.active_stream_count(), 0U);
}

TEST(QuicConnectionTest, UsesConnectionOpsToCreatePeerStreamOnce) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    fiber::quic::QuicConnection conn(options);
    fiber::quic::QuicStreamFrame frame{};
    frame.stream_id = 0;
    frame.length = 3;

    auto first = conn.recv_stream_frame(frame, slice_of("abc"));
    auto duplicate = conn.recv_stream_frame(frame, slice_of("abc"));

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(duplicate.has_value());
    EXPECT_EQ(state.calls, 1U);
    EXPECT_EQ(state.last_stream_id, 0U);
    EXPECT_EQ(conn.active_stream_count(), 1U);
}

TEST(QuicConnectionTest, RejectsPeerStreamWhenConnectionOpsReturnsEmptyLease) {
    StreamCallbackState state{};
    state.return_empty = true;
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Server;
    options.owner = &state;
    options.ops.create_stream = create_stream_record;
    options.ops.on_peer_stream_attached = on_peer_stream_attached_record;
    fiber::quic::QuicConnection conn(options);
    fiber::quic::QuicStreamFrame frame{};
    frame.stream_id = 0;

    auto received = conn.recv_stream_frame(frame, {});

    EXPECT_FALSE(received.has_value());
    EXPECT_EQ(received.error(), fiber::common::IoErr::NoMem);
    EXPECT_EQ(state.calls, 1U);
    EXPECT_EQ(conn.find_stream(0), nullptr);
    EXPECT_EQ(conn.active_stream_count(), 0U);
}

TEST(QuicConnectionTest, ConnectionOpsCanCreateAndRetainRetiredResetStream) {
    StreamCallbackState state{};
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Server;
    options.owner = &state;
    options.ops.create_stream = create_stream_retain;
    options.ops.on_peer_stream_attached = on_peer_stream_attached_retain;
    fiber::quic::QuicConnection conn(options);
    fiber::quic::QuicResetStreamFrame reset{};
    reset.id = 0;
    reset.error_code = 7;
    reset.final_size = 0;

    auto received = conn.recv_reset_stream_frame(reset);

    ASSERT_TRUE(received.has_value());
    ASSERT_TRUE(state.lease);
    EXPECT_EQ(state.calls, 1U);
    EXPECT_EQ(state.lease->stream_id(), 0U);
    EXPECT_TRUE(state.lease->reset_received());
    EXPECT_EQ(state.lease->reset_error_code(), 7U);
    EXPECT_FALSE(state.lease->attached_to_connection());
    EXPECT_EQ(conn.active_stream_count(), 0U);
    state.lease.reset();
}

TEST(QuicConnectionTest, RecvFinStreamRetiresAfterDataIsTaken) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    fiber::quic::QuicConnection conn(options);
    fiber::quic::QuicStreamFrame frame{};
    frame.stream_id = 0;
    frame.length = 3;
    frame.fin = true;

    auto received = conn.recv_stream_frame(frame, slice_of("abc"));
    ASSERT_TRUE(received.has_value());
    auto *stream = conn.find_stream(0);
    ASSERT_NE(stream, nullptr);
    EXPECT_TRUE(stream->has_final_size());
    EXPECT_EQ(stream->final_size(), 3U);

    fiber::mem::IoBufChain out(conn.recv_extent_pool());
    auto taken = stream->try_read(3, out);

    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(*taken, 3U);
    EXPECT_EQ(out.readable_bytes(), 3U);
    conn.release_stream_app(*stream);
    EXPECT_EQ(conn.find_stream(0), nullptr);
    EXPECT_EQ(conn.active_stream_count(), 0U);

    auto duplicate = conn.recv_stream_frame(frame, slice_of("abc"));
    EXPECT_TRUE(duplicate.has_value());
    EXPECT_EQ(conn.active_stream_count(), 0U);

    fiber::quic::QuicStreamFrame conflicting{};
    conflicting.stream_id = 0;
    conflicting.length = 4;
    conflicting.fin = true;
    auto ignored_conflict = conn.recv_stream_frame(conflicting, slice_of("abcd"));
    EXPECT_TRUE(ignored_conflict.has_value());
    EXPECT_EQ(conn.find_stream(0), nullptr);
    EXPECT_EQ(conn.active_stream_count(), 0U);
    EXPECT_EQ(state.calls, 1U);
}

TEST(QuicConnectionTest, ResetStreamCountsFinalSizeGrowthForConnectionFlowControl) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    options.recv_flow.conn_recv_limit = 10;
    options.recv_flow.conn_recv_low_water = 0;
    fiber::quic::QuicConnection conn(options);
    fiber::quic::QuicStreamFrame data{};
    data.stream_id = 0;
    data.offset = 5;
    data.has_offset = true;
    data.length = 1;
    fiber::quic::QuicResetStreamFrame reset{};
    reset.id = 0;
    reset.error_code = 42;
    reset.final_size = 10;

    auto received = conn.recv_stream_frame(data, slice_of("x"));
    auto reset_result = conn.recv_reset_stream_frame(reset);

    ASSERT_TRUE(received.has_value());
    ASSERT_TRUE(reset_result.has_value());
    EXPECT_EQ(conn.recv_data_consumed(), 10U);
}

TEST(QuicConnectionTest, StopReadQueuesStopSendingFrame) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    fiber::quic::QuicConnection conn(options);
    fiber::quic::QuicStreamFrame frame{};
    frame.stream_id = 0;

    ASSERT_TRUE(conn.recv_stream_frame(frame, {}).has_value());
    auto *stream = conn.find_stream(0);
    ASSERT_NE(stream, nullptr);

    auto stopped = stream->stop_read(7);

    ASSERT_TRUE(stopped.has_value());
    auto &space = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    ASSERT_NE(space.pending_frames.front(), nullptr);
    EXPECT_EQ(space.pending_frames.front()->type, fiber::quic::QuicFrameType::StopSending);
    EXPECT_EQ(space.pending_frames.front()->u.stop_sending.id, 0U);
    EXPECT_EQ(space.pending_frames.front()->u.stop_sending.error_code, 7U);
}

TEST(QuicConnectionTest, RemoteStopSendingQueuesResetStreamFrame) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    fiber::quic::QuicConnection conn(options);
    fiber::quic::QuicStreamFrame frame{};
    frame.stream_id = 0;
    ASSERT_TRUE(conn.recv_stream_frame(frame, {}).has_value());
    fiber::quic::QuicStopSendingFrame stop{};
    stop.id = 0;
    stop.error_code = 9;

    auto stopped = conn.recv_stop_sending_frame(stop);

    ASSERT_TRUE(stopped.has_value());
    auto &space = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    ASSERT_NE(space.pending_frames.front(), nullptr);
    EXPECT_EQ(space.pending_frames.front()->type, fiber::quic::QuicFrameType::ResetStream);
    EXPECT_EQ(space.pending_frames.front()->u.reset_stream.id, 0U);
    EXPECT_EQ(space.pending_frames.front()->u.reset_stream.error_code, 9U);
    EXPECT_EQ(space.pending_frames.front()->u.reset_stream.final_size, 0U);
}

TEST(QuicConnectionTest, MaxStreamDataUpdatesStreamWriteWindow) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    fiber::quic::QuicConnection conn(options);
    fiber::quic::QuicStreamFrame frame{};
    frame.stream_id = 0;
    ASSERT_TRUE(conn.recv_stream_frame(frame, {}).has_value());
    auto *stream = conn.find_stream(0);
    ASSERT_NE(stream, nullptr);
    grant_max_data(conn, 1024);

    auto blocked = stream->try_write(iobuf_of("abc"));
    fiber::quic::QuicMaxStreamDataFrame max_stream_data{};
    max_stream_data.id = 0;
    max_stream_data.limit = 5;
    auto updated = conn.recv_max_stream_data_frame(max_stream_data);
    auto written = stream->try_write(iobuf_of("abc"));

    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(blocked.error(), fiber::common::IoErr::WouldBlock);
    ASSERT_TRUE(updated.has_value());
    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(*written, 3U);
}

TEST(QuicConnectionTest, StreamWriteShortWritesToStreamCredit) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    fiber::quic::QuicConnection conn(options);
    auto stream = conn.get_or_create_peer_stream(0);
    ASSERT_TRUE(stream.has_value());
    grant_max_stream_data(conn, 0, 2);
    grant_max_data(conn, 1024);

    auto written = (*stream)->try_write(iobuf_of("abc"));
    auto blocked = (*stream)->try_write(iobuf_of("c"));

    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(*written, 2U);
    EXPECT_EQ(conn.peer_data_reserved(), 2U);
    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(blocked.error(), fiber::common::IoErr::WouldBlock);
}

TEST(QuicConnectionTest, StreamWriteShortWritesToConnectionCredit) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    fiber::quic::QuicConnection conn(options);
    auto stream = conn.get_or_create_peer_stream(0);
    ASSERT_TRUE(stream.has_value());
    grant_max_stream_data(conn, 0, 1024);
    grant_max_data(conn, 2);

    auto written = (*stream)->try_write(iobuf_of("abc"));
    auto blocked = (*stream)->try_write(iobuf_of("c"));

    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(*written, 2U);
    EXPECT_EQ(conn.peer_data_reserved(), 2U);
    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(blocked.error(), fiber::common::IoErr::WouldBlock);
}

TEST(QuicConnectionTest, StreamWriteShortWritesToBufferLimit) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    fiber::quic::QuicConnection conn(options);
    auto stream = conn.get_or_create_peer_stream(0);
    ASSERT_TRUE(stream.has_value());
    grant_max_stream_data(conn, 0, fiber::quic::kQuicStreamSendDefaultBufferLimit + 4);
    grant_max_data(conn, fiber::quic::kQuicStreamSendDefaultBufferLimit + 4);

    std::string payload(fiber::quic::kQuicStreamSendDefaultBufferLimit + 4, 'x');
    auto written = (*stream)->try_write(iobuf_of(payload));
    auto blocked = (*stream)->try_write(iobuf_of("x"));

    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(*written, fiber::quic::kQuicStreamSendDefaultBufferLimit);
    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(blocked.error(), fiber::common::IoErr::WouldBlock);
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicFrameType::StreamDataBlocked), 0U);
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicFrameType::DataBlocked), 0U);
}

TEST(QuicConnectionTest, StreamWriteQueuesStreamDataBlockedAtStreamLimit) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    fiber::quic::QuicConnection conn(options);
    auto stream = conn.get_or_create_peer_stream(0);
    ASSERT_TRUE(stream.has_value());
    grant_max_data(conn, 1024);

    auto blocked = (*stream)->try_write(iobuf_of("a"));
    auto duplicate = (*stream)->try_write(iobuf_of("b"));

    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(blocked.error(), fiber::common::IoErr::WouldBlock);
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_EQ(duplicate.error(), fiber::common::IoErr::WouldBlock);
    EXPECT_EQ(count_pending_stream_data_blocked(conn, 0, 0), 1U);
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicFrameType::DataBlocked), 0U);
}

TEST(QuicConnectionTest, StreamWriteQueuesDataBlockedAtConnectionLimit) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    fiber::quic::QuicConnection conn(options);
    auto stream = conn.get_or_create_peer_stream(0);
    ASSERT_TRUE(stream.has_value());
    grant_max_stream_data(conn, 0, 1024);

    auto blocked = (*stream)->try_write(iobuf_of("a"));
    auto duplicate = (*stream)->try_write(iobuf_of("b"));

    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(blocked.error(), fiber::common::IoErr::WouldBlock);
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_EQ(duplicate.error(), fiber::common::IoErr::WouldBlock);
    EXPECT_EQ(count_pending_data_blocked(conn, 0), 1U);
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicFrameType::StreamDataBlocked), 0U);
}

TEST(QuicConnectionTest, StreamWriteQueuesBothBlockedFramesWhenBothLimitsApply) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    fiber::quic::QuicConnection conn(options);
    auto stream = conn.get_or_create_peer_stream(0);
    ASSERT_TRUE(stream.has_value());

    auto blocked = (*stream)->try_write(iobuf_of("a"));

    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(blocked.error(), fiber::common::IoErr::WouldBlock);
    EXPECT_EQ(count_pending_stream_data_blocked(conn, 0, 0), 1U);
    EXPECT_EQ(count_pending_data_blocked(conn, 0), 1U);
}

TEST(QuicConnectionTest, StreamWriteReportsStreamDataBlockedAgainForNewLimit) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    fiber::quic::QuicConnection conn(options);
    auto stream = conn.get_or_create_peer_stream(0);
    ASSERT_TRUE(stream.has_value());
    grant_max_stream_data(conn, 0, 2);
    grant_max_data(conn, 1024);

    auto first = (*stream)->try_write(iobuf_of("abc"));
    grant_max_stream_data(conn, 0, 4);
    auto second = (*stream)->try_write(iobuf_of("cde"));

    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 2U);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*second, 2U);
    EXPECT_EQ(count_pending_stream_data_blocked(conn, 0, 2), 1U);
    EXPECT_EQ(count_pending_stream_data_blocked(conn, 0, 4), 1U);
}

TEST(QuicConnectionTest, StreamWriteReportsDataBlockedAgainForNewLimit) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    fiber::quic::QuicConnection conn(options);
    auto stream = conn.get_or_create_peer_stream(0);
    ASSERT_TRUE(stream.has_value());
    grant_max_stream_data(conn, 0, 1024);
    grant_max_data(conn, 2);

    auto first = (*stream)->try_write(iobuf_of("abc"));
    grant_max_data(conn, 4);
    auto second = (*stream)->try_write(iobuf_of("cde"));

    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 2U);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*second, 2U);
    EXPECT_EQ(count_pending_data_blocked(conn, 2), 1U);
    EXPECT_EQ(count_pending_data_blocked(conn, 4), 1U);
}

TEST(QuicConnectionTest, LostDataBlockedFrameRequeuesOnlyWhileStillBlocked) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    fiber::quic::QuicConnection conn(options);
    auto stream = conn.get_or_create_peer_stream(0);
    ASSERT_TRUE(stream.has_value());
    grant_max_stream_data(conn, 0, 1024);
    ASSERT_FALSE((*stream)->try_write(iobuf_of("a")).has_value());

    auto &space = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    fiber::quic::QuicOutputFrame *frame = space.pending_frames.pop_front();
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(frame->type, fiber::quic::QuicFrameType::DataBlocked);
    frame->packet_number = 0;
    frame->send_time = fiber::quic::QuicTime{0};
    frame->packet_ack_eliciting = true;
    space.sent_frames.push_back(*frame);
    space.largest_acked_packet_number = 4;
    space.next_packet_number = 5;

    auto lost = fiber::quic::quic_detect_lost(conn, fiber::quic::QuicTime{1000}, nullptr);

    ASSERT_TRUE(lost.has_value());
    EXPECT_TRUE(lost->lost_frames);
    EXPECT_EQ(count_pending_data_blocked(conn, 0), 1U);
}

TEST(QuicConnectionTest, LostDataBlockedFrameDropsAfterConnectionLimitIncreases) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    fiber::quic::QuicConnection conn(options);
    auto stream = conn.get_or_create_peer_stream(0);
    ASSERT_TRUE(stream.has_value());
    grant_max_stream_data(conn, 0, 1024);
    ASSERT_FALSE((*stream)->try_write(iobuf_of("a")).has_value());

    auto &space = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    fiber::quic::QuicOutputFrame *frame = space.pending_frames.pop_front();
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(frame->type, fiber::quic::QuicFrameType::DataBlocked);
    frame->packet_number = 0;
    frame->send_time = fiber::quic::QuicTime{0};
    frame->packet_ack_eliciting = true;
    space.sent_frames.push_back(*frame);
    space.largest_acked_packet_number = 4;
    space.next_packet_number = 5;
    grant_max_data(conn, 1);

    auto lost = fiber::quic::quic_detect_lost(conn, fiber::quic::QuicTime{1000}, nullptr);

    ASSERT_TRUE(lost.has_value());
    EXPECT_FALSE(lost->lost_frames);
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicFrameType::DataBlocked), 0U);
}

TEST(QuicConnectionTest, AsyncWriteResumesAfterMaxStreamData) {
    StreamCallbackState state{};
    fiber::event::EventLoopGroup group(1);
    auto options = server_options_with_factory(state);
    options.loop = &group.at(0);
    fiber::quic::QuicConnection conn(options);
    auto stream = conn.get_or_create_peer_stream(0);
    ASSERT_TRUE(stream.has_value());
    grant_max_data(conn, 1024);

    std::promise<WriteResult> done;
    auto future = done.get_future();
    std::atomic<bool> grant_seen{false};

    group.start();
    fiber::async::spawn(group.at(0), [stream = *stream, &done]() { return write_one(stream, &done); });
    fiber::async::spawn(group.at(0),
                        [&conn, &grant_seen]() { return grant_max_stream_data_after_delay(&conn, 0, 1, &grant_seen); });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "write did not resume after MAX_STREAM_DATA";
        return;
    }

    WriteResult result = future.get();
    EXPECT_TRUE(grant_seen.load(std::memory_order_relaxed));
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.value, 1U);
    group.join();
}

TEST(QuicConnectionTest, AsyncWriteResumesAfterMaxData) {
    StreamCallbackState state{};
    fiber::event::EventLoopGroup group(1);
    auto options = server_options_with_factory(state);
    options.loop = &group.at(0);
    fiber::quic::QuicConnection conn(options);
    auto stream = conn.get_or_create_peer_stream(0);
    ASSERT_TRUE(stream.has_value());
    grant_max_stream_data(conn, 0, 1024);

    std::promise<WriteResult> done;
    auto future = done.get_future();
    std::atomic<bool> grant_seen{false};

    group.start();
    fiber::async::spawn(group.at(0), [stream = *stream, &done]() { return write_one(stream, &done); });
    fiber::async::spawn(group.at(0),
                        [&conn, &grant_seen]() { return grant_max_data_after_delay(&conn, 1, &grant_seen); });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "write did not resume after MAX_DATA";
        return;
    }

    WriteResult result = future.get();
    EXPECT_TRUE(grant_seen.load(std::memory_order_relaxed));
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.value, 1U);
    EXPECT_EQ(conn.peer_data_reserved(), 1U);
    group.join();
}

TEST(QuicConnectionTest, AsyncWriteRequeuesForConnectionWindowAfterMaxStreamData) {
    StreamCallbackState state{};
    fiber::event::EventLoopGroup group(1);
    auto options = server_options_with_factory(state);
    options.loop = &group.at(0);
    fiber::quic::QuicConnection conn(options);
    auto stream = conn.get_or_create_peer_stream(0);
    ASSERT_TRUE(stream.has_value());

    std::promise<WriteResult> done;
    auto future = done.get_future();
    std::atomic<bool> stream_grant_seen{false};
    std::atomic<bool> data_grant_seen{false};

    group.start();
    fiber::async::spawn(group.at(0), [stream = *stream, &done]() { return write_one(stream, &done); });
    fiber::async::spawn(group.at(0), [&conn, &stream_grant_seen]() {
        return grant_max_stream_data_after_delay(&conn, 0, 1, &stream_grant_seen);
    });

    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!stream_grant_seen.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < wait_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(stream_grant_seen.load(std::memory_order_relaxed));
    EXPECT_NE(future.wait_for(std::chrono::milliseconds(20)), std::future_status::ready);

    fiber::async::spawn(group.at(0),
                        [&conn, &data_grant_seen]() { return grant_max_data_after_delay(&conn, 1, &data_grant_seen); });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "write did not resume after later MAX_DATA";
        return;
    }

    WriteResult result = future.get();
    EXPECT_TRUE(data_grant_seen.load(std::memory_order_relaxed));
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.value, 1U);
    EXPECT_EQ(conn.peer_data_reserved(), 1U);
    group.join();
}

TEST(QuicConnectionTest, AsyncWriteTimeoutUnlinksConnectionWindowWaiter) {
    StreamCallbackState state{};
    fiber::event::EventLoopGroup group(1);
    auto options = server_options_with_factory(state);
    options.loop = &group.at(0);
    fiber::quic::QuicConnection conn(options);
    auto stream = conn.get_or_create_peer_stream(0);
    ASSERT_TRUE(stream.has_value());
    grant_max_stream_data(conn, 0, 1);

    std::promise<WriteResult> done;
    auto future = done.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [stream = *stream, &done]() {
        return write_one_with_timeout(stream, std::chrono::milliseconds(20), &done);
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "write did not time out";
        return;
    }

    WriteResult result = future.get();
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, fiber::common::IoErr::TimedOut);
    group.join();

    grant_max_data(conn, 1);
    EXPECT_EQ(conn.peer_data_reserved(), 0U);
}

TEST(QuicConnectionTest, ResetStreamCreatesAndRetiresPeerStream) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    fiber::quic::QuicConnection conn(options);
    fiber::quic::QuicResetStreamFrame reset{};
    reset.id = 0;
    reset.error_code = 42;
    reset.final_size = 0;

    auto received = conn.recv_reset_stream_frame(reset);

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(conn.find_stream(0), nullptr);
    EXPECT_EQ(conn.active_stream_count(), 0U);

    auto duplicate = conn.recv_reset_stream_frame(reset);
    EXPECT_TRUE(duplicate.has_value());

    reset.final_size = 1;
    auto conflict = conn.recv_reset_stream_frame(reset);
    EXPECT_TRUE(conflict.has_value());
    EXPECT_EQ(conn.find_stream(0), nullptr);
    EXPECT_EQ(conn.active_stream_count(), 0U);
}

TEST(QuicConnectionTest, LowerPeerStreamBelowOpenedWatermarkIsGoneWhenMissing) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    fiber::quic::QuicConnection conn(options);

    // Open stream 0, then retire it with a FIN (final_size 0). Test streams are
    // app-released by default and carry no local send work, so receiving the
    // FIN makes the stream ready_for_connection_release and try_release_stream
    // retires it.
    fiber::quic::QuicStreamFrame open{};
    open.stream_id = 0;
    ASSERT_TRUE(conn.recv_stream_frame(open, {}).has_value());
    fiber::quic::QuicStreamFrame fin{};
    fin.stream_id = 0;
    fin.fin = true;
    ASSERT_TRUE(conn.recv_stream_frame(fin, {}).has_value());
    ASSERT_EQ(state.calls, 1U);
    EXPECT_EQ(conn.find_stream(0), nullptr); // retired
    EXPECT_EQ(conn.active_stream_count(), 0U);

    // A late duplicate frame for the now-gone stream 0 is silently dropped
    // (is_gone_peer_stream: seq < opened_count and absent from the table), not
    // re-created and not re-delivered. This is the genuinely-gone path —
    // distinct from an out-of-order gap, whose intermediate streams are now
    // real active streams (see OutOfOrderPeerStreamFrameCreatesImplicit*).
    fiber::quic::QuicStreamFrame late{};
    late.stream_id = 0;
    late.length = 3;
    auto late_received = conn.recv_stream_frame(late, slice_of("xyz"));

    EXPECT_TRUE(late_received.has_value());
    EXPECT_EQ(conn.find_stream(0), nullptr);
    EXPECT_EQ(conn.active_stream_count(), 0U);
    EXPECT_EQ(state.calls, 1U);
}

// RFC 9000 §2.1: a STREAM frame for a higher stream ID that arrives before the
// lower-numbered ones (packet loss / reordering) implicitly opens every
// intermediate stream. They are real active streams — the app is notified of
// each — and frames arriving later for them are delivered, not dropped.
// Previously the gap was marked retired and the intermediate streams were never
// created, causing permanent data loss on lossy links.
TEST(QuicConnectionTest, OutOfOrderPeerStreamFrameCreatesImplicitIntermediates) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    fiber::quic::QuicConnection conn(options);

    // Stream 8 (seq 2) arrives before streams 0 and 4. nginx creates streams
    // 0, 4, 8 in order and notifies the app for each.
    fiber::quic::QuicStreamFrame frame8{};
    frame8.stream_id = 8;
    frame8.length = 3;
    ASSERT_TRUE(conn.recv_stream_frame(frame8, slice_of("abc")).has_value());

    EXPECT_EQ(state.calls, 3U);
    EXPECT_EQ(state.last_stream_id, 8U);
    EXPECT_NE(conn.find_stream(0), nullptr);
    EXPECT_NE(conn.find_stream(4), nullptr);
    EXPECT_NE(conn.find_stream(8), nullptr);
    EXPECT_EQ(conn.active_stream_count(), 3U);

    // The implicit intermediate stream 4 is open with no data and no final
    // size (nginx sets send/recv_final_size = -1; it is NOT half-closed/FIN'd).
    auto *stream4 = conn.find_stream(4);
    ASSERT_NE(stream4, nullptr);
    EXPECT_FALSE(stream4->has_final_size());
    EXPECT_FALSE(stream4->reset_received());

    // A later STREAM+data frame for the implicit stream 4 is delivered (NOT
    // dropped) — proving the previous data-loss bug is fixed.
    fiber::quic::QuicStreamFrame frame4{};
    frame4.stream_id = 4;
    frame4.length = 3;
    ASSERT_TRUE(conn.recv_stream_frame(frame4, slice_of("xyz")).has_value());

    fiber::mem::IoBufChain out(conn.recv_extent_pool());
    auto delivered = stream4->try_read(3, out);
    ASSERT_TRUE(delivered.has_value());
    EXPECT_EQ(*delivered, 3U);
    EXPECT_EQ(state.calls, 3U); // no new streams notified for the in-order frame
}

TEST(QuicConnectionTest, OutOfOrderPeerStreamFrameCreatesMultipleImplicitIntermediates) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    fiber::quic::QuicConnection conn(options);

    // Stream 0 opens normally.
    fiber::quic::QuicStreamFrame frame0{};
    frame0.stream_id = 0;
    ASSERT_TRUE(conn.recv_stream_frame(frame0, {}).has_value());
    EXPECT_EQ(state.calls, 1U);

    // Stream 16 (seq 4) arrives out of order: implicit streams 4, 8, 12 are
    // created in addition to 16.
    fiber::quic::QuicStreamFrame frame16{};
    frame16.stream_id = 16;
    ASSERT_TRUE(conn.recv_stream_frame(frame16, {}).has_value());

    EXPECT_EQ(state.calls, 5U); // 0, 4, 8, 12, 16
    EXPECT_EQ(state.last_stream_id, 16U);
    EXPECT_NE(conn.find_stream(0), nullptr);
    EXPECT_NE(conn.find_stream(4), nullptr);
    EXPECT_NE(conn.find_stream(8), nullptr);
    EXPECT_NE(conn.find_stream(12), nullptr);
    EXPECT_NE(conn.find_stream(16), nullptr);
    EXPECT_EQ(conn.active_stream_count(), 5U);
}

// An implicitly-opened stream with no data is open (not half-closed/FIN'd); a
// later FIN or RESET_STREAM on it is honored (nginx delivers later frames for
// implicit streams rather than dropping them).
TEST(QuicConnectionTest, ImplicitlyOpenedPeerStreamIsOpenAndHonorsLaterFinAndReset) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    fiber::quic::QuicConnection conn(options);

    // Stream 8 (seq 2) arrives first; streams 0 and 4 are implicitly opened.
    fiber::quic::QuicStreamFrame frame8{};
    frame8.stream_id = 8;
    ASSERT_TRUE(conn.recv_stream_frame(frame8, {}).has_value());
    ASSERT_EQ(state.calls, 3U);
    EXPECT_EQ(conn.active_stream_count(), 3U);

    auto *stream4 = conn.find_stream(4);
    ASSERT_NE(stream4, nullptr);
    EXPECT_FALSE(stream4->has_final_size());
    EXPECT_FALSE(stream4->reset_received());

    // A later FIN on the implicit stream 4 is honored: it sets the final size
    // and (test streams are app-released with no local send work) retires the
    // stream. Had the FIN been dropped, stream 4 would still be open.
    fiber::quic::QuicStreamFrame fin4{};
    fin4.stream_id = 4;
    fin4.fin = true;
    ASSERT_TRUE(conn.recv_stream_frame(fin4, {}).has_value());
    EXPECT_EQ(conn.find_stream(4), nullptr);
    EXPECT_EQ(conn.active_stream_count(), 2U);

    // A later RESET_STREAM on the implicit stream 0 is honored likewise.
    fiber::quic::QuicResetStreamFrame reset0{};
    reset0.id = 0;
    reset0.error_code = 7;
    reset0.final_size = 0;
    ASSERT_TRUE(conn.recv_reset_stream_frame(reset0).has_value());
    EXPECT_EQ(conn.find_stream(0), nullptr);
    EXPECT_EQ(conn.active_stream_count(), 1U); // only stream 8 remains
}

// nginx's only hard gate on peer stream creation is the advertised max_streams
// (client_max_streams_bidi): a stream at or beyond it is a STREAM_LIMIT_ERROR.
// There is no separate concurrent-active gate that drops gap data — nginx
// creates intermediate streams unconditionally within the advertised limit.
TEST(QuicConnectionTest, ImplicitPeerStreamCreationRespectsAdvertisedStreamLimit) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    options.max_peer_bidirectional_streams = 3; // advertised = concurrent = 3
    fiber::quic::QuicConnection conn(options);

    // Stream 8 (seq 2) arrives out of order: implicit 0, 4, 8 — all within the
    // advertised limit (seq 0..2). Created unconditionally.
    fiber::quic::QuicStreamFrame frame8{};
    frame8.stream_id = 8;
    ASSERT_TRUE(conn.recv_stream_frame(frame8, {}).has_value());
    EXPECT_EQ(state.calls, 3U);
    EXPECT_EQ(conn.active_stream_count(), 3U);

    // A further out-of-order stream whose sequence reaches the advertised
    // limit (seq 3 >= advertised 3) is a STREAM_LIMIT_ERROR peer violation:
    // the connection closes (nginx closes here too). The gap is NOT created.
    fiber::quic::QuicStreamFrame over{};
    over.stream_id = 12; // seq 3 >= advertised 3
    EXPECT_FALSE(conn.recv_stream_frame(over, {}).has_value());
    EXPECT_TRUE(conn.terminal_closing());
    EXPECT_EQ(conn.close_error(), fiber::quic::QuicErrorCode::StreamLimitError);
    EXPECT_EQ(state.calls, 3U); // no implicit streams for the over-limit gap
}

TEST(QuicConnectionTest, RetiringPeerStreamQueuesMaxStreamsWithinConcurrentLimit) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    options.max_peer_bidirectional_streams = 4;
    fiber::quic::QuicConnection conn(options);

    for (std::uint64_t id = 0; id < 16; id += 4) {
        fiber::quic::QuicStreamFrame frame{};
        frame.stream_id = id;
        ASSERT_TRUE(conn.recv_stream_frame(frame, {}).has_value());
    }
    EXPECT_EQ(conn.active_stream_count(), 4U);

    // Retiring one peer stream grows the advertised max_streams (a MAX_STREAMS
    // frame is queued) and frees a concurrent slot, so the next in-range stream
    // can still open. This is the concurrent-active-stream accounting, which is
    // a non-fatal flow-control gate (RFC 9000 §4.6), NOT a peer violation.
    fiber::quic::QuicStreamFrame fin{};
    fin.stream_id = 0;
    fin.fin = true;
    ASSERT_TRUE(conn.recv_stream_frame(fin, {}).has_value());

    EXPECT_EQ(conn.active_stream_count(), 3U);
    EXPECT_EQ(count_pending_max_streams(conn, fiber::quic::QuicFrameType::MaxStreamsBidi, 5), 1U);

    fiber::quic::QuicStreamFrame in_range{};
    in_range.stream_id = 16; // seq 4 < advertised 5, concurrent slot free
    ASSERT_TRUE(conn.recv_stream_frame(in_range, {}).has_value());
    EXPECT_EQ(conn.active_stream_count(), 4U);

    // A stream ID at or beyond the advertised max_streams is a STREAM_LIMIT_ERROR
    // peer violation (RFC 9000 §4.6): the connection closes immediately rather
    // than silently tolerating the over-limit stream (nginx closes here too).
    fiber::quic::QuicStreamFrame over_advertised{};
    over_advertised.stream_id = 20; // seq 5 >= advertised 5
    EXPECT_FALSE(conn.recv_stream_frame(over_advertised, {}).has_value());
    EXPECT_TRUE(conn.terminal_closing());
    EXPECT_EQ(conn.close_error(), fiber::quic::QuicErrorCode::StreamLimitError);
}

TEST(QuicConnectionTest, PeerBidirectionalAndUnidirectionalStreamLimitsAreIndependent) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    options.max_peer_bidirectional_streams = 1;
    options.max_peer_unidirectional_streams = 1;
    fiber::quic::QuicConnection conn(options);

    fiber::quic::QuicStreamFrame bidi{};
    bidi.stream_id = 0;
    fiber::quic::QuicStreamFrame uni{};
    uni.stream_id = 2;
    fiber::quic::QuicStreamFrame second_bidi{};
    second_bidi.stream_id = 4;
    fiber::quic::QuicStreamFrame second_uni{};
    second_uni.stream_id = 6;

    EXPECT_TRUE(conn.recv_stream_frame(bidi, {}).has_value());
    EXPECT_TRUE(conn.recv_stream_frame(uni, {}).has_value());
    EXPECT_FALSE(conn.recv_stream_frame(second_bidi, {}).has_value());
    EXPECT_FALSE(conn.recv_stream_frame(second_uni, {}).has_value());
    EXPECT_EQ(conn.active_stream_count(), 2U);
}

TEST(QuicConnectionTest, RecvMaxStreamsUpdatesLocalStreamLimitAndIgnoresLowerValues) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Client;
    options.max_local_bidirectional_streams = 1;
    fiber::quic::QuicConnection conn(options);

    auto first = conn.next_local_stream_id(fiber::quic::QuicStreamType::Bidirectional);
    auto blocked = conn.next_local_stream_id(fiber::quic::QuicStreamType::Bidirectional);

    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 0U);
    EXPECT_FALSE(blocked.has_value());
    EXPECT_EQ(blocked.error(), fiber::common::IoErr::Busy);
    EXPECT_EQ(count_pending_streams_blocked(conn, fiber::quic::QuicFrameType::StreamsBlockedBidi, 1), 1U);

    fiber::quic::QuicMaxStreamsFrame max_streams{};
    max_streams.bidirectional = true;
    max_streams.limit = 3;
    ASSERT_TRUE(conn.recv_max_streams_frame(max_streams).has_value());

    auto second = conn.next_local_stream_id(fiber::quic::QuicStreamType::Bidirectional);
    auto third = conn.next_local_stream_id(fiber::quic::QuicStreamType::Bidirectional);
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(*second, 4U);
    EXPECT_EQ(*third, 8U);

    max_streams.limit = 2;
    ASSERT_TRUE(conn.recv_max_streams_frame(max_streams).has_value());
    auto still_blocked = conn.next_local_stream_id(fiber::quic::QuicStreamType::Bidirectional);
    EXPECT_FALSE(still_blocked.has_value());
    EXPECT_EQ(still_blocked.error(), fiber::common::IoErr::Busy);
    EXPECT_EQ(count_pending_streams_blocked(conn, fiber::quic::QuicFrameType::StreamsBlockedBidi, 3), 1U);
}

TEST(QuicConnectionTest, RecvStreamsBlockedDoesNotIncreasePeerStreamLimit) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    options.max_peer_bidirectional_streams = 1;
    fiber::quic::QuicConnection conn(options);

    fiber::quic::QuicStreamFrame first{};
    first.stream_id = 0;
    ASSERT_TRUE(conn.recv_stream_frame(first, {}).has_value());

    fiber::quic::QuicStreamsBlockedFrame blocked{};
    blocked.bidirectional = true;
    blocked.limit = 1;
    ASSERT_TRUE(conn.recv_streams_blocked_frame(blocked).has_value());

    fiber::quic::QuicStreamFrame second{};
    second.stream_id = 4;
    EXPECT_FALSE(conn.recv_stream_frame(second, {}).has_value());
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicFrameType::MaxStreamsBidi), 0U);
}

TEST(QuicConnectionTest, RejectsFinalSizeBelowReceivedStreamData) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    fiber::quic::QuicConnection conn(options);
    fiber::quic::QuicStreamFrame data{};
    data.stream_id = 0;
    data.offset = 10;
    data.has_offset = true;
    data.length = 3;
    fiber::quic::QuicStreamFrame fin{};
    fin.stream_id = 0;
    fin.length = 1;
    fin.fin = true;
    fiber::quic::QuicResetStreamFrame reset{};
    reset.id = 0;
    reset.final_size = 5;

    auto received = conn.recv_stream_frame(data, slice_of("abc"));
    ASSERT_TRUE(received.has_value());

    auto fin_conflict = conn.recv_stream_frame(fin, slice_of("x"));
    auto reset_conflict = conn.recv_reset_stream_frame(reset);

    EXPECT_FALSE(fin_conflict.has_value());
    EXPECT_FALSE(reset_conflict.has_value());
}

TEST(QuicConnectionTest, RejectsLocalOrLimitExceededPassiveStreams) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Server;
    options.max_peer_bidirectional_streams = 1;
    fiber::quic::QuicConnection conn(options);
    fiber::quic::QuicStreamFrame server_initiated{};
    server_initiated.stream_id = 1;
    fiber::quic::QuicStreamFrame over_limit{};
    over_limit.stream_id = 4;

    auto local = conn.recv_stream_frame(server_initiated, {});
    auto limited = conn.recv_stream_frame(over_limit, {});

    EXPECT_FALSE(local.has_value());
    EXPECT_FALSE(limited.has_value());
    EXPECT_EQ(conn.active_stream_count(), 0U);
}

// === Peer Connection ID pool (RFC 9000 §5.1, §19.15) ====================

namespace {

fiber::quic::QuicNewConnectionIdFrame make_new_cid_frame(std::uint64_t sequence_number, std::uint64_t retire_prior_to,
                                                         std::initializer_list<std::uint8_t> cid_bytes,
                                                         std::uint8_t token_pattern) {
    fiber::quic::QuicNewConnectionIdFrame frame{};
    frame.sequence_number = sequence_number;
    frame.retire_prior_to = retire_prior_to;
    frame.cid_len = static_cast<std::uint8_t>(cid_bytes.size());
    std::size_t i = 0;
    for (std::uint8_t byte: cid_bytes) {
        frame.cid[i++] = byte;
    }
    for (std::size_t j = 0; j < fiber::quic::kStatelessResetTokenLength; ++j) {
        frame.stateless_reset_token[j] = static_cast<std::uint8_t>(token_pattern ^ static_cast<std::uint8_t>(j));
    }
    return frame;
}

fiber::quic::QuicConnection::Options peer_pool_options() {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Client;
    options.local_addr = loopback(4433);
    options.remote_addr = loopback(5555);
    options.remote_connection_id = cid_from({0x10, 0x20, 0x30, 0x40});
    return options;
}

std::size_t count_pending_retire_for(const fiber::quic::QuicConnection &conn, std::uint64_t sequence_number) {
    const auto &space = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    std::size_t count = 0;
    for (const fiber::quic::QuicOutputFrame *frame = space.pending_frames.front(); frame != nullptr;
         frame = space.pending_frames.next_of(*frame)) {
        if (frame->type == fiber::quic::QuicFrameType::RetireConnectionId &&
            frame->u.retire_connection_id.sequence_number == sequence_number) {
            ++count;
        }
    }
    return count;
}

} // namespace

TEST(QuicConnectionTest, PeerCidPoolInstallsNewConnectionIdFrame) {
    fiber::quic::QuicConnection conn(peer_pool_options());
    auto frame = make_new_cid_frame(1, 0, {0xaa, 0xbb, 0xcc, 0xdd}, 0x42);

    auto result = conn.recv_new_connection_id_frame(frame);

    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    EXPECT_FALSE(*result);
    EXPECT_EQ(count_pending_retire_for(conn, 1), 0U);
}

TEST(QuicConnectionTest, PeerCidPoolIgnoresRetransmittedNewConnectionId) {
    fiber::quic::QuicConnection conn(peer_pool_options());
    auto frame = make_new_cid_frame(1, 0, {0xaa, 0xbb, 0xcc, 0xdd}, 0x42);

    ASSERT_TRUE(conn.recv_new_connection_id_frame(frame).has_value());
    auto second = conn.recv_new_connection_id_frame(frame);

    ASSERT_TRUE(second.has_value());
    EXPECT_FALSE(*second);
}

TEST(QuicConnectionTest, PeerCidPoolRejectsConflictingDuplicateSeqnum) {
    fiber::quic::QuicConnection conn(peer_pool_options());
    ASSERT_TRUE(conn.recv_new_connection_id_frame(make_new_cid_frame(1, 0, {0xaa, 0xbb}, 0x42)).has_value());

    auto conflict = conn.recv_new_connection_id_frame(make_new_cid_frame(1, 0, {0x11, 0x22}, 0x42));

    EXPECT_FALSE(conflict.has_value());
    EXPECT_EQ(static_cast<std::uint64_t>(conn.close_error()),
              static_cast<std::uint64_t>(fiber::quic::QuicErrorCode::ProtocolViolation));
}

TEST(QuicConnectionTest, PeerCidPoolRejectsZeroLengthCid) {
    fiber::quic::QuicConnection conn(peer_pool_options());
    fiber::quic::QuicNewConnectionIdFrame frame{};
    frame.sequence_number = 1;
    frame.cid_len = 0;

    auto result = conn.recv_new_connection_id_frame(frame);

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(static_cast<std::uint64_t>(conn.close_error()),
              static_cast<std::uint64_t>(fiber::quic::QuicErrorCode::FrameEncodingError));
}

TEST(QuicConnectionTest, PeerCidPoolRejectsRetirePriorToAboveSeqnum) {
    fiber::quic::QuicConnection conn(peer_pool_options());
    auto bad = make_new_cid_frame(1, 5, {0xaa, 0xbb}, 0x42);

    auto result = conn.recv_new_connection_id_frame(bad);

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(static_cast<std::uint64_t>(conn.close_error()),
              static_cast<std::uint64_t>(fiber::quic::QuicErrorCode::FrameEncodingError));
}

TEST(QuicConnectionTest, PeerCidPoolImmediatelyRetiresBelowMaxRetiredSeqnum) {
    fiber::quic::QuicConnection conn(peer_pool_options());
    // Bump max_retired_remote_seq_ to 3 via a frame with seq=5, retire_prior_to=3.
    ASSERT_TRUE(conn.recv_new_connection_id_frame(make_new_cid_frame(5, 3, {0x55, 0x55}, 0x42)).has_value());

    // A late NEW_CONNECTION_ID with seq=2 (< 3) MUST be acked with RETIRE.
    auto result = conn.recv_new_connection_id_frame(make_new_cid_frame(2, 0, {0x22, 0x22}, 0x33));

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(*result);
    EXPECT_EQ(count_pending_retire_for(conn, 2), 1U);
}

TEST(QuicConnectionTest, PeerCidPoolAppliesRetirePriorToAndQueuesRetires) {
    fiber::quic::QuicConnection conn(peer_pool_options());
    ASSERT_TRUE(conn.recv_new_connection_id_frame(make_new_cid_frame(1, 0, {0xa1, 0xb1}, 0x11)).has_value());
    ASSERT_TRUE(conn.recv_new_connection_id_frame(make_new_cid_frame(2, 0, {0xa2, 0xb2}, 0x22)).has_value());

    auto result = conn.recv_new_connection_id_frame(make_new_cid_frame(3, 2, {0xa3, 0xb3}, 0x33));

    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    EXPECT_TRUE(*result);
    // seq 0 (initial DCID, in pool, bound to active path) and seq 1 are < 2 and must retire.
    EXPECT_EQ(count_pending_retire_for(conn, 0), 1U);
    EXPECT_EQ(count_pending_retire_for(conn, 1), 1U);
    EXPECT_EQ(count_pending_retire_for(conn, 2), 0U);
    EXPECT_EQ(count_pending_retire_for(conn, 3), 0U);
}

TEST(QuicConnectionTest, PeerCidPoolSwitchesActivePathToReplacementOnRetire) {
    fiber::quic::QuicConnection conn(peer_pool_options());
    // Provide a replacement CID, then ask to retire seq=0 (the active path's CID).
    auto replacement = make_new_cid_frame(1, 0, {0xaa, 0xbb, 0xcc}, 0x42);
    ASSERT_TRUE(conn.recv_new_connection_id_frame(replacement).has_value());

    // Frame with seq=2 and retire_prior_to=1 retires seq=0 (active).
    auto migrate = make_new_cid_frame(2, 1, {0x77, 0x88}, 0x99);
    auto result = conn.recv_new_connection_id_frame(migrate);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(*result);
    auto *path = conn.active_path();
    ASSERT_NE(path, nullptr);
    EXPECT_EQ(path->remote_connection_id_sequence, 1U);
    ASSERT_EQ(path->remote_connection_id.size(), 3U);
    EXPECT_EQ(path->remote_connection_id.data()[0], 0xaa);
    EXPECT_EQ(conn.remote_connection_id().size(), 3U);
    EXPECT_EQ(conn.remote_connection_id().data()[0], 0xaa);
}

TEST(QuicConnectionTest, PeerCidPoolEnforcesActiveConnectionIdLimit) {
    auto options = peer_pool_options();
    // Connection's advertised active_connection_id_limit defaults to 4 — pool
    // contains slot 0 plus up to 3 more. Push past the limit.
    options.transport.active_connection_id_limit = 3;
    fiber::quic::QuicConnection conn(options);

    ASSERT_TRUE(conn.recv_new_connection_id_frame(make_new_cid_frame(1, 0, {0x01}, 0x11)).has_value());
    ASSERT_TRUE(conn.recv_new_connection_id_frame(make_new_cid_frame(2, 0, {0x02}, 0x22)).has_value());
    // pool now holds {0, 1, 2} == limit. Next one violates the bound.
    auto fourth = conn.recv_new_connection_id_frame(make_new_cid_frame(3, 0, {0x03}, 0x33));

    EXPECT_FALSE(fourth.has_value());
    EXPECT_EQ(static_cast<std::uint64_t>(conn.close_error()),
              static_cast<std::uint64_t>(fiber::quic::QuicErrorCode::ConnectionIdLimitError));
}

TEST(QuicConnectionTest, PeerCidPoolRetransmitsRetireOnlyWhenSlotEvicted) {
    fiber::quic::QuicConnection conn(peer_pool_options());
    ASSERT_TRUE(conn.recv_new_connection_id_frame(make_new_cid_frame(1, 0, {0xaa}, 0x11)).has_value());

    // While the slot is still in the pool, a lost RETIRE for seq=1 would be
    // redundant (the peer will re-issue via NEW_CONNECTION_ID).
    EXPECT_FALSE(conn.should_retransmit_retire_connection_id(1));

    // Trigger retire of seq=1 by issuing a frame with retire_prior_to > 1.
    ASSERT_TRUE(conn.recv_new_connection_id_frame(make_new_cid_frame(2, 2, {0xbb}, 0x22)).has_value());
    EXPECT_TRUE(conn.should_retransmit_retire_connection_id(1));
}
