#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "net/IpAddress.h"
#include "quic/QuicCrypto.h"
#include "quic/QuicPacketCodec.h"
#include "quic/QuicPacketProcessor.h"
#include "quic/QuicTransportCodec.h"

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
            packet, crypto.initial_read, plaintext, plaintext_len, pn + packet.pn_len,
            datagram.size() - static_cast<std::size_t>(pn + packet.pn_len - datagram.data()));
    ASSERT_TRUE(sealed.has_value());
    packet.packet_len = static_cast<std::size_t>(pn + packet.pn_len - datagram.data()) + *sealed;
    ASSERT_TRUE(
            fiber::quic::quic_apply_header_protection(packet, crypto.initial_read, datagram.data(), packet.packet_len));
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

    fiber::quic::QuicConnection::Options options{};
    options.role = fiber::quic::QuicConnectionRole::Server;
    options.remote_addr = loopback(4433);
    options.local_addr = loopback(8443);
    fiber::quic::QuicConnection conn(options);
    std::array<std::uint8_t, 256> plaintext{};
    auto received = received_datagram(datagram.data(), datagram.size());

    auto result = fiber::quic::quic_process_initial_datagram(conn, received, plaintext.data(), plaintext.size());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->packet_type, fiber::quic::QuicPacketType::Initial);
    EXPECT_EQ(result->level, fiber::quic::QuicEncryptionLevel::Initial);
    EXPECT_EQ(result->packet_number, 2U);
    EXPECT_EQ(result->frame_count, 1U);
    EXPECT_TRUE(result->ack_eliciting);
    EXPECT_TRUE(result->send_ack);
    EXPECT_EQ(conn.state(), fiber::quic::QuicConnectionState::Handshaking);
    EXPECT_TRUE(conn.crypto().initial_ready);
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

    fiber::quic::QuicConnection::Options options{};
    options.role = fiber::quic::QuicConnectionRole::Server;
    fiber::quic::QuicConnection conn(options);
    std::array<std::uint8_t, 256> plaintext{};
    auto received = received_datagram(datagram.data(), datagram.size());

    auto result = fiber::quic::quic_process_initial_datagram(conn, received, plaintext.data(), plaintext.size());

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), fiber::common::IoErr::Invalid);
}

TEST(QuicPacketProcessorTest, RejectsTamperedInitialWithoutPacketNumberUpdate) {
    const std::array<std::uint8_t, 1> payload{0x01};
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> datagram{};
    fiber::quic::QuicPacketHeader packet{};
    build_initial_datagram(datagram, packet, payload.data(), payload.size());
    datagram[packet.packet_len - 1] ^= 0x40;

    fiber::quic::QuicConnection::Options options{};
    options.role = fiber::quic::QuicConnectionRole::Server;
    fiber::quic::QuicConnection conn(options);
    std::array<std::uint8_t, 256> plaintext{};
    auto received = received_datagram(datagram.data(), datagram.size());

    auto result = fiber::quic::quic_process_initial_datagram(conn, received, plaintext.data(), plaintext.size());

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(conn.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial).largest_received_packet_number,
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

    fiber::quic::QuicConnection::Options options{};
    options.role = fiber::quic::QuicConnectionRole::Server;
    fiber::quic::QuicConnection conn(options);
    std::array<std::uint8_t, 256> plaintext{};
    auto received = received_datagram(datagram.data(), out.offset());

    auto result = fiber::quic::quic_process_initial_datagram(conn, received, plaintext.data(), plaintext.size());

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), fiber::common::IoErr::Invalid);
}

TEST(QuicPacketProcessorTest, CreatesProbePathForDifferentRemoteAddress) {
    const std::array<std::uint8_t, 1> payload{0x01};
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> datagram{};
    fiber::quic::QuicPacketHeader packet{};
    build_initial_datagram(datagram, packet, payload.data(), payload.size());

    fiber::quic::QuicConnection::Options options{};
    options.role = fiber::quic::QuicConnectionRole::Server;
    options.remote_addr = loopback(4433);
    options.local_addr = loopback(8443);
    fiber::quic::QuicConnection conn(options);
    std::array<std::uint8_t, 256> plaintext{};
    auto received = received_datagram(datagram.data(), datagram.size());
    received.peer = loopback(4434);

    auto result = fiber::quic::quic_process_initial_datagram(conn, received, plaintext.data(), plaintext.size());

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

    fiber::quic::QuicConnection::Options client_options{};
    client_options.role = fiber::quic::QuicConnectionRole::Client;
    fiber::quic::QuicConnection client(client_options);
    ASSERT_TRUE(fiber::quic::quic_set_encryption_secret(client.crypto(), fiber::quic::QuicEncryptionLevel::Application,
                                                        true, suite, secret.data(), 32));

    fiber::quic::QuicConnection::Options server_options{};
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
    fiber::quic::QuicPacketEncodeSpec spec{};
    spec.level = fiber::quic::QuicEncryptionLevel::Application;
    spec.dcid = server_cid;
    spec.frames = &frame;
    spec.frame_count = 1;
    auto encoded = fiber::quic::quic_encode_packet(client, spec, datagram.data(), datagram.size());
    ASSERT_TRUE(encoded.has_value()) << static_cast<int>(encoded.error());

    std::array<std::uint8_t, 256> plaintext{};
    auto received = received_datagram(datagram.data(), encoded->packet_len);
    auto result = fiber::quic::quic_process_datagram(server, received, plaintext.data(), plaintext.size(),
                                                     static_cast<std::uint8_t>(server_cid.size()));

    ASSERT_TRUE(result.has_value()) << static_cast<int>(result.error());
    EXPECT_EQ(result->packet_type, fiber::quic::QuicPacketType::Short);
    EXPECT_EQ(result->level, fiber::quic::QuicEncryptionLevel::Application);
    EXPECT_EQ(result->packet_count, 1U);
    EXPECT_EQ(result->packet_number, 0U);
    EXPECT_TRUE(result->ack_eliciting);
    EXPECT_TRUE(result->send_ack);
    EXPECT_EQ(server.packet_number_space(fiber::quic::QuicEncryptionLevel::Application).pending_ack, 0U);
}
