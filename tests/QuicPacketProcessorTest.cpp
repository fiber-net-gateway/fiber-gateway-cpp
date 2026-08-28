#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string_view>
#include <vector>

#include <fiber/net/IpAddress.h>
#include <fiber/quic/QuicPacketProcessor.h>
#include "quic/QuicCrypto.h"
#include "quic/QuicPacketCodec.h"
#include "quic/QuicTransportCodec.h"
#include "quic/QuicTransportParamsCodec.h"

#include "QuicTestLoop.h"

namespace {

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

fiber::net::SocketAddress loopback(std::uint16_t port) { return {fiber::net::IpAddress::loopback_v4(), port}; }

void destroy_test_stream(void *, fiber::quic::QuicStream &stream) noexcept { delete &stream; }

fiber::quic::QuicStream::Lease create_stream(void * /*owner*/, std::uint64_t /*stream_id*/) noexcept {
    return fiber::quic::QuicStream::Lease::adopt(new (std::nothrow)
                                                         fiber::quic::QuicStream(nullptr, destroy_test_stream));
}

void build_initial_datagram(std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> &datagram,
                            fiber::quic::QuicPacketHeader &packet, const std::uint8_t *plaintext,
                            std::size_t plaintext_len) {
    const auto dcid = cid_from_hex("8394c8f03e515708");
    fiber::quic::QuicCryptoState crypto{};
    ASSERT_TRUE(fiber::quic::quic_init_initial_crypto(crypto, fiber::quic::QuicConnectionRole::Server, dcid));

    packet.long_header = true;
    packet.type = fiber::quic::QuicPacketType::Initial;
    packet.level = fiber::quic::QuicEncryptionLevel::Initial;
    packet.flags =
            fiber::quic::kPacketFlagLong | fiber::quic::kPacketFlagFixed | fiber::quic::kLongPacketTypeInitial | 0x03;
    packet.version = fiber::quic::kQuicVersion1;
    packet.dcid = dcid;
    packet.scid = cid_from_hex("11223344");
    packet.length = 4 + plaintext_len + fiber::quic::kAeadTagLength;
    packet.pn_len = 4;
    packet.packet_number = 2;
    packet.truncated_pn = 2;

    fiber::quic::QuicWriteCursor out(datagram.data(), datagram.size());
    std::uint8_t *pn = nullptr;
    auto header_len = fiber::quic::quic_create_packet_header(out, packet, &pn);
    ASSERT_TRUE(header_len.has_value());
    ASSERT_NE(pn, nullptr);

    packet.packet_data = datagram.data();
    packet.packet_len = *header_len + plaintext_len + fiber::quic::kAeadTagLength;
    packet.protected_pn = pn;
    packet.ciphertext = pn + packet.pn_len;
    packet.ciphertext_len = plaintext_len + fiber::quic::kAeadTagLength;

    auto sealed = fiber::quic::quic_encrypt_packet_payload(
            packet, crypto.initial_read(), plaintext, plaintext_len, pn + packet.pn_len,
            datagram.size() - static_cast<std::size_t>(pn + packet.pn_len - datagram.data()));
    ASSERT_TRUE(sealed.has_value());
    packet.packet_len = static_cast<std::size_t>(pn + packet.pn_len - datagram.data()) + *sealed;
    ASSERT_TRUE(fiber::quic::quic_apply_header_protection(packet, crypto.initial_read(), datagram.data(),
                                                          packet.packet_len));
}

fiber::quic::QuicReceivedDatagram received_datagram(std::uint8_t *data, std::size_t len) {
    fiber::quic::QuicReceivedDatagram datagram{};
    datagram.data = data;
    datagram.len = len;
    datagram.peer = loopback(4433);
    datagram.local = loopback(8443);
    datagram.ecn = fiber::net::UdpEcn::Ect0;
    datagram.received_at = std::chrono::steady_clock::now();
    return datagram;
}

fiber::quic::QuicConnection::Options application_client_options(const fiber::quic::QuicConnectionId &server_cid,
                                                                const fiber::quic::QuicConnectionId &client_cid) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Client;
    options.local_connection_id = client_cid;
    options.remote_connection_id = server_cid;
    return options;
}

fiber::quic::QuicConnection::Options application_server_options(const fiber::quic::QuicConnectionId &server_cid,
                                                                const fiber::quic::QuicConnectionId &client_cid) {
    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Server;
    options.local_addr = loopback(8443);
    options.remote_addr = loopback(4433);
    options.local_connection_id = server_cid;
    options.remote_connection_id = client_cid;
    return options;
}

struct ApplicationPacketTestContext {
    ApplicationPacketTestContext() :
        server_cid(cid_from_hex("0102030405060708")), client_cid(cid_from_hex("1112131415161718")),
        client(application_client_options(server_cid, client_cid)),
        server(application_server_options(server_cid, client_cid)) {
        for (std::size_t i = 0; i < secret.size(); ++i) {
            secret[i] = static_cast<std::uint8_t>(i + 1);
        }
    }

    [[nodiscard]] bool init_keys() noexcept {
        constexpr auto suite = fiber::quic::QuicCryptoSuite::Aes128GcmSha256;
        return fiber::quic::quic_set_encryption_secret(client.crypto(), fiber::quic::QuicEncryptionLevel::Application,
                                                       false, suite, secret.data(), secret.size())
                       .has_value() &&
               fiber::quic::quic_set_encryption_secret(client.crypto(), fiber::quic::QuicEncryptionLevel::Application,
                                                       true, suite, secret.data(), secret.size())
                       .has_value() &&
               fiber::quic::quic_set_encryption_secret(server.crypto(), fiber::quic::QuicEncryptionLevel::Application,
                                                       false, suite, secret.data(), secret.size())
                       .has_value() &&
               fiber::quic::quic_set_encryption_secret(server.crypto(), fiber::quic::QuicEncryptionLevel::Application,
                                                       true, suite, secret.data(), secret.size())
                       .has_value();
    }

    [[nodiscard]] fiber::common::IoResult<fiber::quic::QuicPacketEncodeResult>
    encode(const std::uint8_t *payload, std::size_t payload_len, std::size_t frame_count) noexcept {
        fiber::quic::QuicPacketEncodeSpec spec{};
        spec.level = fiber::quic::QuicEncryptionLevel::Application;
        spec.dcid = server_cid;
        spec.payload = payload;
        spec.payload_len = payload_len;
        spec.payload_frame_count = frame_count;
        spec.payload_ack_eliciting = true;
        return fiber::quic::quic_encode_packet(client, spec, {encode_plaintext.data(), encode_plaintext.size()},
                                               datagram.data(), datagram.size());
    }

    fiber::quic::QuicConnectionId server_cid{};
    fiber::quic::QuicConnectionId client_cid{};
    std::array<std::uint8_t, 32> secret{};
    fiber::quic::QuicConnection client;
    fiber::quic::QuicConnection server;
    std::array<std::uint8_t, 256> datagram{};
    std::array<std::uint8_t, 256> encode_plaintext{};
};

} // namespace

TEST(QuicPacketProcessorTest, ProcessesClientInitialCryptoFrame) {
    const std::array<std::uint8_t, 3> crypto_data{'t', 'l', 's'};
    fiber::quic::QuicOutputFrame frame{};
    frame.type = fiber::quic::QuicFrameType::Crypto;
    frame.u.crypto.offset = 0;
    auto owned = fiber::quic::quic_output_frame_set_owned_data(frame, crypto_data.data(), crypto_data.size());
    ASSERT_TRUE(owned.has_value());

    std::array<std::uint8_t, 64> payload{};
    fiber::quic::QuicWriteCursor payload_out(payload.data(), payload.size());
    auto payload_len = fiber::quic::quic_create_output_frame(&payload_out, frame);
    ASSERT_TRUE(payload_len.has_value());
    fiber::quic::quic_output_frame_release_data(frame);

    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> datagram{};
    fiber::quic::QuicPacketHeader packet{};
    build_initial_datagram(datagram, packet, payload.data(), payload_out.offset());

    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Server;
    options.remote_addr = loopback(4433);
    options.local_addr = loopback(8443);
    fiber::quic::QuicConnection conn(options);
    auto received = received_datagram(datagram.data(), datagram.size());

    auto result = fiber::quic::quic_process_initial_datagram(conn, received);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->packet_type, fiber::quic::QuicPacketType::Initial);
    EXPECT_EQ(result->level, fiber::quic::QuicEncryptionLevel::Initial);
    EXPECT_EQ(result->packet_number, 2U);
    EXPECT_EQ(result->frame_count, 1U);
    EXPECT_TRUE(result->ack_eliciting);
    EXPECT_TRUE(result->send_ack);
    EXPECT_EQ(conn.state(), fiber::quic::QuicConnectionState::Handshaking);
    EXPECT_TRUE(conn.crypto().initial_ready());
    EXPECT_EQ(conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial).largest_received_packet_number, 2U);
}

TEST(QuicPacketProcessorTest, RejectsInitialStreamFrame) {
    const std::array<std::uint8_t, 3> stream_data{'b', 'a', 'd'};
    std::array<std::uint8_t, 64> payload{};
    fiber::quic::QuicWriteCursor payload_out(payload.data(), payload.size());
    ASSERT_TRUE(payload_out.write_u8(0x0a).has_value());
    ASSERT_TRUE(fiber::quic::quic_write_varint(payload_out, 0).has_value());
    ASSERT_TRUE(fiber::quic::quic_write_varint(payload_out, stream_data.size()).has_value());
    ASSERT_TRUE(payload_out.write_bytes(stream_data.data(), stream_data.size()).has_value());

    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> datagram{};
    fiber::quic::QuicPacketHeader packet{};
    build_initial_datagram(datagram, packet, payload.data(), payload_out.offset());

    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Server;
    fiber::quic::QuicConnection conn(options);
    auto received = received_datagram(datagram.data(), datagram.size());

    auto result = fiber::quic::quic_process_initial_datagram(conn, received);

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), fiber::common::IoErr::Invalid);
}

TEST(QuicPacketProcessorTest, RejectsTamperedInitialWithoutPacketNumberUpdate) {
    const std::array<std::uint8_t, 1> payload{0x01};
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> datagram{};
    fiber::quic::QuicPacketHeader packet{};
    build_initial_datagram(datagram, packet, payload.data(), payload.size());
    datagram[packet.packet_len - 1] ^= 0x40;

    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Server;
    fiber::quic::QuicConnection conn(options);
    auto received = received_datagram(datagram.data(), datagram.size());

    auto result = fiber::quic::quic_process_initial_datagram(conn, received);

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial).largest_received_packet_number,
              fiber::quic::kUnsetPacketNumber);
}

TEST(QuicPacketProcessorTest, MalformedInitialTailIsRejectedBeforeStateCommit) {
    const auto dcid = cid_from_hex("8394c8f03e515708");
    const auto client_cid = cid_from_hex("1122334455667788");

    fiber::quic::QuicConnection::Options client_options = fiber::test::quic_options();
    client_options.role = fiber::quic::QuicConnectionRole::Client;
    fiber::quic::QuicConnection client(client_options);
    ASSERT_TRUE(client.init_initial_crypto(dcid));

    fiber::quic::QuicConnection::Options server_options = fiber::test::quic_options();
    server_options.role = fiber::quic::QuicConnectionRole::Server;
    server_options.local_addr = loopback(8443);
    server_options.remote_addr = loopback(4433);
    fiber::quic::QuicConnection server(server_options);
    ASSERT_TRUE(server.init_initial_crypto(dcid));

    // A valid PING followed by an unknown frame type. Initial packets are not
    // strongly authenticated, so the entire payload must be validated before
    // the PING or packet number can affect connection state.
    const std::array<std::uint8_t, 2> payload{0x01, 0x1f};
    std::array<std::uint8_t, 1400> datagram{};
    std::array<std::uint8_t, 1400> encode_plaintext{};
    fiber::quic::QuicPacketEncodeSpec spec{};
    spec.level = fiber::quic::QuicEncryptionLevel::Initial;
    spec.dcid = dcid;
    spec.scid = client_cid;
    spec.payload = payload.data();
    spec.payload_len = payload.size();
    spec.payload_frame_count = 2;
    spec.payload_ack_eliciting = true;
    spec.min_packet_len = fiber::quic::kMinInitialDatagramSize;
    auto encoded = fiber::quic::quic_encode_packet(client, spec, {encode_plaintext.data(), encode_plaintext.size()},
                                                   datagram.data(), datagram.size());
    ASSERT_TRUE(encoded.has_value()) << static_cast<int>(encoded.error());
    auto received = received_datagram(datagram.data(), encoded->packet_len);
    auto result = fiber::quic::quic_process_datagram(server, received, 0);

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), fiber::common::IoErr::Invalid);
    EXPECT_EQ(server.state(), fiber::quic::QuicConnectionState::Init);
    EXPECT_FALSE(server.tls().initialized());
    EXPECT_EQ(server.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial).largest_received_packet_number,
              fiber::quic::kUnsetPacketNumber);
    EXPECT_EQ(server.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial).pending_ack,
              fiber::quic::kUnsetPacketNumber);
}

TEST(QuicPacketProcessorTest, RejectsNonInitialPacket) {
    std::array<std::uint8_t, 64> datagram{};
    fiber::quic::QuicPacketHeader packet{};
    packet.long_header = true;
    packet.type = fiber::quic::QuicPacketType::Handshake;
    packet.level = fiber::quic::QuicEncryptionLevel::Handshake;
    packet.flags =
            fiber::quic::kPacketFlagLong | fiber::quic::kPacketFlagFixed | fiber::quic::kLongPacketTypeHandshake | 0x03;
    packet.version = fiber::quic::kQuicVersion1;
    packet.dcid = cid_from_hex("8394c8f03e515708");
    packet.scid = cid_from_hex("11223344");
    packet.length = 4;
    packet.pn_len = 4;
    packet.truncated_pn = 1;

    fiber::quic::QuicWriteCursor out(datagram.data(), datagram.size());
    std::uint8_t *pn = nullptr;
    auto header_len = fiber::quic::quic_create_packet_header(out, packet, &pn);
    ASSERT_TRUE(header_len.has_value());
    ASSERT_TRUE(out.fill(0, 4));

    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Server;
    fiber::quic::QuicConnection conn(options);
    auto received = received_datagram(datagram.data(), out.offset());

    auto result = fiber::quic::quic_process_initial_datagram(conn, received);

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), fiber::common::IoErr::Invalid);
}

TEST(QuicPacketProcessorTest, CreatesProbePathForDifferentRemoteAddress) {
    const std::array<std::uint8_t, 1> payload{0x01};
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> datagram{};
    fiber::quic::QuicPacketHeader packet{};
    build_initial_datagram(datagram, packet, payload.data(), payload.size());

    fiber::quic::QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Server;
    options.remote_addr = loopback(4433);
    options.local_addr = loopback(8443);
    fiber::quic::QuicConnection conn(options);
    auto received = received_datagram(datagram.data(), datagram.size());
    received.peer = loopback(4434);

    auto result = fiber::quic::quic_process_initial_datagram(conn, received);

    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    EXPECT_TRUE(result->created_path);
    ASSERT_NE(result->path, nullptr);
    EXPECT_EQ(result->path, conn.active_path());
    EXPECT_EQ(conn.remote_addr().port(), 4434);
    EXPECT_EQ(conn.path_count(), 1U);
}

TEST(QuicPacketProcessorTest, ProcessesApplicationPingPacket) {
    constexpr fiber::quic::QuicCryptoSuite suite = fiber::quic::QuicCryptoSuite::Aes128GcmSha256;
    std::array<std::uint8_t, fiber::quic::kQuicMaxSecretLength> secret{};
    for (std::size_t i = 0; i < 32; ++i) {
        secret[i] = static_cast<std::uint8_t>(i + 1);
    }

    const auto server_cid = cid_from_hex("0102030405060708");
    const auto client_cid = cid_from_hex("1112131415161718");

    fiber::quic::QuicConnection::Options client_options = fiber::test::quic_options();
    client_options.role = fiber::quic::QuicConnectionRole::Client;
    fiber::quic::QuicConnection client(client_options);
    ASSERT_TRUE(fiber::quic::quic_set_encryption_secret(client.crypto(), fiber::quic::QuicEncryptionLevel::Application,
                                                        true, suite, secret.data(), 32));

    fiber::quic::QuicConnection::Options server_options = fiber::test::quic_options();
    server_options.role = fiber::quic::QuicConnectionRole::Server;
    server_options.local_addr = loopback(8443);
    server_options.remote_addr = loopback(4433);
    server_options.local_connection_id = server_cid;
    server_options.remote_connection_id = client_cid;
    fiber::quic::QuicConnection server(server_options);
    ASSERT_TRUE(fiber::quic::quic_set_encryption_secret(server.crypto(), fiber::quic::QuicEncryptionLevel::Application,
                                                        false, suite, secret.data(), 32));

    fiber::quic::QuicOutputFrame frame{};
    frame.type = fiber::quic::QuicFrameType::Ping;

    std::array<std::uint8_t, 256> datagram{};
    std::array<std::uint8_t, 256> encode_plaintext{};
    fiber::quic::QuicPacketEncodeSpec spec{};
    spec.level = fiber::quic::QuicEncryptionLevel::Application;
    spec.dcid = server_cid;
    spec.frames = &frame;
    spec.frame_count = 1;
    auto encoded = fiber::quic::quic_encode_packet(client, spec, {encode_plaintext.data(), encode_plaintext.size()},
                                                   datagram.data(), datagram.size());
    ASSERT_TRUE(encoded.has_value()) << static_cast<int>(encoded.error());
    auto received = received_datagram(datagram.data(), encoded->packet_len);
    auto result = fiber::quic::quic_process_datagram(server, received, static_cast<std::uint8_t>(server_cid.size()));

    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    EXPECT_EQ(result->packet_type, fiber::quic::QuicPacketType::Short);
    EXPECT_EQ(result->level, fiber::quic::QuicEncryptionLevel::Application);
    EXPECT_EQ(result->packet_count, 1U);
    EXPECT_EQ(result->packet_number, 0U);
    EXPECT_TRUE(result->ack_eliciting);
    EXPECT_TRUE(result->send_ack);
    EXPECT_EQ(server.packet_number_space(fiber::quic::QuicEncryptionLevel::Application).pending_ack, 0U);
}

TEST(QuicPacketProcessorTest, MalformedApplicationTailClosesWithFrameEncodingError) {
    ApplicationPacketTestContext context;
    ASSERT_TRUE(context.init_keys());

    const std::array<std::uint8_t, 2> payload{0x01, 0x1f}; // PING, then unknown frame type
    auto encoded = context.encode(payload.data(), payload.size(), 2);
    ASSERT_TRUE(encoded.has_value()) << static_cast<int>(encoded.error());

    auto received = received_datagram(context.datagram.data(), encoded->packet_len);
    auto result = fiber::quic::quic_process_datagram(context.server, received,
                                                     static_cast<std::uint8_t>(context.server_cid.size()));

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), fiber::common::IoErr::Invalid);
    EXPECT_EQ(context.server.state(), fiber::quic::QuicConnectionState::Closing);
    EXPECT_EQ(context.server.close_error(), fiber::quic::QuicErrorCode::FrameEncodingError);
    const auto &space = context.server.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    const fiber::quic::QuicOutputFrame *close = space.pending_frames.front();
    ASSERT_NE(close, nullptr);
    EXPECT_EQ(close->type, fiber::quic::QuicFrameType::ConnectionClose);
    EXPECT_EQ(close->u.close.frame_type, 0x1fU);
}

TEST(QuicPacketProcessorTest, DisallowedApplicationFrameClosesWithProtocolViolation) {
    ApplicationPacketTestContext context;
    ASSERT_TRUE(context.init_keys());

    const std::array<std::uint8_t, 1> payload{
            static_cast<std::uint8_t>(fiber::quic::QuicFrameType::HandshakeDone),
    };
    auto encoded = context.encode(payload.data(), payload.size(), 1);
    ASSERT_TRUE(encoded.has_value()) << static_cast<int>(encoded.error());

    auto received = received_datagram(context.datagram.data(), encoded->packet_len);
    auto result = fiber::quic::quic_process_datagram(context.server, received,
                                                     static_cast<std::uint8_t>(context.server_cid.size()));

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), fiber::common::IoErr::Invalid);
    EXPECT_EQ(context.server.state(), fiber::quic::QuicConnectionState::Closing);
    EXPECT_EQ(context.server.close_error(), fiber::quic::QuicErrorCode::ProtocolViolation);
    const auto &space = context.server.packet_number_space(fiber::quic::QuicEncryptionLevel::Application);
    const fiber::quic::QuicOutputFrame *close = space.pending_frames.front();
    ASSERT_NE(close, nullptr);
    EXPECT_EQ(close->u.close.frame_type, static_cast<std::uint64_t>(fiber::quic::QuicFrameType::HandshakeDone));
}

TEST(QuicPacketProcessorTest, HandshakeDoneConfirmsClientHandshake) {
    ApplicationPacketTestContext context;
    ASSERT_TRUE(context.init_keys());
    const std::array<std::uint8_t, 1> payload{
            static_cast<std::uint8_t>(fiber::quic::QuicFrameType::HandshakeDone),
    };
    fiber::quic::QuicPacketEncodeSpec spec{};
    spec.level = fiber::quic::QuicEncryptionLevel::Application;
    spec.dcid = context.client_cid;
    spec.payload = payload.data();
    spec.payload_len = payload.size();
    spec.payload_frame_count = 1;
    spec.payload_ack_eliciting = true;
    auto encoded = fiber::quic::quic_encode_packet(context.server, spec,
                                                   {context.encode_plaintext.data(), context.encode_plaintext.size()},
                                                   context.datagram.data(), context.datagram.size());
    ASSERT_TRUE(encoded.has_value()) << static_cast<int>(encoded.error());

    auto received = received_datagram(context.datagram.data(), encoded->packet_len);
    auto result = fiber::quic::quic_process_datagram(context.client, received,
                                                     static_cast<std::uint8_t>(context.client_cid.size()));

    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    EXPECT_TRUE(context.client.handshake_confirmed());
    EXPECT_TRUE(result->handshake_confirmed);
}

TEST(QuicPacketProcessorTest, ClosingConnectionSkipsMalformedApplicationPayload) {
    ApplicationPacketTestContext context;
    ASSERT_TRUE(context.init_keys());
    context.server.close(fiber::quic::QuicErrorCode::NoError);
    ASSERT_EQ(context.server.state(), fiber::quic::QuicConnectionState::Closing);

    const std::array<std::uint8_t, 1> payload{0x1f};
    auto encoded = context.encode(payload.data(), payload.size(), 1);
    ASSERT_TRUE(encoded.has_value()) << static_cast<int>(encoded.error());

    auto received = received_datagram(context.datagram.data(), encoded->packet_len);
    auto result = fiber::quic::quic_process_datagram(context.server, received,
                                                     static_cast<std::uint8_t>(context.server_cid.size()));

    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    EXPECT_EQ(result->packet_count, 1U);
    EXPECT_EQ(result->frame_count, 0U);
    EXPECT_EQ(context.server.state(), fiber::quic::QuicConnectionState::Closing);
    EXPECT_EQ(context.server.close_error(), fiber::quic::QuicErrorCode::NoError);
}

TEST(QuicPacketProcessorTest, MalformedKeyUpdatePacketDoesNotRotateKeys) {
    ApplicationPacketTestContext context;
    ASSERT_TRUE(context.init_keys());
    ASSERT_TRUE(fiber::quic::quic_derive_next_key_pair(context.client.crypto()));
    ASSERT_TRUE(fiber::quic::quic_derive_next_key_pair(context.server.crypto()));
    context.server.confirm_handshake();

    context.client.crypto().promote_application_keys();
    context.client.crypto().epoch().phase = true;
    ASSERT_TRUE(context.client.key_phase());
    ASSERT_FALSE(context.server.key_phase());

    const std::array<std::uint8_t, 2> payload{0x01, 0x1f};
    auto encoded = context.encode(payload.data(), payload.size(), 2);
    ASSERT_TRUE(encoded.has_value()) << static_cast<int>(encoded.error());

    auto received = received_datagram(context.datagram.data(), encoded->packet_len);
    auto result = fiber::quic::quic_process_datagram(context.server, received,
                                                     static_cast<std::uint8_t>(context.server_cid.size()));

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(context.server.state(), fiber::quic::QuicConnectionState::Closing);
    EXPECT_EQ(context.server.close_error(), fiber::quic::QuicErrorCode::FrameEncodingError);
    EXPECT_FALSE(context.server.key_phase());
    EXPECT_FALSE(context.server.crypto().previous_application_keys_ready());
    EXPECT_TRUE(context.server.crypto().next_application_keys_ready());
}

TEST(QuicPacketProcessorTest, AppliesAuthenticatedPeerKeyUpdate) {
    ApplicationPacketTestContext context;
    ASSERT_TRUE(context.init_keys());
    ASSERT_TRUE(fiber::quic::quic_derive_next_key_pair(context.client.crypto()));
    ASSERT_TRUE(fiber::quic::quic_derive_next_key_pair(context.server.crypto()));
    context.server.confirm_handshake();

    context.client.crypto().promote_application_keys();
    context.client.crypto().epoch().phase = true;
    const std::array<std::uint8_t, 1> payload{0x01};
    auto encoded = context.encode(payload.data(), payload.size(), 1);
    ASSERT_TRUE(encoded.has_value()) << static_cast<int>(encoded.error());

    auto received = received_datagram(context.datagram.data(), encoded->packet_len);
    auto result = fiber::quic::quic_process_datagram(context.server, received,
                                                     static_cast<std::uint8_t>(context.server_cid.size()));

    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    EXPECT_EQ(context.server.crypto().epoch().generation, 1U);
    EXPECT_TRUE(context.server.key_phase());
    EXPECT_EQ(context.server.crypto().epoch().current_read_lowest_pn, encoded->packet_number);
    EXPECT_TRUE(context.server.crypto().previous_application_keys_ready());
    EXPECT_TRUE(context.server.crypto().next_application_keys_ready());
}

TEST(QuicPacketProcessorTest, DecryptsReorderedPacketWithPreviousEpoch) {
    ApplicationPacketTestContext context;
    ASSERT_TRUE(context.init_keys());
    ASSERT_TRUE(fiber::quic::quic_derive_next_key_pair(context.client.crypto()));
    ASSERT_TRUE(fiber::quic::quic_derive_next_key_pair(context.server.crypto()));
    context.server.confirm_handshake();

    const std::array<std::uint8_t, 1> payload{0x01};
    auto previous = context.encode(payload.data(), payload.size(), 1);
    ASSERT_TRUE(previous.has_value()) << static_cast<int>(previous.error());
    std::array<std::uint8_t, 256> previous_datagram = context.datagram;

    context.client.crypto().promote_application_keys();
    context.client.crypto().epoch().phase = true;
    auto current = context.encode(payload.data(), payload.size(), 1);
    ASSERT_TRUE(current.has_value()) << static_cast<int>(current.error());
    ASSERT_GT(current->packet_number, previous->packet_number);

    auto received_current = received_datagram(context.datagram.data(), current->packet_len);
    auto current_result = fiber::quic::quic_process_datagram(context.server, received_current,
                                                             static_cast<std::uint8_t>(context.server_cid.size()));
    ASSERT_TRUE(current_result.has_value()) << static_cast<int>(current_result.error());
    ASSERT_EQ(context.server.crypto().epoch().generation, 1U);

    auto received_previous = received_datagram(previous_datagram.data(), previous->packet_len);
    auto previous_result = fiber::quic::quic_process_datagram(context.server, received_previous,
                                                              static_cast<std::uint8_t>(context.server_cid.size()));
    ASSERT_TRUE(previous_result.has_value()) << static_cast<int>(previous_result.error());
    EXPECT_EQ(previous_result->packet_number, previous->packet_number);
    EXPECT_EQ(context.server.crypto().epoch().generation, 1U);
    EXPECT_TRUE(context.server.key_phase());
}

TEST(QuicPacketProcessorTest, ProcessesEarlyDataStreamPacketWithEarlyKeys) {
    constexpr fiber::quic::QuicCryptoSuite suite = fiber::quic::QuicCryptoSuite::Aes128GcmSha256;
    std::array<std::uint8_t, fiber::quic::kQuicMaxSecretLength> secret{};
    for (std::size_t i = 0; i < 32; ++i) {
        secret[i] = static_cast<std::uint8_t>(0xa0 + i);
    }

    const auto server_cid = cid_from_hex("0102030405060708");
    const auto client_cid = cid_from_hex("1112131415161718");

    fiber::quic::QuicConnection::Options client_options = fiber::test::quic_options();
    client_options.role = fiber::quic::QuicConnectionRole::Client;
    client_options.local_connection_id = client_cid;
    client_options.remote_connection_id = server_cid;
    fiber::quic::QuicConnection client(client_options);
    ASSERT_TRUE(fiber::quic::quic_set_encryption_secret(client.crypto(), fiber::quic::QuicEncryptionLevel::EarlyData,
                                                        true, suite, secret.data(), 32));

    fiber::quic::QuicConnection::Options server_options = fiber::test::quic_options();
    server_options.role = fiber::quic::QuicConnectionRole::Server;
    server_options.local_addr = loopback(8443);
    server_options.remote_addr = loopback(4433);
    server_options.local_connection_id = server_cid;
    server_options.remote_connection_id = client_cid;
    server_options.ops.create_stream = create_stream;
    fiber::quic::QuicConnection server(server_options);
    ASSERT_TRUE(fiber::quic::quic_set_encryption_secret(server.crypto(), fiber::quic::QuicEncryptionLevel::EarlyData,
                                                        false, suite, secret.data(), 32));
    fiber::quic::QuicTransportParams peer_params{};
    peer_params.initial_source_connection_id = client_cid;
    peer_params.has_initial_source_connection_id = true;
    peer_params.initial_max_data = 4096;
    peer_params.initial_max_stream_data_bidi_local = 4096;
    peer_params.initial_max_stream_data_bidi_remote = 4096;
    peer_params.initial_max_stream_data_uni = 4096;
    peer_params.initial_max_streams_bidi = 4;
    peer_params.initial_max_streams_uni = 4;
    ASSERT_TRUE(server.apply_peer_transport_params(peer_params));

    const std::array<std::uint8_t, 6> stream_payload{0x0a, 0x00, 0x03, 'a', 'b', 'c'};
    std::array<std::uint8_t, 256> datagram{};
    fiber::quic::QuicPacketHeader packet{};
    packet.long_header = true;
    packet.type = fiber::quic::QuicPacketType::ZeroRtt;
    packet.level = fiber::quic::QuicEncryptionLevel::EarlyData;
    packet.flags =
            fiber::quic::kPacketFlagLong | fiber::quic::kPacketFlagFixed | fiber::quic::kLongPacketTypeZeroRtt | 0x03;
    packet.version = fiber::quic::kQuicVersion1;
    packet.dcid = server_cid;
    packet.scid = client_cid;
    packet.length = 4 + stream_payload.size() + fiber::quic::kAeadTagLength;
    packet.pn_len = 4;
    packet.packet_number = 0;
    packet.truncated_pn = 0;

    fiber::quic::QuicWriteCursor packet_out(datagram.data(), datagram.size());
    std::uint8_t *pn = nullptr;
    auto header_len = fiber::quic::quic_create_packet_header(packet_out, packet, &pn);
    ASSERT_TRUE(header_len.has_value());
    ASSERT_NE(pn, nullptr);

    packet.packet_data = datagram.data();
    packet.packet_len = *header_len + stream_payload.size() + fiber::quic::kAeadTagLength;
    packet.protected_pn = pn;
    packet.ciphertext = pn + packet.pn_len;
    packet.ciphertext_len = stream_payload.size() + fiber::quic::kAeadTagLength;

    auto sealed = fiber::quic::quic_encrypt_packet_payload(
            packet, client.crypto().early_write(), stream_payload.data(), stream_payload.size(), pn + packet.pn_len,
            datagram.size() - static_cast<std::size_t>(pn + packet.pn_len - datagram.data()));
    ASSERT_TRUE(sealed.has_value()) << static_cast<int>(sealed.error());
    packet.packet_len = static_cast<std::size_t>(pn + packet.pn_len - datagram.data()) + *sealed;
    ASSERT_TRUE(fiber::quic::quic_apply_header_protection(packet, client.crypto().early_write(), datagram.data(),
                                                          packet.packet_len));
    auto received = received_datagram(datagram.data(), packet.packet_len);
    auto result = fiber::quic::quic_process_datagram(server, received, static_cast<std::uint8_t>(server_cid.size()));

    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    EXPECT_EQ(result->packet_type, fiber::quic::QuicPacketType::ZeroRtt);
    EXPECT_EQ(result->level, fiber::quic::QuicEncryptionLevel::EarlyData);
    EXPECT_EQ(result->packet_count, 1U);
    EXPECT_TRUE(result->ack_eliciting);
    EXPECT_TRUE(result->send_ack);
    EXPECT_EQ(server.packet_number_space(fiber::quic::QuicEncryptionLevel::Application).pending_ack, 0U);
    fiber::quic::QuicStream *stream = server.find_stream(0);
    ASSERT_NE(stream, nullptr);
    fiber::mem::IoBufChain stream_data(server.recv_extent_pool());
    auto stream_read = stream->try_read(3, stream_data);
    ASSERT_TRUE(stream_read.has_value());
    ASSERT_EQ(*stream_read, 3U);
    ASSERT_NE(stream_data.front(), nullptr);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(stream_data.front()->readable_data()),
                               stream_data.front()->readable()),
              "abc");
    EXPECT_EQ(server.state(), fiber::quic::QuicConnectionState::Init);
}

TEST(QuicPacketProcessorTest, ConnectionCloseDuringGracefulShutdownEntersDrainingAndStopsFrameDispatch) {
    constexpr fiber::quic::QuicCryptoSuite suite = fiber::quic::QuicCryptoSuite::Aes128GcmSha256;
    std::array<std::uint8_t, fiber::quic::kQuicMaxSecretLength> secret{};
    for (std::size_t i = 0; i < 32; ++i) {
        secret[i] = static_cast<std::uint8_t>(0x80 + i);
    }

    const auto server_cid = cid_from_hex("0102030405060708");
    const auto client_cid = cid_from_hex("1112131415161718");

    fiber::quic::QuicConnection::Options client_options = fiber::test::quic_options();
    client_options.role = fiber::quic::QuicConnectionRole::Client;
    fiber::quic::QuicConnection client(client_options);
    ASSERT_TRUE(fiber::quic::quic_set_encryption_secret(client.crypto(), fiber::quic::QuicEncryptionLevel::Application,
                                                        true, suite, secret.data(), 32));

    fiber::quic::QuicConnection::Options server_options = fiber::test::quic_options();
    server_options.role = fiber::quic::QuicConnectionRole::Server;
    server_options.local_addr = loopback(8443);
    server_options.remote_addr = loopback(4433);
    server_options.local_connection_id = server_cid;
    server_options.remote_connection_id = client_cid;
    server_options.ops.create_stream = create_stream;
    fiber::quic::QuicConnection server(server_options);
    ASSERT_TRUE(fiber::quic::quic_set_encryption_secret(server.crypto(), fiber::quic::QuicEncryptionLevel::Application,
                                                        false, suite, secret.data(), 32));
    ASSERT_TRUE(server.mark_established().has_value());

    fiber::quic::QuicStreamFrame stream_frame{};
    stream_frame.stream_id = 0;
    ASSERT_TRUE(server.recv_stream_frame(stream_frame, {}).has_value());
    ASSERT_EQ(server.active_stream_count(), 1U);
    server.shutdown(fiber::quic::QuicErrorCode::NoError);
    ASSERT_EQ(server.state(), fiber::quic::QuicConnectionState::GracefulClosing);

    std::array<fiber::quic::QuicOutputFrame, 2> frames{};
    frames[0].type = fiber::quic::QuicFrameType::ConnectionClose;
    frames[0].u.close.error_code = static_cast<std::uint64_t>(fiber::quic::QuicErrorCode::InternalError);
    frames[0].u.close.frame_type = 0;
    frames[1].type = fiber::quic::QuicFrameType::Ping;

    std::array<std::uint8_t, 256> datagram{};
    std::array<std::uint8_t, 256> encode_plaintext{};
    fiber::quic::QuicPacketEncodeSpec spec{};
    spec.level = fiber::quic::QuicEncryptionLevel::Application;
    spec.dcid = server_cid;
    spec.frames = frames.data();
    spec.frame_count = frames.size();
    auto encoded = fiber::quic::quic_encode_packet(client, spec, {encode_plaintext.data(), encode_plaintext.size()},
                                                   datagram.data(), datagram.size());
    ASSERT_TRUE(encoded.has_value()) << static_cast<int>(encoded.error());
    auto received = received_datagram(datagram.data(), encoded->packet_len);
    auto result = fiber::quic::quic_process_datagram(server, received, static_cast<std::uint8_t>(server_cid.size()));

    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    EXPECT_EQ(server.state(), fiber::quic::QuicConnectionState::Draining);
    EXPECT_EQ(result->frame_count, 1U);
    EXPECT_FALSE(result->send_ack);
}

TEST(QuicPacketProcessorTest, PathChallengeQueuesPathResponse) {
    constexpr fiber::quic::QuicCryptoSuite suite = fiber::quic::QuicCryptoSuite::Aes128GcmSha256;
    std::array<std::uint8_t, fiber::quic::kQuicMaxSecretLength> secret{};
    for (std::size_t i = 0; i < 32; ++i) {
        secret[i] = static_cast<std::uint8_t>(0x30 + i);
    }

    const auto server_cid = cid_from_hex("0102030405060708");
    const auto client_cid = cid_from_hex("1112131415161718");

    fiber::quic::QuicConnection::Options client_options = fiber::test::quic_options();
    client_options.role = fiber::quic::QuicConnectionRole::Client;
    fiber::quic::QuicConnection client(client_options);
    ASSERT_TRUE(fiber::quic::quic_set_encryption_secret(client.crypto(), fiber::quic::QuicEncryptionLevel::Application,
                                                        true, suite, secret.data(), 32));

    fiber::quic::QuicConnection::Options server_options = fiber::test::quic_options();
    server_options.role = fiber::quic::QuicConnectionRole::Server;
    server_options.local_addr = loopback(8443);
    server_options.remote_addr = loopback(4433);
    server_options.local_connection_id = server_cid;
    server_options.remote_connection_id = client_cid;
    fiber::quic::QuicConnection server(server_options);
    ASSERT_TRUE(fiber::quic::quic_set_encryption_secret(server.crypto(), fiber::quic::QuicEncryptionLevel::Application,
                                                        false, suite, secret.data(), 32));

    fiber::quic::QuicOutputFrame frame{};
    frame.type = fiber::quic::QuicFrameType::PathChallenge;
    for (std::size_t i = 0; i < sizeof(frame.u.path_challenge.data); ++i) {
        frame.u.path_challenge.data[i] = static_cast<std::uint8_t>(0xc0 + i);
    }

    std::array<std::uint8_t, 256> datagram{};
    std::array<std::uint8_t, 256> encode_plaintext{};
    fiber::quic::QuicPacketEncodeSpec spec{};
    spec.level = fiber::quic::QuicEncryptionLevel::Application;
    spec.dcid = server_cid;
    spec.frames = &frame;
    spec.frame_count = 1;
    auto encoded = fiber::quic::quic_encode_packet(client, spec, {encode_plaintext.data(), encode_plaintext.size()},
                                                   datagram.data(), datagram.size());
    ASSERT_TRUE(encoded.has_value()) << static_cast<int>(encoded.error());
    auto received = received_datagram(datagram.data(), encoded->packet_len);
    auto result = fiber::quic::quic_process_datagram(server, received, static_cast<std::uint8_t>(server_cid.size()));

    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    EXPECT_TRUE(result->send_output);
    ASSERT_NE(result->path, nullptr);
    ASSERT_FALSE(result->path->pending_frames.empty());
    const fiber::quic::QuicOutputFrame *response = result->path->pending_frames.front();
    ASSERT_NE(response, nullptr);
    EXPECT_EQ(response->type, fiber::quic::QuicFrameType::PathResponse);
    EXPECT_EQ(response->path, result->path);
    EXPECT_EQ(response->min_packet_len, fiber::quic::kMinInitialDatagramSize);
    EXPECT_EQ(std::memcmp(response->u.path_response.data, frame.u.path_challenge.data,
                          sizeof(frame.u.path_challenge.data)),
              0);
    EXPECT_EQ(server.packet_number_space(fiber::quic::QuicEncryptionLevel::Application).pending_frames.front()->type,
              fiber::quic::QuicFrameType::Ping);
}
