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
    params.has_preferred_address = true;
    params.preferred_address.ipv4 = {fiber::net::IpAddress::v4({192, 0, 2, 10}), 8443};
    params.preferred_address.ipv6 = {
            fiber::net::IpAddress::v6({0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}), 9443};
    params.preferred_address.connection_id = cid_from({0x31, 0x32, 0x33, 0x34});
    for (std::size_t i = 0; i < fiber::quic::kStatelessResetTokenLength; ++i) {
        params.preferred_address.stateless_reset_token[i] = static_cast<std::uint8_t>(0xa0 + i);
    }

    std::array<std::uint8_t, 512> buf{};
    fiber::quic::QuicWriteCursor out(buf.data(), buf.size());
    std::size_t zero_rtt_len = 0;

    auto written = fiber::quic::quic_create_transport_params(fiber::quic::QuicTransportParamOwner::Server, &out, params,
                                                             &zero_rtt_len);

    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(*written, out.offset());
    EXPECT_GT(zero_rtt_len, 0U);
    EXPECT_LT(zero_rtt_len, *written);

    fiber::quic::QuicReadCursor zero_rtt_in(buf.data(), zero_rtt_len);
    fiber::quic::QuicTransportParams zero_rtt{};
    auto zero_rtt_read = fiber::quic::quic_parse_transport_params(fiber::quic::QuicTransportParamOwner::Server,
                                                                  zero_rtt_in, zero_rtt);

    ASSERT_TRUE(zero_rtt_read.has_value());
    EXPECT_EQ(zero_rtt.initial_max_data, params.initial_max_data);
    EXPECT_EQ(zero_rtt.initial_max_stream_data_bidi_local, params.initial_max_stream_data_bidi_local);
    EXPECT_EQ(zero_rtt.initial_max_stream_data_bidi_remote, params.initial_max_stream_data_bidi_remote);
    EXPECT_EQ(zero_rtt.initial_max_stream_data_uni, params.initial_max_stream_data_uni);
    EXPECT_EQ(zero_rtt.initial_max_streams_bidi, params.initial_max_streams_bidi);
    EXPECT_EQ(zero_rtt.initial_max_streams_uni, params.initial_max_streams_uni);
    EXPECT_EQ(zero_rtt.max_idle_timeout, params.max_idle_timeout);
    EXPECT_EQ(zero_rtt.max_udp_payload_size, params.max_udp_payload_size);
    EXPECT_EQ(zero_rtt.active_connection_id_limit, params.active_connection_id_limit);
    EXPECT_TRUE(zero_rtt.disable_active_migration);
    EXPECT_EQ(zero_rtt.ack_delay_exponent, 3U);
    EXPECT_EQ(zero_rtt.max_ack_delay, 25U);
    EXPECT_FALSE(zero_rtt.has_initial_source_connection_id);
    EXPECT_FALSE(zero_rtt.has_original_destination_connection_id);
    EXPECT_FALSE(zero_rtt.has_retry_source_connection_id);
    EXPECT_FALSE(zero_rtt.has_stateless_reset_token);
    EXPECT_FALSE(zero_rtt.has_preferred_address);

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
    ASSERT_TRUE(parsed.has_preferred_address);
    EXPECT_EQ(parsed.preferred_address.ipv4.port(), 8443);
    EXPECT_EQ(parsed.preferred_address.ipv4.ip().v4_bytes(), params.preferred_address.ipv4.ip().v4_bytes());
    EXPECT_EQ(parsed.preferred_address.ipv6.port(), 9443);
    EXPECT_EQ(parsed.preferred_address.ipv6.ip().v6_bytes(), params.preferred_address.ipv6.ip().v6_bytes());
    EXPECT_EQ(parsed.preferred_address.connection_id.size(), 4U);
    EXPECT_EQ(parsed.preferred_address.stateless_reset_token[5], 0xa5);
}

TEST(QuicTransportParamsCodecTest, RejectsPreferredAddressWithEmptyConnectionId) {
    fiber::quic::QuicTransportParams params{};
    params.has_preferred_address = true;
    std::array<std::uint8_t, 256> buf{};
    fiber::quic::QuicWriteCursor out(buf.data(), buf.size());

    auto written =
            fiber::quic::quic_create_transport_params(fiber::quic::QuicTransportParamOwner::Server, &out, params);

    EXPECT_FALSE(written.has_value());
    EXPECT_EQ(written.error(), fiber::common::IoErr::Invalid);
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

TEST(QuicTransportParamsCodecTest, RejectsDuplicateValueParam) {
    std::array<std::uint8_t, 16> buf{};
    fiber::quic::QuicWriteCursor out(buf.data(), buf.size());
    for (const std::uint64_t value: {1U, 2U}) {
        ASSERT_TRUE(fiber::quic::quic_write_varint(out, fiber::quic::kQuicTpInitialMaxData).has_value());
        ASSERT_TRUE(fiber::quic::quic_write_varint(out, 1).has_value());
        ASSERT_TRUE(fiber::quic::quic_write_varint(out, value).has_value());
    }

    fiber::quic::QuicReadCursor in(buf.data(), out.offset());
    fiber::quic::QuicTransportParams parsed{};
    auto read = fiber::quic::quic_parse_transport_params(fiber::quic::QuicTransportParamOwner::Client, in, parsed);

    EXPECT_FALSE(read.has_value());
    EXPECT_EQ(read.error(), fiber::common::IoErr::Invalid);
}

TEST(QuicTransportParamsCodecTest, RejectsDuplicateEmptyParam) {
    const std::array<std::uint8_t, 4> bytes{
            static_cast<std::uint8_t>(fiber::quic::kQuicTpDisableActiveMigration),
            0,
            static_cast<std::uint8_t>(fiber::quic::kQuicTpDisableActiveMigration),
            0,
    };
    fiber::quic::QuicReadCursor in(bytes.data(), bytes.size());
    fiber::quic::QuicTransportParams parsed{};

    auto read = fiber::quic::quic_parse_transport_params(fiber::quic::QuicTransportParamOwner::Client, in, parsed);

    EXPECT_FALSE(read.has_value());
    EXPECT_EQ(read.error(), fiber::common::IoErr::Invalid);
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

TEST(QuicTransportParamsCodecTest, RejectsInvalidConstrainedParamsOnParse) {
    struct InvalidParam {
        std::uint64_t id;
        std::uint64_t value;
    };
    constexpr std::array<InvalidParam, 5> invalid_params{{
            {fiber::quic::kQuicTpMaxUdpPayloadSize, fiber::quic::kMinInitialDatagramSize - 1},
            {fiber::quic::kQuicTpMaxUdpPayloadSize, fiber::quic::kQuicMaxUdpPayloadSize + 1},
            {fiber::quic::kQuicTpAckDelayExponent, 21},
            {fiber::quic::kQuicTpMaxAckDelay, 1ULL << 14U},
            {fiber::quic::kQuicTpActiveConnectionIdLimit, 1},
    }};

    for (const InvalidParam invalid: invalid_params) {
        std::array<std::uint8_t, 32> buf{};
        fiber::quic::QuicWriteCursor out(buf.data(), buf.size());
        ASSERT_TRUE(fiber::quic::quic_write_varint(out, invalid.id).has_value());
        ASSERT_TRUE(fiber::quic::quic_write_varint(out, fiber::quic::quic_varint_len(invalid.value)).has_value());
        ASSERT_TRUE(fiber::quic::quic_write_varint(out, invalid.value).has_value());

        fiber::quic::QuicReadCursor in(buf.data(), out.offset());
        fiber::quic::QuicTransportParams parsed{};
        auto read = fiber::quic::quic_parse_transport_params(fiber::quic::QuicTransportParamOwner::Client, in, parsed);

        EXPECT_FALSE(read.has_value()) << "transport parameter id=" << invalid.id << " value=" << invalid.value;
    }
}

TEST(QuicTransportParamsCodecTest, RejectsInvalidConstrainedParamsOnCreate) {
    const auto expect_invalid = [](const fiber::quic::QuicTransportParams &params) {
        std::array<std::uint8_t, 256> buf{};
        fiber::quic::QuicWriteCursor out(buf.data(), buf.size());
        auto written =
                fiber::quic::quic_create_transport_params(fiber::quic::QuicTransportParamOwner::Client, &out, params);
        EXPECT_FALSE(written.has_value());
    };

    fiber::quic::QuicTransportParams params{};
    params.max_udp_payload_size = fiber::quic::kMinInitialDatagramSize - 1;
    expect_invalid(params);

    params = {};
    params.max_udp_payload_size = fiber::quic::kQuicMaxUdpPayloadSize + 1;
    expect_invalid(params);

    params = {};
    params.ack_delay_exponent = 21;
    expect_invalid(params);

    params = {};
    params.max_ack_delay = 1ULL << 14U;
    expect_invalid(params);

    params = {};
    params.active_connection_id_limit = 1;
    expect_invalid(params);
}

TEST(QuicTransportParamsCodecTest, AcceptsConstrainedParamBoundaries) {
    fiber::quic::QuicTransportParams params{};
    params.max_udp_payload_size = fiber::quic::kMinInitialDatagramSize;
    params.ack_delay_exponent = 20;
    params.max_ack_delay = (1ULL << 14U) - 1;
    params.active_connection_id_limit = 2;

    std::array<std::uint8_t, 256> buf{};
    fiber::quic::QuicWriteCursor out(buf.data(), buf.size());
    auto written =
            fiber::quic::quic_create_transport_params(fiber::quic::QuicTransportParamOwner::Client, &out, params);
    ASSERT_TRUE(written.has_value());

    fiber::quic::QuicReadCursor in(buf.data(), out.offset());
    fiber::quic::QuicTransportParams parsed{};
    auto read = fiber::quic::quic_parse_transport_params(fiber::quic::QuicTransportParamOwner::Client, in, parsed);

    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(parsed.max_udp_payload_size, params.max_udp_payload_size);
    EXPECT_EQ(parsed.ack_delay_exponent, params.ack_delay_exponent);
    EXPECT_EQ(parsed.max_ack_delay, params.max_ack_delay);
    EXPECT_EQ(parsed.active_connection_id_limit, params.active_connection_id_limit);
}
