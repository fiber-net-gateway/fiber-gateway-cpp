#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "quic/QuicTransportCodec.h"
#include "quic/QuicTransportParamsCodec.h"

namespace {

fiber::quic::QuicConnectionId cid_from(std::initializer_list<std::uint8_t> bytes) {
    auto cid = fiber::quic::QuicConnectionId::from_bytes(bytes.begin(), bytes.size());
    EXPECT_TRUE(cid.has_value());
    return cid.value_or(fiber::quic::QuicConnectionId{});
}

} // namespace

TEST(QuicTransportParamsCodecTest, ServerParamsRoundTrip) {
    fiber::quic::QuicTransportParams params{};
    params.max_idle_timeout = 30000;
    params.max_udp_payload_size = 1400;
    params.initial_max_data = 65536;
    params.initial_max_stream_data_bidi_local = 4096;
    params.initial_max_stream_data_bidi_remote = 8192;
    params.initial_max_stream_data_uni = 2048;
    params.initial_max_streams_bidi = 64;
    params.initial_max_streams_uni = 16;
    params.ack_delay_exponent = 4;
    params.max_ack_delay = 20;
    params.active_connection_id_limit = 4;
    params.disable_active_migration = true;
    params.has_original_destination_connection_id = true;
    params.original_destination_connection_id = cid_from({0x01, 0x02, 0x03, 0x04});
    params.has_initial_source_connection_id = true;
    params.initial_source_connection_id = cid_from({0x11, 0x12});
    params.has_retry_source_connection_id = true;
    params.retry_source_connection_id = cid_from({0x21, 0x22, 0x23});
    params.has_stateless_reset_token = true;
    for (std::size_t i = 0; i < fiber::quic::kStatelessResetTokenLength; ++i) {
        params.stateless_reset_token[i] = static_cast<std::uint8_t>(0x80 + i);
    }

    std::array<std::uint8_t, 256> buf{};
    fiber::quic::QuicWriteCursor out(buf.data(), buf.size());
    std::size_t zero_rtt_len = 0;

    auto written = fiber::quic::quic_create_transport_params(fiber::quic::QuicTransportParamOwner::Server, &out, params,
                                                             &zero_rtt_len);

    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(*written, out.offset());
    EXPECT_GT(zero_rtt_len, 0U);
    EXPECT_LT(zero_rtt_len, *written);

    fiber::quic::QuicReadCursor in(buf.data(), out.offset());
    fiber::quic::QuicTransportParams parsed{};
    auto read = fiber::quic::quic_parse_transport_params(fiber::quic::QuicTransportParamOwner::Server, in, parsed);

    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(parsed.max_idle_timeout, params.max_idle_timeout);
    EXPECT_EQ(parsed.max_udp_payload_size, params.max_udp_payload_size);
    EXPECT_EQ(parsed.initial_max_data, params.initial_max_data);
    EXPECT_EQ(parsed.initial_max_stream_data_bidi_local, params.initial_max_stream_data_bidi_local);
    EXPECT_EQ(parsed.initial_max_stream_data_bidi_remote, params.initial_max_stream_data_bidi_remote);
    EXPECT_EQ(parsed.initial_max_stream_data_uni, params.initial_max_stream_data_uni);
    EXPECT_EQ(parsed.initial_max_streams_bidi, params.initial_max_streams_bidi);
    EXPECT_EQ(parsed.initial_max_streams_uni, params.initial_max_streams_uni);
    EXPECT_EQ(parsed.ack_delay_exponent, params.ack_delay_exponent);
    EXPECT_EQ(parsed.max_ack_delay, params.max_ack_delay);
    EXPECT_EQ(parsed.active_connection_id_limit, params.active_connection_id_limit);
    EXPECT_TRUE(parsed.disable_active_migration);
    EXPECT_TRUE(parsed.has_original_destination_connection_id);
    EXPECT_EQ(parsed.original_destination_connection_id.size(), params.original_destination_connection_id.size());
    EXPECT_TRUE(parsed.has_initial_source_connection_id);
    EXPECT_EQ(parsed.initial_source_connection_id.size(), params.initial_source_connection_id.size());
    EXPECT_TRUE(parsed.has_retry_source_connection_id);
    EXPECT_EQ(parsed.retry_source_connection_id.size(), params.retry_source_connection_id.size());
    EXPECT_TRUE(parsed.has_stateless_reset_token);
    EXPECT_EQ(parsed.stateless_reset_token[3], params.stateless_reset_token[3]);
}

TEST(QuicTransportParamsCodecTest, RejectsServerOnlyParamsFromClient) {
    const std::array<std::uint8_t, 4> bytes{
            static_cast<std::uint8_t>(fiber::quic::kQuicTpStatelessResetToken),
            fiber::quic::kStatelessResetTokenLength,
            0,
            0,
    };
    fiber::quic::QuicReadCursor in(bytes.data(), bytes.size());
    fiber::quic::QuicTransportParams parsed{};

    auto read = fiber::quic::quic_parse_transport_params(fiber::quic::QuicTransportParamOwner::Client, in, parsed);

    EXPECT_FALSE(read.has_value());
}

TEST(QuicTransportParamsCodecTest, SkipsUnknownTransportParam) {
    std::array<std::uint8_t, 8> buf{};
    fiber::quic::QuicWriteCursor out(buf.data(), buf.size());
    ASSERT_TRUE(fiber::quic::quic_write_varint(out, 0x20).has_value());
    ASSERT_TRUE(fiber::quic::quic_write_varint(out, 1).has_value());
    ASSERT_TRUE(out.write_u8(0xaa).has_value());

    fiber::quic::QuicReadCursor in(buf.data(), out.offset());
    fiber::quic::QuicTransportParams parsed{};
    auto read = fiber::quic::quic_parse_transport_params(fiber::quic::QuicTransportParamOwner::Client, in, parsed);

    EXPECT_TRUE(read.has_value());
}

TEST(QuicTransportParamsCodecTest, RejectsInitialMaxStreamsAboveProtocolLimit) {
    fiber::quic::QuicTransportParams params{};
    params.initial_max_streams_bidi = fiber::quic::kQuicMaxStreamLimit + 1;

    std::array<std::uint8_t, 64> buf{};
    fiber::quic::QuicWriteCursor out(buf.data(), buf.size());
    auto written =
            fiber::quic::quic_create_transport_params(fiber::quic::QuicTransportParamOwner::Client, &out, params);

    EXPECT_FALSE(written.has_value());

    fiber::quic::QuicWriteCursor param_out(buf.data(), buf.size());
    ASSERT_TRUE(fiber::quic::quic_write_varint(param_out, fiber::quic::kQuicTpInitialMaxStreamsBidi).has_value());
    ASSERT_TRUE(fiber::quic::quic_write_varint(param_out, fiber::quic::quic_varint_len(params.initial_max_streams_bidi))
                        .has_value());
    ASSERT_TRUE(fiber::quic::quic_write_varint(param_out, params.initial_max_streams_bidi).has_value());

    fiber::quic::QuicReadCursor in(buf.data(), param_out.offset());
    fiber::quic::QuicTransportParams parsed{};
    auto read = fiber::quic::quic_parse_transport_params(fiber::quic::QuicTransportParamOwner::Client, in, parsed);

    EXPECT_FALSE(read.has_value());
}
