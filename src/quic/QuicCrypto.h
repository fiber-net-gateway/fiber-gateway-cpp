#ifndef FIBER_QUIC_QUIC_CRYPTO_H
#define FIBER_QUIC_QUIC_CRYPTO_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "../common/IoError.h"
#include "QuicProtocol.h"

namespace fiber::quic {

struct QuicInitialSecrets {
    std::array<std::uint8_t, kQuicInitialSecretLength> initial{};
    std::array<std::uint8_t, kQuicInitialSecretLength> client{};
    std::array<std::uint8_t, kQuicInitialSecretLength> server{};
};

[[nodiscard]] common::IoResult<QuicInitialSecrets>
quic_derive_initial_secrets(const QuicConnectionId &original_dcid) noexcept;

[[nodiscard]] common::IoResult<void> quic_init_initial_crypto(QuicCryptoState &state, QuicConnectionRole role,
                                                              const QuicConnectionId &original_dcid) noexcept;

[[nodiscard]] common::IoResult<void> quic_create_retry_integrity_tag(const QuicConnectionId &original_dcid,
                                                                     const std::uint8_t *retry_packet,
                                                                     std::size_t retry_packet_len,
                                                                     std::uint8_t *tag_out,
                                                                     std::size_t tag_len) noexcept;

[[nodiscard]] common::IoResult<void>
quic_set_packet_protection_secret(QuicPacketProtectionKeys &keys, QuicCryptoSuite suite, const std::uint8_t *secret,
                                  std::size_t secret_len) noexcept;

[[nodiscard]] common::IoResult<void>
quic_set_encryption_secret(QuicCryptoState &state, QuicEncryptionLevel level, bool write_secret,
                           QuicCryptoSuite suite, const std::uint8_t *secret, std::size_t secret_len) noexcept;

[[nodiscard]] QuicPacketProtectionKeys *quic_packet_keys(QuicCryptoState &state, QuicEncryptionLevel level,
                                                         bool write_keys) noexcept;
[[nodiscard]] const QuicPacketProtectionKeys *quic_packet_keys(const QuicCryptoState &state,
                                                               QuicEncryptionLevel level, bool write_keys) noexcept;

[[nodiscard]] common::IoResult<void>
quic_header_protection_mask(const QuicPacketProtectionKeys &keys, const std::uint8_t *sample, std::size_t sample_len,
                            std::uint8_t (&mask)[kQuicHeaderProtectionMaskLength]) noexcept;

[[nodiscard]] common::IoResult<void> quic_apply_header_protection(QuicPacketHeader &packet,
                                                                  const QuicPacketProtectionKeys &keys,
                                                                  std::uint8_t *datagram,
                                                                  std::size_t datagram_len) noexcept;

[[nodiscard]] common::IoResult<void> quic_remove_header_protection(QuicPacketHeader &packet,
                                                                   const QuicPacketProtectionKeys &keys,
                                                                   std::uint8_t *datagram,
                                                                   std::size_t datagram_len) noexcept;

[[nodiscard]] common::IoResult<std::size_t> quic_encrypt_packet_payload(const QuicPacketHeader &packet,
                                                                        const QuicPacketProtectionKeys &keys,
                                                                        const std::uint8_t *plaintext,
                                                                        std::size_t plaintext_len, std::uint8_t *out,
                                                                        std::size_t out_cap) noexcept;

[[nodiscard]] common::IoResult<QuicSlice> quic_decrypt_packet_payload(QuicPacketHeader &packet,
                                                                      QuicPacketNumberSpace &space,
                                                                      const QuicPacketProtectionKeys &keys,
                                                                      std::uint8_t *datagram,
                                                                      std::size_t datagram_len,
                                                                      std::uint8_t *plaintext,
                                                                      std::size_t plaintext_cap) noexcept;

[[nodiscard]] common::IoResult<QuicSlice> quic_decrypt_initial_packet(QuicConnection &connection,
                                                                      QuicPacketHeader &packet,
                                                                      std::uint8_t *datagram,
                                                                      std::size_t datagram_len,
                                                                      std::uint8_t *plaintext,
                                                                      std::size_t plaintext_cap) noexcept;

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_CRYPTO_H
