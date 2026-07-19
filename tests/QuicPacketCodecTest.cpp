#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

#include "quic/QuicCrypto.h"
#include "quic/QuicPacketCodec.h"
#include "quic/QuicTransportCodec.h"

#include "QuicTestLoop.h"

namespace {

struct DecodedPayloadSummary {
    std::uint32_t frame_count = 0;
    bool ack_eliciting = false;
};

fiber::common::IoResult<DecodedPayloadSummary>
summarize_decoded_payload(fiber::quic::QuicConnectionRole receiver_role,
                          const fiber::quic::QuicPacketDecodeResult &decoded) noexcept {
    DecodedPayloadSummary summary{};
    fiber::quic::QuicReadCursor payload(decoded.payload.readable_data(), decoded.payload.readable());
    while (!payload.empty()) {
        auto parsed = fiber::quic::quic_parse_frame_for_receiver(receiver_role, decoded.header.level, payload);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        ++summary.frame_count;
        summary.ack_eliciting = summary.ack_eliciting || parsed->frame.ack_eliciting;
    }
    return summary;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::vector<std::uint8_t> hex(std::string_view value) {
    std::vector<std::uint8_t> out;
    out.reserve(value.size() / 2);
    int high = -1;
    for (char c: value) {
        const int v = hex_value(c);
        if (v < 0) {
            continue;
        }
        if (high < 0) {
            high = v;
        } else {
            out.push_back(static_cast<std::uint8_t>((high << 4U) | v));
            high = -1;
        }
    }
    EXPECT_LT(high, 0);
    return out;
}

fiber::quic::QuicConnectionId cid_from_hex(std::string_view value) {
    auto bytes = hex(value);
    auto cid = fiber::quic::QuicConnectionId::from_bytes(bytes.data(), bytes.size());
    EXPECT_TRUE(cid.has_value());
    return cid.value_or(fiber::quic::QuicConnectionId{});
}

} // namespace

TEST(QuicPacketCodecTest, EncodesAndDecodesProtectedInitialPacket) {
    const auto original_dcid = cid_from_hex("8394c8f03e515708");

    fiber::quic::QuicConnection::Options server_options = fiber::test::quic_options();
    server_options.role = fiber::quic::QuicConnectionRole::Server;
    fiber::quic::QuicConnection server(server_options);
    ASSERT_TRUE(server.init_initial_crypto(original_dcid));

    fiber::quic::QuicConnection::Options client_options = fiber::test::quic_options();
    client_options.role = fiber::quic::QuicConnectionRole::Client;
    fiber::quic::QuicConnection client(client_options);
    ASSERT_TRUE(client.init_initial_crypto(original_dcid));

    fiber::quic::QuicOutputFrame frames[1]{};
    frames[0].type = fiber::quic::QuicFrameType::Ping;

    std::array<std::uint8_t, 1400> datagram{};
    std::array<std::uint8_t, 1400> encode_plaintext{};
    fiber::quic::QuicPacketEncodeSpec spec{};
    spec.level = fiber::quic::QuicEncryptionLevel::Initial;
    spec.dcid = original_dcid;
    spec.scid = cid_from_hex("11223344");
    spec.frames = frames;
    spec.frame_count = 1;
    spec.min_packet_len = fiber::quic::kMinInitialDatagramSize;

    auto encoded = fiber::quic::quic_encode_packet(server, spec, {encode_plaintext.data(), encode_plaintext.size()},
                                                   datagram.data(), datagram.size());

    ASSERT_TRUE(encoded.has_value()) << static_cast<int>(encoded.error());
    EXPECT_EQ(encoded->packet_number, 0U);
    EXPECT_GE(encoded->packet_len, fiber::quic::kMinInitialDatagramSize);
    EXPECT_TRUE(encoded->ack_eliciting);

    auto decoded = fiber::quic::quic_decode_packet(client, datagram.data(), encoded->packet_len, 0);

    ASSERT_TRUE(decoded.has_value()) << static_cast<int>(decoded.error());
    EXPECT_TRUE(decoded->payload.storage_trackable());
    EXPECT_EQ(decoded->header.type, fiber::quic::QuicPacketType::Initial);
    EXPECT_EQ(decoded->header.packet_number, 0U);
    std::fill(datagram.begin(), datagram.end(), 0);
    auto decoded_summary = summarize_decoded_payload(fiber::quic::QuicConnectionRole::Client, *decoded);
    ASSERT_TRUE(decoded_summary.has_value()) << static_cast<int>(decoded_summary.error());
    EXPECT_EQ(decoded_summary->frame_count, 2U);
    EXPECT_TRUE(decoded_summary->ack_eliciting);
    EXPECT_EQ(client.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial).largest_received_packet_number, 0U);
}

TEST(QuicPacketCodecTest, EncodesInitialPacketAboveFourKilobytes) {
    const auto original_dcid = cid_from_hex("8394c8f03e515708");

    fiber::quic::QuicConnection::Options server_options = fiber::test::quic_options();
    server_options.role = fiber::quic::QuicConnectionRole::Server;
    fiber::quic::QuicConnection server(server_options);
    ASSERT_TRUE(server.init_initial_crypto(original_dcid));

    fiber::quic::QuicOutputFrame frame{};
    frame.type = fiber::quic::QuicFrameType::Ping;

    std::array<std::uint8_t, 6000> datagram{};
    std::array<std::uint8_t, 6000> encode_plaintext{};
    fiber::quic::QuicPacketEncodeSpec spec{};
    spec.level = fiber::quic::QuicEncryptionLevel::Initial;
    spec.dcid = original_dcid;
    spec.scid = cid_from_hex("11223344");
    spec.frames = &frame;
    spec.frame_count = 1;
    spec.min_packet_len = 4800;

    auto encoded = fiber::quic::quic_encode_packet(server, spec, {encode_plaintext.data(), encode_plaintext.size()},
                                                   datagram.data(), datagram.size());

    ASSERT_TRUE(encoded.has_value()) << static_cast<int>(encoded.error());
    EXPECT_GE(encoded->packet_len, 4800U);
}

TEST(QuicPacketCodecTest, CreatesVersionNegotiationPacket) {
    fiber::quic::QuicPacketHeader request{};
    request.long_header = true;
    request.flags = fiber::quic::kPacketFlagLong | fiber::quic::kPacketFlagFixed;
    request.dcid = cid_from_hex("01020304");
    request.scid = cid_from_hex("112233");

    std::array<std::uint8_t, 64> buf{};
    fiber::quic::QuicWriteCursor out(buf.data(), buf.size());
    auto written = fiber::quic::quic_create_version_negotiation_packet(request, out);

    ASSERT_TRUE(written.has_value());
    ASSERT_GE(*written, 1U + 4U + 1U + request.dcid.size() + 1U + request.scid.size() + 4U);
    EXPECT_EQ(buf[1], 0U);
    EXPECT_EQ(buf[2], 0U);
    EXPECT_EQ(buf[3], 0U);
    EXPECT_EQ(buf[4], 0U);
}

TEST(QuicPacketCodecTest, CreatesAndParsesRetryPacket) {
    const std::array<std::uint8_t, 5> token{'t', 'o', 'k', 'e', 'n'};
    fiber::quic::QuicRetryPacketSpec spec{};
    spec.original_dcid = cid_from_hex("8394c8f03e515708");
    spec.dcid = cid_from_hex("01020304");
    spec.scid = cid_from_hex("11223344");
    spec.token = {token.data(), token.size()};

    std::array<std::uint8_t, 128> buf{};
    fiber::quic::QuicWriteCursor out(buf.data(), buf.size());
    auto written = fiber::quic::quic_create_retry_packet(spec, out);

    ASSERT_TRUE(written.has_value());
    auto parsed = fiber::quic::quic_parse_packet_header(buf.data(), *written, 0);

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->type, fiber::quic::QuicPacketType::Retry);
    EXPECT_EQ(parsed->token.len, token.size());
    EXPECT_EQ(parsed->token.data[0], 't');
    EXPECT_EQ(*written,
              1U + 4U + 1U + spec.dcid.size() + 1U + spec.scid.size() + token.size() + fiber::quic::kAeadTagLength);
}

class QuicPacketCodecSuiteTest : public ::testing::TestWithParam<fiber::quic::QuicCryptoSuite> {};

TEST_P(QuicPacketCodecSuiteTest, EncodesAndDecodesApplicationPacket) {
    const fiber::quic::QuicCryptoSuite suite = GetParam();
    std::array<std::uint8_t, fiber::quic::kQuicMaxSecretLength> secret{};
    const std::size_t secret_len = suite == fiber::quic::QuicCryptoSuite::Aes256GcmSha384 ? 48U : 32U;
    for (std::size_t i = 0; i < secret_len; ++i) {
        secret[i] = static_cast<std::uint8_t>(i + 1);
    }

    fiber::quic::QuicConnection::Options server_options = fiber::test::quic_options();
    server_options.role = fiber::quic::QuicConnectionRole::Server;
    fiber::quic::QuicConnection server(server_options);
    ASSERT_TRUE(fiber::quic::quic_set_encryption_secret(server.crypto(), fiber::quic::QuicEncryptionLevel::Application,
                                                        true, suite, secret.data(), secret_len));

    fiber::quic::QuicConnection::Options client_options = fiber::test::quic_options();
    client_options.role = fiber::quic::QuicConnectionRole::Client;
    fiber::quic::QuicConnection client(client_options);
    ASSERT_TRUE(fiber::quic::quic_set_encryption_secret(client.crypto(), fiber::quic::QuicEncryptionLevel::Application,
                                                        false, suite, secret.data(), secret_len));
    const auto server_keys =
            fiber::quic::quic_packet_keys(server.crypto(), fiber::quic::QuicEncryptionLevel::Application, true);
    const auto client_keys =
            fiber::quic::quic_packet_keys(client.crypto(), fiber::quic::QuicEncryptionLevel::Application, false);
    ASSERT_TRUE(server_keys);
    ASSERT_TRUE(client_keys);
    ASSERT_EQ(server_keys.header->key_len, client_keys.header->key_len);
    for (std::size_t i = 0; i < server_keys.header->key_len; ++i) {
        ASSERT_EQ(server_keys.header->key[i], client_keys.header->key[i]) << i;
    }

    fiber::quic::QuicOutputFrame frame{};
    frame.type = fiber::quic::QuicFrameType::Ping;

    std::array<std::uint8_t, 256> datagram{};
    std::array<std::uint8_t, 256> encode_plaintext{};
    fiber::quic::QuicPacketEncodeSpec spec{};
    spec.level = fiber::quic::QuicEncryptionLevel::Application;
    spec.dcid = cid_from_hex("01020304");
    spec.frames = &frame;
    spec.frame_count = 1;

    auto encoded = fiber::quic::quic_encode_packet(server, spec, {encode_plaintext.data(), encode_plaintext.size()},
                                                   datagram.data(), datagram.size());
    ASSERT_TRUE(encoded.has_value()) << static_cast<int>(encoded.error());

    auto decoded = fiber::quic::quic_decode_packet(client, datagram.data(), encoded->packet_len,
                                                   static_cast<std::uint8_t>(spec.dcid.size()));

    ASSERT_TRUE(decoded.has_value()) << static_cast<int>(decoded.error());
    EXPECT_EQ(decoded->header.type, fiber::quic::QuicPacketType::Short);
    EXPECT_EQ(decoded->header.packet_number, 0U);
    auto decoded_summary = summarize_decoded_payload(fiber::quic::QuicConnectionRole::Client, *decoded);
    ASSERT_TRUE(decoded_summary.has_value()) << static_cast<int>(decoded_summary.error());
    EXPECT_EQ(decoded_summary->frame_count, 2U);
    EXPECT_TRUE(decoded_summary->ack_eliciting);
}

INSTANTIATE_TEST_SUITE_P(All, QuicPacketCodecSuiteTest,
                         ::testing::Values(fiber::quic::QuicCryptoSuite::Aes128GcmSha256,
                                           fiber::quic::QuicCryptoSuite::Aes256GcmSha384,
                                           fiber::quic::QuicCryptoSuite::ChaCha20Poly1305Sha256));
