#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <new>
#include <string_view>

#include "quic/QuicConnection.h"
#include "quic/QuicProtocol.h"
#include "quic/QuicTransportCodec.h"
#include "quic/QuicTransportParamsCodec.h"

namespace {

fiber::net::SocketAddress loopback(std::uint16_t port) { return {fiber::net::IpAddress::loopback_v4(), port}; }

fiber::quic::QuicConnectionId cid_from(std::initializer_list<std::uint8_t> bytes) {
    auto cid = fiber::quic::QuicConnectionId::from_bytes(bytes.begin(), bytes.size());
    return cid.value_or(fiber::quic::QuicConnectionId{});
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
    std::uint32_t schedule_calls = 0;
    std::uint64_t last_stream_id = 0;
    bool return_empty = false;
    fiber::quic::QuicStream::Lease lease{};
};

fiber::quic::QuicStream::Lease make_test_stream(const fiber::quic::QuicNewStreamContext &ctx) noexcept {
    return fiber::quic::QuicStream::Lease::adopt(
            new fiber::quic::QuicStream(ctx.stream_id, ctx.recv_extent_pool, ctx.recv_options));
}

fiber::quic::QuicStream::Lease on_new_stream_record(void *owner,
                                                    const fiber::quic::QuicNewStreamContext &ctx) noexcept {
    auto *state = static_cast<StreamCallbackState *>(owner);
    ++state->calls;
    state->last_stream_id = ctx.stream_id;
    if (state->return_empty) {
        return {};
    }
    return make_test_stream(ctx);
}

fiber::quic::QuicStream::Lease on_new_stream_retain(void *owner,
                                                    const fiber::quic::QuicNewStreamContext &ctx) noexcept {
    auto *state = static_cast<StreamCallbackState *>(owner);
    ++state->calls;
    state->last_stream_id = ctx.stream_id;
    auto *stream = new (std::nothrow) fiber::quic::QuicStream(ctx.stream_id, ctx.recv_extent_pool, ctx.recv_options);
    if (stream == nullptr) {
        return {};
    }
    state->lease = stream->lease();
    return fiber::quic::QuicStream::Lease::adopt(stream);
}

fiber::quic::QuicConnection::Options server_options_with_factory(StreamCallbackState &state) noexcept {
    fiber::quic::QuicConnection::Options options{};
    options.role = fiber::quic::QuicConnectionRole::Server;
    options.owner = &state;
    options.ops.on_new_stream = on_new_stream_record;
    return options;
}

void schedule_send_record(void *owner, fiber::quic::QuicConnection &) noexcept {
    ++static_cast<StreamCallbackState *>(owner)->schedule_calls;
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
    fiber::quic::QuicConnection::Options options{};
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
    fiber::quic::QuicConnection::Options options{};
    options.role = fiber::quic::QuicConnectionRole::Server;
    fiber::quic::QuicConnection conn(options);

    auto stream_id = conn.next_local_stream_id(fiber::quic::QuicStreamType::Unidirectional);

    ASSERT_TRUE(stream_id.has_value());
    EXPECT_EQ(*stream_id, 3U);
    EXPECT_TRUE(fiber::quic::QuicConnection::is_unidirectional_stream(*stream_id));
    EXPECT_TRUE(conn.is_local_stream(*stream_id));
}

TEST(QuicConnectionTest, InitializesThreePacketNumberSpaces) {
    fiber::quic::QuicConnection conn(fiber::quic::QuicConnection::Options{});

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
    fiber::quic::QuicConnection conn(fiber::quic::QuicConnection::Options{});

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
    fiber::quic::QuicConnection conn(fiber::quic::QuicConnection::Options{});

    auto &initial = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial);
    auto &handshake = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Handshake);

    EXPECT_EQ(fiber::quic::quic_use_next_packet_number(initial), 0U);
    EXPECT_EQ(fiber::quic::quic_use_next_packet_number(initial), 1U);
    EXPECT_EQ(fiber::quic::quic_use_next_packet_number(handshake), 0U);
    EXPECT_EQ(initial.next_packet_number, 2U);
    EXPECT_EQ(handshake.next_packet_number, 1U);
}

TEST(QuicConnectionTest, QueuesFramesIntrusively) {
    fiber::quic::QuicConnection conn(fiber::quic::QuicConnection::Options{});
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
    options.schedule_send_owner = &state;
    options.schedule_send = schedule_send_record;
    fiber::quic::QuicConnection conn(options);

    auto stream = conn.get_or_create_peer_stream(0);
    ASSERT_TRUE(stream.has_value());
    fiber::quic::QuicMaxStreamDataFrame limit{};
    limit.id = 0;
    limit.limit = 1024;
    ASSERT_TRUE(conn.recv_max_stream_data_frame(limit).has_value());

    auto written = (*stream)->try_write(iobuf_of("abc"));

    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(*written, 3u);
    EXPECT_TRUE(conn.has_stream_send_work());
    auto &space = conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    EXPECT_TRUE(space.pending_frames.empty());
    ASSERT_NE(conn.peek_stream_send_work(), nullptr);
    EXPECT_EQ(conn.peek_stream_send_work()->stream_id(), 0u);
    EXPECT_EQ(state.schedule_calls, 1u);
}

TEST(QuicConnectionTest, RecvFlowDefaultsInitializeLocalTransportAndLimit) {
    fiber::quic::QuicConnection conn(fiber::quic::QuicConnection::Options{});

    EXPECT_EQ(conn.recv_data_limit(), fiber::quic::kQuicDefaultConnRecvLimit);
    EXPECT_EQ(conn.local_transport().initial_max_data, fiber::quic::kQuicDefaultConnRecvLimit);
    EXPECT_EQ(conn.local_transport().initial_max_stream_data_bidi_local, fiber::quic::kQuicDefaultStreamBufferSize);
    EXPECT_EQ(conn.local_transport().initial_max_stream_data_bidi_remote, fiber::quic::kQuicDefaultStreamBufferSize);
    EXPECT_EQ(conn.local_transport().initial_max_stream_data_uni, fiber::quic::kQuicDefaultStreamBufferSize);
}

TEST(QuicConnectionTest, CreatesInitialActivePathFromOptions) {
    fiber::quic::QuicConnection::Options options{};
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
    fiber::quic::QuicConnection conn(fiber::quic::QuicConnection::Options{});
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

TEST(QuicConnectionTest, ReplacesProbePathWhenCreatingAnotherProbe) {
    fiber::quic::QuicConnection conn(fiber::quic::QuicConnection::Options{});
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

TEST(QuicConnectionTest, AppliesPeerTransportParamsAndUpdatesLocalStreamLimits) {
    fiber::quic::QuicConnection::Options options{};
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

TEST(QuicConnectionTest, RejectsPeerTransportParamsWithMismatchedInitialScid) {
    fiber::quic::QuicConnection::Options options{};
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
    options.schedule_send_owner = &state;
    options.schedule_send = schedule_send_record;
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
    EXPECT_EQ(state.schedule_calls, 1U);
}

TEST(QuicConnectionTest, StreamReadExtendsStreamFlowControlOnly) {
    StreamCallbackState state{};
    auto options = server_options_with_factory(state);
    options.recv_flow.conn_recv_limit = 100;
    options.recv_flow.conn_recv_low_water = 0;
    options.recv_flow.stream_buffer_limit = 8;
    options.recv_flow.stream_low_water = 3;
    options.schedule_send_owner = &state;
    options.schedule_send = schedule_send_record;
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
    EXPECT_EQ(state.schedule_calls, 1U);
}

TEST(QuicConnectionTest, RejectsPassiveStreamWhenConnectionOpsIsMissing) {
    fiber::quic::QuicConnection::Options options{};
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
    fiber::quic::QuicConnection::Options options{};
    options.role = fiber::quic::QuicConnectionRole::Server;
    options.owner = &state;
    options.ops.on_new_stream = on_new_stream_record;
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
    fiber::quic::QuicConnection::Options options{};
    options.role = fiber::quic::QuicConnectionRole::Server;
    options.owner = &state;
    options.ops.on_new_stream = on_new_stream_retain;
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
    EXPECT_FALSE(conflict.has_value());
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
    fiber::quic::QuicConnection::Options options{};
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
