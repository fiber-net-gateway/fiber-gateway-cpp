#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "quic/QuicCrypto.h"
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

template<std::size_t N>
std::vector<std::uint8_t> vec_from_array(const std::array<std::uint8_t, N> &value) {
    return {value.begin(), value.end()};
}

std::vector<std::uint8_t> vec_from_bytes(const std::uint8_t *data, std::size_t len) { return {data, data + len}; }

void build_initial_datagram(std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> &datagram,
                            fiber::quic::QuicPacketHeader &packet, std::uint8_t **packet_number_pos,
                            std::size_t plaintext_len) {
    packet.long_header = true;
    packet.type = fiber::quic::QuicPacketType::Initial;
    packet.level = fiber::quic::QuicEncryptionLevel::Initial;
    packet.flags =
            fiber::quic::kPacketFlagLong | fiber::quic::kPacketFlagFixed | fiber::quic::kLongPacketTypeInitial | 0x03;
    packet.version = fiber::quic::kQuicVersion1;
    packet.dcid = cid_from_hex("8394c8f03e515708");
    packet.scid = cid_from_hex("11223344");
    packet.length = 4 + plaintext_len + fiber::quic::kAeadTagLength;
    packet.pn_len = 4;
    packet.packet_number = 2;
    packet.truncated_pn = 2;

    fiber::quic::QuicWriteCursor out(datagram.data(), datagram.size());
    auto header_len = fiber::quic::quic_create_packet_header(out, packet, packet_number_pos);

    ASSERT_TRUE(header_len.has_value());
    ASSERT_NE(*packet_number_pos, nullptr);
    packet.packet_data = datagram.data();
    packet.packet_len = *header_len + plaintext_len + fiber::quic::kAeadTagLength;
    packet.protected_pn = *packet_number_pos;
    packet.ciphertext = *packet_number_pos + packet.pn_len;
    packet.ciphertext_len = plaintext_len + fiber::quic::kAeadTagLength;
}

} // namespace

TEST(QuicCryptoTest, DerivesRfc9001InitialSecrets) {
    auto dcid = cid_from_hex("8394c8f03e515708");

    auto secrets = fiber::quic::quic_derive_initial_secrets(dcid);

    ASSERT_TRUE(secrets.has_value());
    EXPECT_EQ(vec_from_array(secrets->initial),
              hex("7db5df06e7a69e432496adedb00851923595221596ae2ae9fb8115c1e9ed0a44"));
    EXPECT_EQ(vec_from_array(secrets->client), hex("c00cf151ca5be075ed0ebfb5c80323c42d6b7db67881289af4008f1f6c357aea"));
    EXPECT_EQ(vec_from_array(secrets->server), hex("3c199828fd139efd216c155ad844cc81fb82fa8d7446fa7d78be803acdda951b"));
}

TEST(QuicCryptoTest, InitializesInitialPacketProtectionKeysForServerRole) {
    auto dcid = cid_from_hex("8394c8f03e515708");
    fiber::quic::QuicCryptoState state{};

    auto initialized = fiber::quic::quic_init_initial_crypto(state, fiber::quic::QuicConnectionRole::Server, dcid);

    ASSERT_TRUE(initialized.has_value());
    ASSERT_TRUE(state.initial_ready);
    ASSERT_TRUE(state.initial_read.ready);
    ASSERT_TRUE(state.initial_write.ready);
    EXPECT_EQ(vec_from_bytes(state.initial_read.key.data(), state.initial_read.key_len),
              hex("1f369613dd76d5467730efcbe3b1a22d"));
    EXPECT_EQ(vec_from_array(state.initial_read.iv), hex("fa044b2f42a3fd3b46fb255c"));
    EXPECT_EQ(vec_from_bytes(state.initial_read.hp.data(), state.initial_read.hp_len),
              hex("9f50449e04a0e810283a1e9933adedd2"));
    EXPECT_EQ(vec_from_bytes(state.initial_write.key.data(), state.initial_write.key_len),
              hex("cf3a5331653c364c88f0f379b6067e37"));
    EXPECT_EQ(vec_from_array(state.initial_write.iv), hex("0ac1493ca1905853b0bba03e"));
    EXPECT_EQ(vec_from_bytes(state.initial_write.hp.data(), state.initial_write.hp_len),
              hex("c206b8d9b9f0f37644430b490eeaa314"));
}

TEST(QuicCryptoTest, AppliesAndRemovesInitialHeaderProtection) {
    auto dcid = cid_from_hex("8394c8f03e515708");
    fiber::quic::QuicCryptoState state{};
    ASSERT_TRUE(fiber::quic::quic_init_initial_crypto(state, fiber::quic::QuicConnectionRole::Server, dcid));

    std::array<std::uint8_t, 64> datagram{};
    datagram[0] =
            fiber::quic::kPacketFlagLong | fiber::quic::kPacketFlagFixed | fiber::quic::kLongPacketTypeInitial | 0x03;
    for (std::size_t i = 1; i < datagram.size(); ++i) {
        datagram[i] = static_cast<std::uint8_t>(i);
    }

    constexpr std::size_t pn_offset = 18;
    const std::uint8_t original_flags = datagram[0];
    const std::array<std::uint8_t, 4> original_pn{
            datagram[pn_offset],
            datagram[pn_offset + 1],
            datagram[pn_offset + 2],
            datagram[pn_offset + 3],
    };

    fiber::quic::QuicPacketHeader packet{};
    packet.packet_data = datagram.data();
    packet.packet_len = datagram.size();
    packet.flags = original_flags;
    packet.long_header = true;
    packet.pn_len = 4;
    packet.protected_pn = datagram.data() + pn_offset;

    auto applied =
            fiber::quic::quic_apply_header_protection(packet, state.initial_read, datagram.data(), datagram.size());
    ASSERT_TRUE(applied.has_value());
    EXPECT_NE(datagram[0], original_flags);

    auto removed =
            fiber::quic::quic_remove_header_protection(packet, state.initial_read, datagram.data(), datagram.size());
    ASSERT_TRUE(removed.has_value());
    EXPECT_EQ(datagram[0], original_flags);
    EXPECT_EQ(datagram[pn_offset], original_pn[0]);
    EXPECT_EQ(datagram[pn_offset + 1], original_pn[1]);
    EXPECT_EQ(datagram[pn_offset + 2], original_pn[2]);
    EXPECT_EQ(datagram[pn_offset + 3], original_pn[3]);
}

TEST(QuicCryptoTest, DecryptsInitialPacketAndUpdatesOnlyInitialPacketNumberSpace) {
    fiber::quic::QuicConnection::Options options{};
    options.role = fiber::quic::QuicConnectionRole::Server;
    fiber::quic::QuicConnection connection(options);
    auto dcid = cid_from_hex("8394c8f03e515708");
    ASSERT_TRUE(connection.init_initial_crypto(dcid));

    constexpr std::array<std::uint8_t, 5> plaintext{'h', 'e', 'l', 'l', 'o'};
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> datagram{};
    fiber::quic::QuicPacketHeader packet{};
    std::uint8_t *pn = nullptr;
    build_initial_datagram(datagram, packet, &pn, plaintext.size());
    const std::size_t ciphertext_offset = static_cast<std::size_t>(pn + packet.pn_len - datagram.data());

    auto sealed = fiber::quic::quic_encrypt_packet_payload(packet, connection.crypto().initial_read, plaintext.data(),
                                                           plaintext.size(), pn + packet.pn_len,
                                                           datagram.size() - ciphertext_offset);
    ASSERT_TRUE(sealed.has_value());
    packet.packet_len = static_cast<std::size_t>(pn + packet.pn_len - datagram.data()) + *sealed;

    auto protected_header = fiber::quic::quic_apply_header_protection(packet, connection.crypto().initial_read,
                                                                      datagram.data(), packet.packet_len);
    ASSERT_TRUE(protected_header.has_value());

    auto parsed = fiber::quic::quic_parse_packet_header(datagram.data(), datagram.size(), 0);
    ASSERT_TRUE(parsed.has_value());

    std::array<std::uint8_t, 64> decrypted{};
    auto opened = fiber::quic::quic_decrypt_initial_packet(connection, *parsed, datagram.data(), packet.packet_len,
                                                           decrypted.data(), decrypted.size());

    ASSERT_TRUE(opened.has_value());
    ASSERT_EQ(opened->len, plaintext.size());
    EXPECT_TRUE(std::equal(plaintext.begin(), plaintext.end(), opened->data));
    EXPECT_EQ(connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial).largest_received_packet_number,
              2U);
    EXPECT_EQ(
            connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Handshake).largest_received_packet_number,
            fiber::quic::kUnsetPacketNumber);
    EXPECT_EQ(connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Application)
                      .largest_received_packet_number,
              fiber::quic::kUnsetPacketNumber);
}

TEST(QuicCryptoTest, FailedInitialDecryptDoesNotUpdatePacketNumberSpace) {
    fiber::quic::QuicConnection::Options options{};
    options.role = fiber::quic::QuicConnectionRole::Server;
    fiber::quic::QuicConnection connection(options);
    auto dcid = cid_from_hex("8394c8f03e515708");
    ASSERT_TRUE(connection.init_initial_crypto(dcid));

    constexpr std::array<std::uint8_t, 5> plaintext{'h', 'e', 'l', 'l', 'o'};
    std::array<std::uint8_t, fiber::quic::kMinInitialDatagramSize> datagram{};
    fiber::quic::QuicPacketHeader packet{};
    std::uint8_t *pn = nullptr;
    build_initial_datagram(datagram, packet, &pn, plaintext.size());
    const std::size_t ciphertext_offset = static_cast<std::size_t>(pn + packet.pn_len - datagram.data());

    auto sealed = fiber::quic::quic_encrypt_packet_payload(packet, connection.crypto().initial_read, plaintext.data(),
                                                           plaintext.size(), pn + packet.pn_len,
                                                           datagram.size() - ciphertext_offset);
    ASSERT_TRUE(sealed.has_value());
    packet.packet_len = static_cast<std::size_t>(pn + packet.pn_len - datagram.data()) + *sealed;
    ASSERT_TRUE(fiber::quic::quic_apply_header_protection(packet, connection.crypto().initial_read, datagram.data(),
                                                          packet.packet_len));
    datagram[packet.packet_len - 1] ^= 0x40;

    auto parsed = fiber::quic::quic_parse_packet_header(datagram.data(), datagram.size(), 0);
    ASSERT_TRUE(parsed.has_value());

    std::array<std::uint8_t, 64> decrypted{};
    auto opened = fiber::quic::quic_decrypt_initial_packet(connection, *parsed, datagram.data(), packet.packet_len,
                                                           decrypted.data(), decrypted.size());

    EXPECT_FALSE(opened.has_value());
    EXPECT_EQ(connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Initial).largest_received_packet_number,
              fiber::quic::kUnsetPacketNumber);
}
