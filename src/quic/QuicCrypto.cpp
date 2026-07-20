#include "QuicCrypto.h"

#include <algorithm>
#include <cstring>

#include <openssl/aead.h>
#include <openssl/chacha.h>
#include <openssl/evp.h>
#include <openssl/hkdf.h>
#include <openssl/mem.h>

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
constexpr char kQuicKeyUpdateLabel[] = "quic ku";

constexpr std::uint8_t kRetryIntegrityKey[] = {
        0xbe, 0x0c, 0x69, 0x0b, 0x9f, 0x66, 0x57, 0x5a, 0x1d, 0x76, 0x6b, 0x54, 0xe3, 0x68, 0xc8, 0x4e,
};

constexpr std::uint8_t kRetryIntegrityNonce[] = {
        0x46, 0x15, 0x99, 0xd3, 0x5d, 0x63, 0x2b, 0xf2, 0x23, 0x98, 0x25, 0xbb,
};

struct SuiteSpec {
    const EVP_AEAD *aead = nullptr;
    const EVP_MD *digest = nullptr;
    std::size_t key_len = 0;
    std::size_t secret_len = 0;
    std::size_t hp_len = 0;
    bool hp_chacha20 = false;
};

[[nodiscard]] SuiteSpec suite_spec(QuicCryptoSuite suite) noexcept {
    switch (suite) {
        case QuicCryptoSuite::InitialAes128GcmSha256:
        case QuicCryptoSuite::Aes128GcmSha256:
            return {EVP_aead_aes_128_gcm(), EVP_sha256(), 16, 32, 16, false};
        case QuicCryptoSuite::Aes256GcmSha384:
            return {EVP_aead_aes_256_gcm(), EVP_sha384(), 32, 48, 32, false};
        case QuicCryptoSuite::ChaCha20Poly1305Sha256:
            return {EVP_aead_chacha20_poly1305(), EVP_sha256(), 32, 32, 32, true};
    }
    return {};
}

[[nodiscard]] common::IoResult<void> hkdf_expand_label(const EVP_MD *digest, std::uint8_t *out, std::size_t out_len,
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

    if (!HKDF_expand(out, out_len, digest, secret, secret_len, info, offset)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
}

[[nodiscard]] common::IoResult<void>
derive_packet_protection_keys(QuicPacketProtectionKeyView keys,
                              const std::array<std::uint8_t, kQuicInitialSecretLength> &secret) noexcept {
    return quic_set_packet_protection_secret(keys, QuicCryptoSuite::InitialAes128GcmSha256, secret.data(),
                                             secret.size());
}

[[nodiscard]] common::IoResult<void> init_aes_header_protection(QuicHeaderProtectionKeys &keys) noexcept {
    if (AES_set_encrypt_key(keys.key.data(), static_cast<unsigned>(keys.key_len * 8U), &keys.aes_key) != 0) {
        keys.reset();
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
}

[[nodiscard]] common::IoResult<void> derive_packet_protection_keys_from_secret(QuicPacketProtectionKeyView keys,
                                                                               QuicCryptoSuite suite,
                                                                               const std::uint8_t *secret,
                                                                               std::size_t secret_len) noexcept {
    const SuiteSpec spec = suite_spec(suite);
    if (!keys || spec.aead == nullptr || spec.digest == nullptr || secret == nullptr || secret_len != spec.secret_len) {
        return std::unexpected(common::IoErr::Invalid);
    }

    keys.packet->reset();
    keys.header->reset();
    keys.packet->suite = suite;
    keys.packet->secret_len = secret_len;
    keys.packet->iv_len = kQuicIvLength;
    keys.header->key_len = spec.hp_len;
    keys.header->chacha20 = spec.hp_chacha20;
    std::memcpy(keys.packet->secret.data(), secret, secret_len);

    std::array<std::uint8_t, kQuicMaxKeyLength> packet_key{};
    auto derived = hkdf_expand_label(spec.digest, packet_key.data(), spec.key_len, secret, secret_len, kQuicKeyLabel,
                                     nullptr, 0);
    if (!derived) {
        OPENSSL_cleanse(packet_key.data(), packet_key.size());
        keys.packet->reset();
        keys.header->reset();
        return std::unexpected(derived.error());
    }
    derived = hkdf_expand_label(spec.digest, keys.packet->iv.data(), keys.packet->iv_len, secret, secret_len,
                                kQuicIvLabel, nullptr, 0);
    if (!derived) {
        OPENSSL_cleanse(packet_key.data(), packet_key.size());
        keys.packet->reset();
        keys.header->reset();
        return std::unexpected(derived.error());
    }
    derived = hkdf_expand_label(spec.digest, keys.header->key.data(), keys.header->key_len, secret, secret_len,
                                kQuicHeaderProtectionLabel, nullptr, 0);
    if (!derived) {
        OPENSSL_cleanse(packet_key.data(), packet_key.size());
        keys.packet->reset();
        keys.header->reset();
        return std::unexpected(derived.error());
    }

    if (!EVP_AEAD_CTX_init(&keys.packet->aead, spec.aead, packet_key.data(), spec.key_len, EVP_AEAD_DEFAULT_TAG_LENGTH,
                           nullptr)) {
        OPENSSL_cleanse(packet_key.data(), packet_key.size());
        keys.packet->reset();
        keys.header->reset();
        return std::unexpected(common::IoErr::Invalid);
    }
    OPENSSL_cleanse(packet_key.data(), packet_key.size());
    keys.packet->aead_initialized = true;

    if (!keys.header->chacha20) {
        auto hp = init_aes_header_protection(*keys.header);
        if (!hp) {
            keys.packet->reset();
            return std::unexpected(hp.error());
        }
    }

    keys.header->ready = true;
    keys.packet->ready = true;
    return {};
}

[[nodiscard]] common::IoResult<std::size_t>
packet_number_offset(const QuicPacketHeader &packet, const std::uint8_t *datagram, std::size_t datagram_len) noexcept {
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

    std::memcpy(nonce, keys.iv.data(), keys.iv_len);
    for (std::size_t i = 0; i < 8; ++i) {
        nonce[kQuicIvLength - 1 - i] ^=
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

    auto expanded = hkdf_expand_label(EVP_sha256(), secrets.client.data(), secrets.client.size(),
                                      secrets.initial.data(), secrets.initial.size(), kClientInitialLabel, nullptr, 0);
    if (!expanded) {
        return std::unexpected(expanded.error());
    }
    expanded = hkdf_expand_label(EVP_sha256(), secrets.server.data(), secrets.server.size(), secrets.initial.data(),
                                 secrets.initial.size(), kServerInitialLabel, nullptr, 0);
    if (!expanded) {
        return std::unexpected(expanded.error());
    }
    return secrets;
}

common::IoResult<void> quic_init_initial_crypto(QuicCryptoState &state, QuicConnectionRole role,
                                                const QuicConnectionId &original_dcid) noexcept {
    if (state.initial_discarded()) {
        return std::unexpected(common::IoErr::NotFound);
    }
    state.reset();
    return quic_reinit_initial_crypto(state, role, original_dcid);
}

common::IoResult<void> quic_reinit_initial_crypto(QuicCryptoState &state, QuicConnectionRole role,
                                                  const QuicConnectionId &destination_cid) noexcept {
    if (destination_cid.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    auto secrets = quic_derive_initial_secrets(destination_cid);
    if (!secrets) {
        return std::unexpected(secrets.error());
    }

    auto allocated = state.reset_initial_keys();
    if (!allocated) {
        return std::unexpected(allocated.error());
    }
    auto derived = derive_packet_protection_keys(
            state.initial_read(), role == QuicConnectionRole::Server ? secrets->client : secrets->server);
    if (!derived) {
        (void) state.reset_initial_keys();
        return std::unexpected(derived.error());
    }
    derived = derive_packet_protection_keys(state.initial_write(),
                                            role == QuicConnectionRole::Server ? secrets->server : secrets->client);
    if (!derived) {
        (void) state.reset_initial_keys();
        return std::unexpected(derived.error());
    }
    return {};
}

common::IoResult<void> quic_create_retry_integrity_tag(const QuicConnectionId &original_dcid,
                                                       const std::uint8_t *retry_packet, std::size_t retry_packet_len,
                                                       std::uint8_t *tag_out, std::size_t tag_len) noexcept {
    if ((retry_packet == nullptr && retry_packet_len != 0) || tag_out == nullptr || tag_len != kAeadTagLength ||
        original_dcid.size() > kMaxConnectionIdLength) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::uint8_t pseudo_packet[1 + kMaxConnectionIdLength + 1500]{};
    if (retry_packet_len > sizeof(pseudo_packet) - 1 - original_dcid.size()) {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }
    std::size_t offset = 0;
    pseudo_packet[offset++] = original_dcid.length;
    if (!original_dcid.empty()) {
        std::memcpy(pseudo_packet + offset, original_dcid.data(), original_dcid.size());
        offset += original_dcid.size();
    }
    if (retry_packet_len != 0) {
        std::memcpy(pseudo_packet + offset, retry_packet, retry_packet_len);
        offset += retry_packet_len;
    }

    EVP_AEAD_CTX ctx;
    EVP_AEAD_CTX_zero(&ctx);
    if (!EVP_AEAD_CTX_init(&ctx, EVP_aead_aes_128_gcm(), kRetryIntegrityKey, sizeof(kRetryIntegrityKey),
                           EVP_AEAD_DEFAULT_TAG_LENGTH, nullptr)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::size_t out_len = tag_len;
    const int ok = EVP_AEAD_CTX_seal(&ctx, tag_out, &out_len, tag_len, kRetryIntegrityNonce,
                                     sizeof(kRetryIntegrityNonce), nullptr, 0, pseudo_packet, offset);
    EVP_AEAD_CTX_cleanup(&ctx);
    if (!ok || out_len != tag_len) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
}

common::IoResult<bool> quic_validate_retry_integrity_tag(const QuicConnectionId &original_dcid,
                                                         const std::uint8_t *retry_packet,
                                                         std::size_t retry_packet_len) noexcept {
    if (retry_packet == nullptr || retry_packet_len < kAeadTagLength) {
        return std::unexpected(common::IoErr::Invalid);
    }
    std::uint8_t expected[kAeadTagLength]{};
    auto created = quic_create_retry_integrity_tag(original_dcid, retry_packet, retry_packet_len - kAeadTagLength,
                                                   expected, sizeof(expected));
    if (!created) {
        return std::unexpected(created.error());
    }
    return CRYPTO_memcmp(expected, retry_packet + retry_packet_len - kAeadTagLength, sizeof(expected)) == 0;
}

common::IoResult<void> quic_set_packet_protection_secret(QuicPacketProtectionKeyView keys, QuicCryptoSuite suite,
                                                         const std::uint8_t *secret, std::size_t secret_len) noexcept {
    return derive_packet_protection_keys_from_secret(keys, suite, secret, secret_len);
}

common::IoResult<void> quic_set_encryption_secret(QuicCryptoState &state, QuicEncryptionLevel level, bool write_secret,
                                                  QuicCryptoSuite suite, const std::uint8_t *secret,
                                                  std::size_t secret_len) noexcept {
    if (level == QuicEncryptionLevel::Initial) {
        return std::unexpected(common::IoErr::Invalid);
    }
    auto allocated = level == QuicEncryptionLevel::Application ? state.ensure_application() : state.ensure_transient();
    if (!allocated) {
        return std::unexpected(allocated.error());
    }
    auto keys = quic_packet_keys(state, level, write_secret);
    if (!keys) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return quic_set_packet_protection_secret(keys, suite, secret, secret_len);
}

namespace {

[[nodiscard]] common::IoResult<void> derive_next_keys(const QuicPacketProtectionKeys &current,
                                                      QuicPacketProtectionKeys &next) noexcept {
    if (!current.ready || current.secret_len == 0) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const SuiteSpec spec = suite_spec(current.suite);
    if (spec.aead == nullptr || spec.digest == nullptr || current.secret_len != spec.secret_len) {
        return std::unexpected(common::IoErr::Invalid);
    }

    next.reset();
    next.suite = current.suite;
    next.secret_len = current.secret_len;
    next.iv_len = current.iv_len;

    auto derived = hkdf_expand_label(spec.digest, next.secret.data(), next.secret_len, current.secret.data(),
                                     current.secret_len, kQuicKeyUpdateLabel, nullptr, 0);
    if (!derived) {
        next.reset();
        return std::unexpected(derived.error());
    }
    std::array<std::uint8_t, kQuicMaxKeyLength> packet_key{};
    derived = hkdf_expand_label(spec.digest, packet_key.data(), spec.key_len, next.secret.data(), next.secret_len,
                                kQuicKeyLabel, nullptr, 0);
    if (!derived) {
        OPENSSL_cleanse(packet_key.data(), packet_key.size());
        next.reset();
        return std::unexpected(derived.error());
    }
    derived = hkdf_expand_label(spec.digest, next.iv.data(), next.iv_len, next.secret.data(), next.secret_len,
                                kQuicIvLabel, nullptr, 0);
    if (!derived) {
        OPENSSL_cleanse(packet_key.data(), packet_key.size());
        next.reset();
        return std::unexpected(derived.error());
    }

    if (!EVP_AEAD_CTX_init(&next.aead, spec.aead, packet_key.data(), spec.key_len, EVP_AEAD_DEFAULT_TAG_LENGTH,
                           nullptr)) {
        OPENSSL_cleanse(packet_key.data(), packet_key.size());
        next.reset();
        return std::unexpected(common::IoErr::Invalid);
    }
    OPENSSL_cleanse(packet_key.data(), packet_key.size());
    next.aead_initialized = true;
    next.ready = true;
    return {};
}

} // namespace

common::IoResult<void> quic_derive_next_key_pair(QuicCryptoState &state) noexcept {
    auto current_read = state.application_read();
    auto current_write = state.application_write();
    auto next_read = state.next_application_read();
    auto next_write = state.next_application_write();
    if (!current_read.ready() || !current_write.ready() || !next_read || !next_write) {
        return std::unexpected(common::IoErr::Invalid);
    }
    auto derived_read = derive_next_keys(*current_read.packet, *next_read.packet);
    if (!derived_read) {
        next_read.packet->reset();
        return std::unexpected(derived_read.error());
    }
    auto derived_write = derive_next_keys(*current_write.packet, *next_write.packet);
    if (!derived_write) {
        next_read.packet->reset();
        next_write.packet->reset();
        return std::unexpected(derived_write.error());
    }
    return {};
}

QuicPacketProtectionKeyView quic_packet_keys(QuicCryptoState &state, QuicEncryptionLevel level,
                                             bool write_keys) noexcept {
    switch (level) {
        case QuicEncryptionLevel::Initial:
            return write_keys ? state.initial_write() : state.initial_read();
        case QuicEncryptionLevel::EarlyData:
            return write_keys ? state.early_write() : state.early_read();
        case QuicEncryptionLevel::Handshake:
            return write_keys ? state.handshake_write() : state.handshake_read();
        case QuicEncryptionLevel::Application:
            return write_keys ? state.application_write() : state.application_read();
    }
    return {};
}

common::IoResult<void> quic_header_protection_mask(QuicPacketProtectionKeyView keys, const std::uint8_t *sample,
                                                   std::size_t sample_len,
                                                   std::uint8_t (&mask)[kQuicHeaderProtectionMaskLength]) noexcept {
    if (!keys || !keys.header->ready || sample == nullptr || sample_len < kQuicHeaderProtectionSampleLength) {
        return std::unexpected(common::IoErr::Invalid);
    }

    if (keys.header->chacha20) {
        std::uint32_t counter = 0;
        std::memcpy(&counter, sample, sizeof(counter));
        static constexpr std::uint8_t kZeros[kQuicHeaderProtectionMaskLength]{};
        CRYPTO_chacha_20(mask, kZeros, kQuicHeaderProtectionMaskLength, keys.header->key.data(), sample + 4, counter);
        return {};
    }

    std::uint8_t encrypted[AES_BLOCK_SIZE]{};
    AES_encrypt(sample, encrypted, &keys.header->aes_key);
    std::memcpy(mask, encrypted, kQuicHeaderProtectionMaskLength);
    return {};
}

common::IoResult<void> quic_apply_header_protection(QuicPacketHeader &packet, QuicPacketProtectionKeyView keys,
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

common::IoResult<void> quic_remove_header_protection(QuicPacketHeader &packet, QuicPacketProtectionKeyView keys,
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
                                                          QuicPacketProtectionKeyView keys,
                                                          const std::uint8_t *plaintext, std::size_t plaintext_len,
                                                          std::uint8_t *out, std::size_t out_cap) noexcept {
    if (!keys || !keys.packet->ready || (plaintext == nullptr && plaintext_len != 0) || out == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }

    auto header_len = aead_header_len(packet);
    if (!header_len) {
        return std::unexpected(header_len.error());
    }
    std::uint8_t nonce[kQuicInitialIvLength]{};
    auto made_nonce = make_packet_nonce(*keys.packet, packet.packet_number, nonce);
    if (!made_nonce) {
        return std::unexpected(made_nonce.error());
    }

    std::size_t out_len = 0;
    if (!EVP_AEAD_CTX_seal(&keys.packet->aead, out, &out_len, out_cap, nonce, sizeof(nonce), plaintext, plaintext_len,
                           packet.packet_data, *header_len)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return out_len;
}

common::IoResult<QuicSlice> quic_decrypt_aead_payload(const QuicPacketHeader &packet, QuicPacketProtectionKeyView keys,
                                                      std::uint8_t *plaintext, std::size_t plaintext_cap) noexcept {
    if (!keys || !keys.packet->ready || plaintext == nullptr || packet.packet_data == nullptr ||
        packet.ciphertext == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }

    auto header_len = aead_header_len(packet);
    if (!header_len) {
        return std::unexpected(header_len.error());
    }

    std::uint8_t nonce[kQuicInitialIvLength]{};
    auto made_nonce = make_packet_nonce(*keys.packet, packet.packet_number, nonce);
    if (!made_nonce) {
        return std::unexpected(made_nonce.error());
    }

    std::size_t plaintext_len = 0;
    if (!EVP_AEAD_CTX_open(&keys.packet->aead, plaintext, &plaintext_len, plaintext_cap, nonce, sizeof(nonce),
                           packet.ciphertext, packet.ciphertext_len, packet.packet_data, *header_len)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return QuicSlice{plaintext, plaintext_len};
}

std::uint64_t quic_confidentiality_limit(QuicCryptoSuite suite) noexcept {
    switch (suite) {
        case QuicCryptoSuite::InitialAes128GcmSha256:
        case QuicCryptoSuite::Aes128GcmSha256:
        case QuicCryptoSuite::Aes256GcmSha384:
            return std::uint64_t{1} << 23U;
        case QuicCryptoSuite::ChaCha20Poly1305Sha256:
            return std::uint64_t{1} << 62U;
    }
    return 0;
}

std::uint64_t quic_integrity_limit(QuicCryptoSuite suite) noexcept {
    switch (suite) {
        case QuicCryptoSuite::InitialAes128GcmSha256:
        case QuicCryptoSuite::Aes128GcmSha256:
        case QuicCryptoSuite::Aes256GcmSha384:
            return std::uint64_t{1} << 52U;
        case QuicCryptoSuite::ChaCha20Poly1305Sha256:
            return std::uint64_t{1} << 36U;
    }
    return 0;
}

common::IoResult<QuicSlice> quic_decrypt_packet_payload(QuicPacketHeader &packet, QuicPacketNumberSpace &space,
                                                        QuicPacketProtectionKeyView keys, std::uint8_t *datagram,
                                                        std::size_t datagram_len, std::uint8_t *plaintext,
                                                        std::size_t plaintext_cap) noexcept {
    if (!keys.ready() || plaintext == nullptr) {
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

    auto opened = quic_decrypt_aead_payload(packet, keys, plaintext, plaintext_cap);
    if (!opened) {
        return std::unexpected(opened.error());
    }

    space.record_received_packet_number(packet.packet_number);
    return *opened;
}

common::IoResult<QuicSlice> quic_decrypt_initial_packet(QuicConnection &connection, QuicPacketHeader &packet,
                                                        std::uint8_t *datagram, std::size_t datagram_len,
                                                        std::uint8_t *plaintext, std::size_t plaintext_cap) noexcept {
    if (packet.level != QuicEncryptionLevel::Initial || !connection.crypto().initial_ready()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    QuicPacketNumberSpace &space = connection.packet_number_space(packet.level);
    return quic_decrypt_packet_payload(packet, space, connection.crypto().initial_read(), datagram, datagram_len,
                                       plaintext, plaintext_cap);
}

} // namespace fiber::quic
