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

[[nodiscard]] common::IoResult<void> quic_reinit_initial_crypto(QuicCryptoState &state, QuicConnectionRole role,
                                                                const QuicConnectionId &destination_cid) noexcept;

[[nodiscard]] common::IoResult<void>
quic_create_retry_integrity_tag(const QuicConnectionId &original_dcid, const std::uint8_t *retry_packet,
                                std::size_t retry_packet_len, std::uint8_t *tag_out, std::size_t tag_len) noexcept;

[[nodiscard]] common::IoResult<bool> quic_validate_retry_integrity_tag(const QuicConnectionId &original_dcid,
                                                                       const std::uint8_t *retry_packet,
                                                                       std::size_t retry_packet_len) noexcept;

[[nodiscard]] common::IoResult<void> quic_set_packet_protection_secret(QuicPacketProtectionKeyView keys,
                                                                       QuicCryptoSuite suite,
                                                                       const std::uint8_t *secret,
                                                                       std::size_t secret_len) noexcept;

[[nodiscard]] common::IoResult<void> quic_set_encryption_secret(QuicCryptoState &state, QuicEncryptionLevel level,
                                                                bool write_secret, QuicCryptoSuite suite,
                                                                const std::uint8_t *secret,
                                                                std::size_t secret_len) noexcept;

// Derive the next-generation Application keys from the current Application keys, per
// RFC 9001 §6.1 Algorithm 8. The Application secrets are extended via
// HKDF-Expand-Label("tls13 quic ku") and the resulting key/IV are installed into
// the next read/write slots. Application header-protection state is shared by
// every epoch because RFC 9001 §5.4.3 does not rotate HP keys on key update.
[[nodiscard]] common::IoResult<void> quic_derive_next_key_pair(QuicCryptoState &state) noexcept;

[[nodiscard]] QuicPacketProtectionKeyView quic_packet_keys(QuicCryptoState &state, QuicEncryptionLevel level,
                                                           bool write_keys) noexcept;

[[nodiscard]] common::IoResult<void>
quic_header_protection_mask(QuicPacketProtectionKeyView keys, const std::uint8_t *sample, std::size_t sample_len,
                            std::uint8_t (&mask)[kQuicHeaderProtectionMaskLength]) noexcept;

[[nodiscard]] common::IoResult<void> quic_apply_header_protection(QuicPacketHeader &packet,
                                                                  QuicPacketProtectionKeyView keys,
                                                                  std::uint8_t *datagram,
                                                                  std::size_t datagram_len) noexcept;

[[nodiscard]] common::IoResult<void> quic_remove_header_protection(QuicPacketHeader &packet,
                                                                   QuicPacketProtectionKeyView keys,
                                                                   std::uint8_t *datagram,
                                                                   std::size_t datagram_len) noexcept;

[[nodiscard]] common::IoResult<std::size_t> quic_encrypt_packet_payload(const QuicPacketHeader &packet,
                                                                        QuicPacketProtectionKeyView keys,
                                                                        const std::uint8_t *plaintext,
                                                                        std::size_t plaintext_len, std::uint8_t *out,
                                                                        std::size_t out_cap) noexcept;

[[nodiscard]] common::IoResult<QuicSlice>
quic_decrypt_packet_payload(QuicPacketHeader &packet, QuicPacketNumberSpace &space, QuicPacketProtectionKeyView keys,
                            std::uint8_t *datagram, std::size_t datagram_len, std::uint8_t *plaintext,
                            std::size_t plaintext_cap) noexcept;

// AEAD-only decrypt — assumes header protection has already been removed and the
// packet number has already been decoded into packet.packet_number and
// packet.ciphertext / packet.ciphertext_len. This is the lower half of the
// decrypt pipeline, separated so the key_phase bit can be inspected between HP
// removal and AEAD open (RFC 9001 §6.3).
[[nodiscard]] common::IoResult<QuicSlice> quic_decrypt_aead_payload(const QuicPacketHeader &packet,
                                                                    QuicPacketProtectionKeyView keys,
                                                                    std::uint8_t *plaintext,
                                                                    std::size_t plaintext_cap) noexcept;

[[nodiscard]] std::uint64_t quic_confidentiality_limit(QuicCryptoSuite suite) noexcept;
[[nodiscard]] std::uint64_t quic_integrity_limit(QuicCryptoSuite suite) noexcept;

[[nodiscard]] common::IoResult<QuicSlice> quic_decrypt_initial_packet(QuicConnection &connection,
                                                                      QuicPacketHeader &packet, std::uint8_t *datagram,
                                                                      std::size_t datagram_len, std::uint8_t *plaintext,
                                                                      std::size_t plaintext_cap) noexcept;

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_CRYPTO_H
