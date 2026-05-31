#include "QuicCrypto.h"

#include <algorithm>
#include <cstring>

#include <openssl/aead.h>
#include <openssl/evp.h>
#include <openssl/hkdf.h>

#include "QuicTransportCodec.h"

namespace fiber::quic {

namespace {

constexpr std::uint8_t kQuicV1InitialSalt[] = {
        0x38, 0x76, 0x2c, 0xf7, 0xf5, 0x59, 0x34, 0xb3, 0x4d, 0x17,
        0x9a, 0xe6, 0xa4, 0xc8, 0x0c, 0xad, 0xcc, 0xbb, 0x7f, 0x0a,
};

constexpr char kTls13LabelPrefix[] = "tls13 ";
constexpr char kClientInitialLabel[] = "client in";
constexpr char kServerInitialLabel[] = "server in";
constexpr char kQuicKeyLabel[] = "quic key";
constexpr char kQuicIvLabel[] = "quic iv";
constexpr char kQuicHeaderProtectionLabel[] = "quic hp";

[[nodiscard]] common::IoResult<void> hkdf_expand_label(std::uint8_t *out, std::size_t out_len,
                                                       const std::uint8_t *secret, std::size_t secret_len,
                                                       const char *label, const std::uint8_t *context,
                                                       std::size_t context_len) noexcept {
    if ((out == nullptr && out_len != 0) || (secret == nullptr && secret_len != 0) || label == nullptr ||
        (context == nullptr && context_len != 0) || out_len > 0xffffU || context_len > 0xffU) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::size_t label_len = std::strlen(label);
    constexpr std::size_t prefix_len = sizeof(kTls13LabelPrefix) - 1;
    if (prefix_len + label_len > 0xffU) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::uint8_t info[2 + 1 + prefix_len + 32 + 1 + 32]{};
    std::size_t offset = 0;
    info[offset++] = static_cast<std::uint8_t>((out_len >> 8U) & 0xffU);
    info[offset++] = static_cast<std::uint8_t>(out_len & 0xffU);
    info[offset++] = static_cast<std::uint8_t>(prefix_len + label_len);
    std::memcpy(info + offset, kTls13LabelPrefix, prefix_len);
    offset += prefix_len;
    std::memcpy(info + offset, label, label_len);
    offset += label_len;
    info[offset++] = static_cast<std::uint8_t>(context_len);
    if (context_len != 0) {
        std::memcpy(info + offset, context, context_len);
        offset += context_len;
    }

    if (!HKDF_expand(out, out_len, EVP_sha256(), secret, secret_len, info, offset)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
}

[[nodiscard]] common::IoResult<void>
derive_packet_protection_keys(QuicPacketProtectionKeys &keys,
                              const std::array<std::uint8_t, kQuicInitialSecretLength> &secret) noexcept {
    keys.reset();
    keys.suite = QuicCryptoSuite::InitialAes128GcmSha256;

    auto derived = hkdf_expand_label(keys.key.data(), keys.key.size(), secret.data(), secret.size(), kQuicKeyLabel,
                                     nullptr, 0);
    if (!derived) {
        return std::unexpected(derived.error());
    }
    derived = hkdf_expand_label(keys.iv.data(), keys.iv.size(), secret.data(), secret.size(), kQuicIvLabel, nullptr, 0);
    if (!derived) {
        return std::unexpected(derived.error());
    }
    derived = hkdf_expand_label(keys.hp.data(), keys.hp.size(), secret.data(), secret.size(),
                                kQuicHeaderProtectionLabel, nullptr, 0);
    if (!derived) {
        return std::unexpected(derived.error());
    }

    if (!EVP_AEAD_CTX_init(&keys.aead, EVP_aead_aes_128_gcm(), keys.key.data(), keys.key.size(),
                           EVP_AEAD_DEFAULT_TAG_LENGTH, nullptr)) {
        keys.reset();
        return std::unexpected(common::IoErr::Invalid);
    }
    keys.aead_initialized = true;

    if (AES_set_encrypt_key(keys.hp.data(), static_cast<unsigned>(keys.hp.size() * 8U), &keys.hp_key) != 0) {
        keys.reset();
        return std::unexpected(common::IoErr::Invalid);
    }

    keys.ready = true;
    return {};
}

[[nodiscard]] common::IoResult<std::size_t> packet_number_offset(const QuicPacketHeader &packet,
                                                                 const std::uint8_t *datagram,
                                                                 std::size_t datagram_len) noexcept {
    if (datagram == nullptr || packet.packet_data == nullptr || packet.protected_pn == nullptr ||
        packet.packet_data != datagram || packet.protected_pn < packet.packet_data) {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::size_t offset = static_cast<std::size_t>(packet.protected_pn - packet.packet_data);
    if (offset >= datagram_len) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return offset;
}

[[nodiscard]] common::IoResult<void> make_packet_nonce(const QuicPacketProtectionKeys &keys,
                                                       std::uint64_t packet_number,
                                                       std::uint8_t (&nonce)[kQuicInitialIvLength]) noexcept {
    if (!keys.ready) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::memcpy(nonce, keys.iv.data(), keys.iv.size());
    for (std::size_t i = 0; i < 8; ++i) {
        nonce[kQuicInitialIvLength - 1 - i] ^=
                static_cast<std::uint8_t>((packet_number >> static_cast<unsigned>(i * 8U)) & 0xffU);
    }
    return {};
}

[[nodiscard]] common::IoResult<std::size_t> aead_header_len(const QuicPacketHeader &packet) noexcept {
    if (packet.packet_data == nullptr || packet.protected_pn == nullptr || packet.pn_len == 0 || packet.pn_len > 4 ||
        packet.protected_pn < packet.packet_data) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return static_cast<std::size_t>(packet.protected_pn - packet.packet_data) + packet.pn_len;
}

} // namespace

common::IoResult<QuicInitialSecrets> quic_derive_initial_secrets(const QuicConnectionId &original_dcid) noexcept {
    QuicInitialSecrets secrets{};
    std::size_t secret_len = 0;
    if (!HKDF_extract(secrets.initial.data(), &secret_len, EVP_sha256(), original_dcid.data(), original_dcid.size(),
                      kQuicV1InitialSalt, sizeof(kQuicV1InitialSalt)) ||
        secret_len != secrets.initial.size()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    auto expanded = hkdf_expand_label(secrets.client.data(), secrets.client.size(), secrets.initial.data(),
                                      secrets.initial.size(), kClientInitialLabel, nullptr, 0);
    if (!expanded) {
        return std::unexpected(expanded.error());
    }
    expanded = hkdf_expand_label(secrets.server.data(), secrets.server.size(), secrets.initial.data(),
                                 secrets.initial.size(), kServerInitialLabel, nullptr, 0);
    if (!expanded) {
        return std::unexpected(expanded.error());
    }
    return secrets;
}

common::IoResult<void> quic_init_initial_crypto(QuicCryptoState &state, QuicConnectionRole role,
                                                const QuicConnectionId &original_dcid) noexcept {
    auto secrets = quic_derive_initial_secrets(original_dcid);
    if (!secrets) {
        return std::unexpected(secrets.error());
    }

    state.reset();
    auto derived = derive_packet_protection_keys(state.initial_read, role == QuicConnectionRole::Server
                                                                             ? secrets->client
                                                                             : secrets->server);
    if (!derived) {
        return std::unexpected(derived.error());
    }
    derived = derive_packet_protection_keys(state.initial_write, role == QuicConnectionRole::Server ? secrets->server
                                                                                                   : secrets->client);
    if (!derived) {
        return std::unexpected(derived.error());
    }

    state.initial_ready = true;
    return {};
}

common::IoResult<void> quic_header_protection_mask(const QuicPacketProtectionKeys &keys, const std::uint8_t *sample,
                                                   std::size_t sample_len,
                                                   std::uint8_t (&mask)[kQuicHeaderProtectionMaskLength]) noexcept {
    if (!keys.ready || sample == nullptr || sample_len < kQuicHeaderProtectionSampleLength) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::uint8_t encrypted[AES_BLOCK_SIZE]{};
    AES_encrypt(sample, encrypted, &keys.hp_key);
    std::memcpy(mask, encrypted, kQuicHeaderProtectionMaskLength);
    return {};
}

common::IoResult<void> quic_apply_header_protection(QuicPacketHeader &packet, const QuicPacketProtectionKeys &keys,
                                                    std::uint8_t *datagram, std::size_t datagram_len) noexcept {
    auto pn_offset = packet_number_offset(packet, datagram, datagram_len);
    if (!pn_offset) {
        return std::unexpected(pn_offset.error());
    }
    const std::size_t sample_offset = *pn_offset + 4;
    if (sample_offset > datagram_len || datagram_len - sample_offset < kQuicHeaderProtectionSampleLength ||
        packet.pn_len == 0 || packet.pn_len > 4 || datagram_len - *pn_offset < packet.pn_len) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::uint8_t mask[kQuicHeaderProtectionMaskLength]{};
    auto masked = quic_header_protection_mask(keys, datagram + sample_offset, datagram_len - sample_offset, mask);
    if (!masked) {
        return std::unexpected(masked.error());
    }

    const std::uint8_t flag_mask = packet.long_header ? 0x0fU : 0x1fU;
    datagram[0] ^= static_cast<std::uint8_t>(mask[0] & flag_mask);
    for (std::uint8_t i = 0; i < packet.pn_len; ++i) {
        datagram[*pn_offset + i] ^= mask[1 + i];
    }
    packet.protected_flags = datagram[0];
    packet.flags = datagram[0];
    return {};
}

common::IoResult<void> quic_remove_header_protection(QuicPacketHeader &packet, const QuicPacketProtectionKeys &keys,
                                                     std::uint8_t *datagram, std::size_t datagram_len) noexcept {
    auto pn_offset = packet_number_offset(packet, datagram, datagram_len);
    if (!pn_offset) {
        return std::unexpected(pn_offset.error());
    }
    const std::size_t sample_offset = *pn_offset + 4;
    if (sample_offset > datagram_len || datagram_len - sample_offset < kQuicHeaderProtectionSampleLength) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::uint8_t mask[kQuicHeaderProtectionMaskLength]{};
    auto masked = quic_header_protection_mask(keys, datagram + sample_offset, datagram_len - sample_offset, mask);
    if (!masked) {
        return std::unexpected(masked.error());
    }

    packet.protected_flags = datagram[0];
    const std::uint8_t flag_mask = packet.long_header ? 0x0fU : 0x1fU;
    datagram[0] ^= static_cast<std::uint8_t>(mask[0] & flag_mask);
    packet.flags = datagram[0];
    const std::uint8_t pn_len = static_cast<std::uint8_t>((packet.flags & kPacketFlagPnLengthMask) + 1);
    if (datagram_len - *pn_offset < pn_len) {
        return std::unexpected(common::IoErr::Invalid);
    }
    for (std::uint8_t i = 0; i < pn_len; ++i) {
        datagram[*pn_offset + i] ^= mask[1 + i];
    }
    return {};
}

common::IoResult<std::size_t> quic_encrypt_packet_payload(const QuicPacketHeader &packet,
                                                          const QuicPacketProtectionKeys &keys,
                                                          const std::uint8_t *plaintext, std::size_t plaintext_len,
                                                          std::uint8_t *out, std::size_t out_cap) noexcept {
    if (!keys.ready || (plaintext == nullptr && plaintext_len != 0) || out == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }

    auto header_len = aead_header_len(packet);
    if (!header_len) {
        return std::unexpected(header_len.error());
    }
    std::uint8_t nonce[kQuicInitialIvLength]{};
    auto made_nonce = make_packet_nonce(keys, packet.packet_number, nonce);
    if (!made_nonce) {
        return std::unexpected(made_nonce.error());
    }

    std::size_t out_len = 0;
    if (!EVP_AEAD_CTX_seal(&keys.aead, out, &out_len, out_cap, nonce, sizeof(nonce), plaintext, plaintext_len,
                           packet.packet_data, *header_len)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return out_len;
}

common::IoResult<QuicSlice> quic_decrypt_packet_payload(QuicPacketHeader &packet, QuicPacketNumberSpace &space,
                                                        const QuicPacketProtectionKeys &keys, std::uint8_t *datagram,
                                                        std::size_t datagram_len, std::uint8_t *plaintext,
                                                        std::size_t plaintext_cap) noexcept {
    if (!keys.ready || plaintext == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }

    auto unprotected = quic_remove_header_protection(packet, keys, datagram, datagram_len);
    if (!unprotected) {
        return std::unexpected(unprotected.error());
    }
    auto read_pn = quic_read_packet_number(packet, space);
    if (!read_pn) {
        return std::unexpected(read_pn.error());
    }

    auto header_len = aead_header_len(packet);
    if (!header_len || *header_len > datagram_len || packet.ciphertext == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::uint8_t nonce[kQuicInitialIvLength]{};
    auto made_nonce = make_packet_nonce(keys, packet.packet_number, nonce);
    if (!made_nonce) {
        return std::unexpected(made_nonce.error());
    }

    std::size_t plaintext_len = 0;
    if (!EVP_AEAD_CTX_open(&keys.aead, plaintext, &plaintext_len, plaintext_cap, nonce, sizeof(nonce),
                           packet.ciphertext, packet.ciphertext_len, datagram, *header_len)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    space.record_received_packet_number(packet.packet_number);
    return QuicSlice{plaintext, plaintext_len};
}

common::IoResult<QuicSlice> quic_decrypt_initial_packet(QuicConnection &connection, QuicPacketHeader &packet,
                                                        std::uint8_t *datagram, std::size_t datagram_len,
                                                        std::uint8_t *plaintext,
                                                        std::size_t plaintext_cap) noexcept {
    if (packet.level != QuicEncryptionLevel::Initial || !connection.crypto().initial_ready) {
        return std::unexpected(common::IoErr::Invalid);
    }
    QuicPacketNumberSpace &space = connection.packet_number_space(packet.level);
    return quic_decrypt_packet_payload(packet, space, connection.crypto().initial_read, datagram, datagram_len,
                                       plaintext, plaintext_cap);
}

} // namespace fiber::quic
